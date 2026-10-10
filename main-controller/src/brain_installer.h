#pragma once

#include "BoardCommissioning.h"
#include "BoardInstall.h"
#include "BoardExportTransfer.h"
#include "BoardMaintenance.h"
#include <cstring>
#include <memory>
#include <new>

namespace babytech { namespace brain {

enum class BrainInstallStage { Idle, Discovering, Reserving, Reading, Installing,
                               Verifying, Persisted, Failed };

// The UI loop owns the installer and transport. The template also lets host
// tests connect the same production channels without a second UART owner.
// Persisted is durable evidence, not runtime activation or motion permission.
template<class Link>
class BrainInstaller : private boardlink::CommissioningGuard {
public:
    BrainInstaller(Link& link, boardlink::BrainStateStore& store,
                   bool (*localMaintenance)(), uint32_t (*nowMs)(),
                   bool (*newEpoch)(char (&)[33]))
        : link_(link), store_(store), localMaintenance_(localMaintenance),
          nowMs_(nowMs), newEpoch_(newEpoch) {}

    bool start(const char* device, bool handoffConfirmed, uint32_t nowMs) {
        if (busy() || stage_ == BrainInstallStage::Persisted || !handoffConfirmed ||
            !localMaintenance_ || !localMaintenance_() || !nowMs_ || !newEpoch_ ||
            !validDevice(device) || importer_.faulted() || store_.faulted() ||
            (writesRequested_ && std::strcmp(device, request_.pairing.deviceId))) return false;
        if (!link_.requestDiscovery(device, nowMs)) return false;
        std::strcpy(device_, device);
        resuming_ = writesRequested_;
        if (!resuming_) request_ = boardlink::CommissioningImport{};
        peerBoot_ = 0;
        nonce_[0] = 0;
        proof_ = false;
        reason_ = "none";
        stage_ = BrainInstallStage::Discovering;
        return true;
    }
    bool busy() const {
        return stage_ != BrainInstallStage::Idle && stage_ != BrainInstallStage::Persisted &&
               stage_ != BrainInstallStage::Failed;
    }
    BrainInstallStage stage() const { return stage_; }
    const char* reason() const { return reason_; }
    bool mayHaveWritten() const { return writesRequested_; }
    const char* status() const {
        switch (stage_) {
            case BrainInstallStage::Idle: return "idle";
            case BrainInstallStage::Discovering: return "discovering";
            case BrainInstallStage::Reserving: return "reserving";
            case BrainInstallStage::Reading: return "reading";
            case BrainInstallStage::Installing: return "installing";
            case BrainInstallStage::Verifying: return "verifying";
            case BrainInstallStage::Persisted: return "persisted_restart_required";
            case BrainInstallStage::Failed: return "failed";
        }
        return "failed";
    }
    void cancel(uint32_t nowMs) { if (busy()) fail("cancelled_outcome_unknown", nowMs); }
    void poll(uint32_t nowMs) {
        if (!busy()) return;
        if (!localMaintenance_()) { fail("maintenance_ended", nowMs); return; }
        if (stage_ == BrainInstallStage::Discovering) {
            const auto& peer = link_.discoveryResult();
            if (peer.state == boardlink::DiscoveryState::Pending) return;
            if (peer.state != boardlink::DiscoveryState::Found) {
                fail("discovery_unavailable", nowMs); return;
            }
            peerBoot_ = peer.peerBoot;
            std::memcpy(peerPhysical_, peer.physicalId, sizeof(peerPhysical_));
            if (!link_.requestMaintenance(device_, nowMs)) {
                fail("reservation_unavailable", nowMs); return;
            }
            stage_ = BrainInstallStage::Reserving;
            return;
        }
        if (stage_ == BrainInstallStage::Reserving) {
            const auto state = link_.maintenanceState();
            if (state == boardlink::BoardMaintenanceState::Pending) return;
            if (state != boardlink::BoardMaintenanceState::Active) {
                fail("reservation_unavailable", nowMs); return;
            }
            std::memcpy(nonce_, link_.installationNonce(), sizeof(nonce_));
            const bool reading = leaseValid(nowMs) &&
                (resuming_ ? link_.requestRecoveryRecords(device_, motionPair(request_.pairing), nowMs)
                           : link_.requestRecords(device_, nowMs));
            if (!reading) {
                fail("records_unavailable", nowMs); return;
            }
            stage_ = BrainInstallStage::Reading;
            return;
        }
        if (!leaseValid(nowMs)) { fail("reservation_lost_outcome_unknown", nowMs); return; }
        if (stage_ == BrainInstallStage::Installing) {
            const auto state = link_.installationState();
            if (state == boardlink::BoardInstallState::Pending) return;
            if (state != boardlink::BoardInstallState::Complete) {
                fail("motion_write_outcome_unknown", nowMs); return;
            }
            const auto result = link_.installationResult();
            if (result != boardlink::CommissioningResult::Installed &&
                result != boardlink::CommissioningResult::AlreadyInstalled) {
                fail(importFailure(result), nowMs); return;
            }
            if (!link_.requestInstalledRecords(device_, motionPair(request_.pairing), nowMs)) {
                fail("verification_unavailable", nowMs); return;
            }
            stage_ = BrainInstallStage::Verifying;
            return;
        }
        const auto state = link_.recordsState();
        if (state == boardlink::ExportTransferState::Pending) return;
        const auto* snapshot = link_.recordsSnapshot();
        if (state != boardlink::ExportTransferState::Complete || !snapshot ||
            snapshot->boot != peerBoot_ || std::strcmp(snapshot->deviceId, device_) ||
            std::strcmp(snapshot->physicalId, peerPhysical_)) {
            fail("records_invalid", nowMs); return;
        }
        if (stage_ == BrainInstallStage::Reading) {
            if (!prepare(*snapshot)) { fail(reason_, nowMs); return; }
            // All staging lives in long-lived members or checked heap memory.
            auto motion = std::unique_ptr<boardlink::CommissioningImport>(
                new (std::nothrow) boardlink::CommissioningImport(request_));
            if (!motion) { fail("resources_unavailable", nowMs); return; }
            motion->pairing = motionPair(request_.pairing);
            if (!link_.requestInstallation(*motion, nowMs)) {
                fail("motion_import_unavailable", nowMs); return;
            }
            stage_ = BrainInstallStage::Installing;
            writesRequested_ = true;
            return;
        }
        if (!motionMatches(*snapshot, true)) { fail("motion_verification_failed", nowMs); return; }
        proof_ = true;
        const auto result = importer_.importBrain(request_, store_, *this);
        proof_ = false;
        if (result != boardlink::CommissioningResult::Installed &&
            result != boardlink::CommissioningResult::AlreadyInstalled) {
            fail(importFailure(result), nowMs); return;
        }
        // The final synchronous commit/readback can outlast its reservation.
        // Preserve the written records, but do not claim peer verification is
        // still exclusive or activate anything using an expired proof.
        if (!localMaintenance_() || !leaseValid(nowMs_())) {
            fail("reservation_lost_after_write", nowMs_()); return;
        }
        stage_ = BrainInstallStage::Persisted;
        reason_ = "activation_pending";
        link_.releaseMaintenance(nowMs_());
    }
private:
    static const char* importFailure(boardlink::CommissioningResult result) {
        using Result = boardlink::CommissioningResult;
        switch (result) {
            case Result::Invalid: return "import_invalid";
            case Result::IdentityMismatch: return "import_identity_mismatch";
            case Result::Conflict: return "import_conflict";
            case Result::Unsafe: return "import_unsafe";
            case Result::LegacyPending: return "import_legacy_event_pending";
            case Result::StorageFault: return "import_storage_fault";
            case Result::StateMissing: return "import_state_missing";
            default: return "import_result_unknown";
        }
    }
    static bool validDevice(const char* value) {
        if (!value || !*value) return false;
        for (size_t i = 0; i <= 64; ++i) {
            const char c = value[i];
            if (!c) return i > 0;
            const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                               (c >= '0' && c <= '9');
            if (!alnum && (i == 0 || (c != '_' && c != '-'))) return false;
        }
        return false;
    }
    static bool samePair(const v4::Pairing& a, const v4::Pairing& b) {
        return a.role == b.role && !std::strcmp(a.deviceId, b.deviceId) &&
               !std::strcmp(a.epoch, b.epoch) &&
               !std::strcmp(a.localPhysicalId, b.localPhysicalId) &&
               !std::strcmp(a.peerPhysicalId, b.peerPhysicalId);
    }
    static v4::Pairing motionPair(const v4::Pairing& brain) {
        auto motion = brain;
        motion.role = v4::Role::Motion;
        std::memcpy(motion.localPhysicalId, brain.peerPhysicalId, sizeof(motion.localPhysicalId));
        std::memcpy(motion.peerPhysicalId, brain.localPhysicalId, sizeof(motion.peerPhysicalId));
        return motion;
    }
    bool adoptPair(const v4::Pairing& existing) {
        auto brain = existing.role == v4::Role::Brain ? existing : motionPair(existing);
        brain.role = v4::Role::Brain;
        if (!v4::validPairing(brain) || std::strcmp(brain.deviceId, device_) ||
            std::strcmp(brain.localPhysicalId, link_.installationPhysicalId()) ||
            std::strcmp(brain.peerPhysicalId, peerPhysical_)) return false;
        if (request_.pairing.epoch[0] && !samePair(request_.pairing, brain)) return false;
        request_.pairing = brain;
        return true;
    }
    bool prepare(const boardlink::MotionExportSnapshot& motion) {
        using namespace boardlink;
        reason_ = "existing_records_conflict";
        auto brain = std::unique_ptr<BrainState>(new (std::nothrow) BrainState{});
        if (!brain) { reason_ = "resources_unavailable"; return false; }
        v4::Pairing pair;
        const auto paired = loadBoardPairing(v4::Role::Brain, pair);
        const auto loaded = store_.inspectForCommissioning(*brain);
        if (paired != PairingLoad::Ready && paired != PairingLoad::Missing) return false;
        if (loaded != BrainLoad::Ready && loaded != BrainLoad::Missing) return false;
        if (paired == PairingLoad::Ready &&
            (loaded != BrainLoad::Ready || !samePair(pair, brain->pairing))) return false;
        if (paired == PairingLoad::Ready && !adoptPair(pair)) return false;
        if (loaded == BrainLoad::Ready) {
            // Partial installation may reuse an orphan record, never its used watermarks.
            if (brain->localSequence || brain->pending || !adoptPair(brain->pairing)) return false;
            if (resuming_ && (request_.hasContext != brain->hasContext ||
                (request_.hasContext && !sameProductContext(request_.context, brain->context)))) return false;
            request_.hasContext = brain->hasContext;
            request_.context = brain->context;
        }
        if (motion.pairStatus != ExportRead::Ready && motion.pairStatus != ExportRead::Missing) return false;
        if (motion.stateStatus != ExportRead::Ready && motion.stateStatus != ExportRead::Missing) return false;
        if (motion.pairStatus == ExportRead::Ready &&
            (motion.stateStatus != ExportRead::Ready || !adoptPair(motion.pairing))) return false;
        if (motion.stateStatus == ExportRead::Ready && !adoptPair(motion.state.pairing)) return false;
        if (motion.legacyEvent != ExportRead::Missing) {
            reason_ = "legacy_event_pending"; return false;
        }
        if (motion.legacyStatus == ExportRead::Ready) {
            if ((loaded == BrainLoad::Ready || resuming_) &&
                (!request_.hasContext || !sameProductContext(request_.context, motion.legacy))) return false;
            request_.hasContext = true;
            request_.context = motion.legacy;
        } else if (motion.legacyStatus != ExportRead::Missing || request_.hasContext) return false;
        if (!request_.pairing.epoch[0]) {
            request_.pairing.role = v4::Role::Brain;
            std::strcpy(request_.pairing.deviceId, device_);
            std::strcpy(request_.pairing.localPhysicalId, link_.installationPhysicalId());
            std::strcpy(request_.pairing.peerPhysicalId, peerPhysical_);
            if (!newEpoch_(request_.pairing.epoch) || !v4::validPairing(request_.pairing)) return false;
        }
        if (motion.stateStatus == ExportRead::Ready && !motionMatches(motion, false)) return false;
        return true;
    }
    bool motionMatches(const boardlink::MotionExportSnapshot& snapshot, bool requirePair) const {
        using namespace boardlink;
        if (snapshot.stateStatus != ExportRead::Ready ||
            (requirePair && snapshot.pairStatus != ExportRead::Ready)) return false;
        auto expected = std::unique_ptr<boardlink::MotionState>(
            new (std::nothrow) boardlink::MotionState{});
        if (!expected) return false;
        expected->pairing = motionPair(request_.pairing);
        if ((snapshot.pairStatus == ExportRead::Ready && !samePair(snapshot.pairing, expected->pairing)) ||
            (request_.hasContext && !makeMotionContextBarrier(request_.context, expected->context))) return false;
        return sameMotionState(snapshot.state, *expected) && snapshot.legacyEvent == ExportRead::Missing &&
            (request_.hasContext ? (snapshot.legacyStatus == ExportRead::Ready &&
                                   sameProductContext(request_.context, snapshot.legacy))
                                 : snapshot.legacyStatus == ExportRead::Missing);
    }
    bool leaseValid(uint32_t nowMs) {
        return link_.installationLeaseValid(nowMs) &&
               link_.discoveryResult().peerBoot == peerBoot_ &&
               !std::strcmp(link_.discoveryResult().physicalId, peerPhysical_) &&
               !std::strcmp(link_.installationNonce(), nonce_);
    }
    bool allowImport(const boardlink::CommissioningImport& request) override {
        const auto* snapshot = link_.recordsSnapshot();
        return proof_ && localMaintenance_() && leaseValid(nowMs_()) && snapshot &&
               snapshot->boot == peerBoot_ && samePair(request.pairing, request_.pairing) &&
               request.hasContext == request_.hasContext &&
               (!request.hasContext || boardlink::sameProductContext(request.context, request_.context)) &&
               motionMatches(*snapshot, true);
    }
    void fail(const char* reason, uint32_t nowMs) {
        reason_ = reason;
        proof_ = false;
        stage_ = BrainInstallStage::Failed;
        link_.releaseMaintenance(nowMs);
    }
    Link& link_;
    boardlink::BrainStateStore& store_;
    bool (*localMaintenance_)();
    uint32_t (*nowMs_)();
    bool (*newEpoch_)(char (&)[33]);
    boardlink::BoardCommissioning importer_{};
    boardlink::CommissioningImport request_{};
    char device_[65]{}, peerPhysical_[13]{}, nonce_[33]{};
    uint64_t peerBoot_ = 0;
    BrainInstallStage stage_ = BrainInstallStage::Idle;
    const char* reason_ = "none";
    bool proof_ = false;
    bool writesRequested_ = false;
    bool resuming_ = false;
};

} }
