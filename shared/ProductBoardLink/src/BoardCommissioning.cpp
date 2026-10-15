#include "BoardCommissioning.h"

#ifdef ARDUINO
#include "LegacyContextStore.h"
#include <nvs.h>
#include <cmath>
#include <cstring>

namespace babytech { namespace boardlink {
namespace {
bool validImport(const CommissioningImport& request, v4::Role role) {
    if (!v4::validPairing(request.pairing) || request.pairing.role != role) return false;
    const auto& c = request.context;
    if (request.hasContext)
        return validProductContext(c) && !std::strcmp(c.deviceId, request.pairing.deviceId);
    return !c.deviceId[0] && !c.profileVersion && !c.cleared && !c.babyId[0] && !c.babyName[0] &&
        !c.formulaBrand[0] && !c.waterMl && !c.temperatureC && c.powderGPer100Ml == 0 &&
        !std::signbit(c.powderGPer100Ml);
}
bool samePair(const v4::Pairing& a, const v4::Pairing& b) {
    return a.role == b.role && !std::strcmp(a.deviceId, b.deviceId) && !std::strcmp(a.epoch, b.epoch) &&
        !std::strcmp(a.localPhysicalId, b.localPhysicalId) && !std::strcmp(a.peerPhysicalId, b.peerPhysicalId);
}
}

CommissioningResult BoardCommissioning::fault() {
    faulted_ = true;
    return CommissioningResult::StorageFault;
}

CommissioningResult BoardCommissioning::inspectPair(const CommissioningImport& request, v4::Role role,
                                                    bool& alreadyPaired) {
    const auto hardware = verifyBoardPairing(role, request.pairing);
    if (hardware == PairingLoad::IdentityMismatch) return CommissioningResult::IdentityMismatch;
    if (hardware != PairingLoad::Ready) return fault();
    v4::Pairing existing;
    const auto loaded = loadBoardPairing(role, existing);
    alreadyPaired = loaded == PairingLoad::Ready;
    if (alreadyPaired)
        return samePair(existing, request.pairing) ? CommissioningResult::AlreadyInstalled
                                                 : CommissioningResult::Conflict;
    if (loaded == PairingLoad::Missing) return CommissioningResult::Installed;
    if (loaded == PairingLoad::IdentityMismatch) return CommissioningResult::Conflict;
    return fault();
}

CommissioningResult BoardCommissioning::inspectLegacyMotion(const CommissioningImport& request) {
    nvs_handle_t handle;
    const auto opened = nvs_open("formulaevt", NVS_READONLY, &handle);
    if (opened != ESP_OK && opened != ESP_ERR_NVS_NOT_FOUND) return fault();
    if (opened == ESP_OK) {
        size_t length = 0;
        const auto read = nvs_get_str(handle, "payload", nullptr, &length);
        nvs_close(handle);
        if (read == ESP_OK) {
            // Any old journal/terminal remains owned by the paired legacy
            // firmware. Even a malformed record is never permission to erase.
            if (length <= 1 || length > 2048) return fault();
            return CommissioningResult::LegacyPending;
        }
        if (read != ESP_ERR_NVS_NOT_FOUND) return fault();
    }
    ProductContext existing;
    const auto loaded = loadLegacyProductContext(request.pairing.deviceId, existing);
    if (loaded == LegacyContextLoad::Missing)
        return request.hasContext ? CommissioningResult::Conflict : CommissioningResult::Installed;
    if (loaded != LegacyContextLoad::Ready) return fault();
    return request.hasContext && sameProductContext(request.context, existing)
        ? CommissioningResult::Installed : CommissioningResult::Conflict;
}

CommissioningResult BoardCommissioning::finishPair(const CommissioningImport& request, bool stateWritten,
                                                   CommissioningGuard& guard) {
    if (!guard.allowImport(request)) return CommissioningResult::Unsafe;
    const auto result = installer_.installFirst(request.pairing.role, request.pairing);
    switch (result) {
        case PairingInstall::Installed: return CommissioningResult::Installed;
        case PairingInstall::AlreadyInstalled:
            return stateWritten ? CommissioningResult::Installed : CommissioningResult::AlreadyInstalled;
        case PairingInstall::Invalid: return CommissioningResult::Invalid;
        case PairingInstall::IdentityMismatch: return CommissioningResult::IdentityMismatch;
        case PairingInstall::Conflict: return CommissioningResult::Conflict;
        default: return fault();
    }
}

CommissioningResult BoardCommissioning::importBrain(const CommissioningImport& request,
                                                    BrainStateStore& store, CommissioningGuard& guard) {
    if (faulted_ || store.faulted()) return fault();
    if (!validImport(request, v4::Role::Brain)) return CommissioningResult::Invalid;
    if (!guard.allowImport(request)) return CommissioningResult::Unsafe;
    bool alreadyPaired = false;
    const auto pair = inspectPair(request, v4::Role::Brain, alreadyPaired);
    if (pair != CommissioningResult::Installed && pair != CommissioningResult::AlreadyInstalled) return pair;
    const auto loaded = store.load(request.pairing);
    if (loaded == BrainLoad::Missing && alreadyPaired) {
        faulted_ = true;
        return CommissioningResult::StateMissing;
    }
    if (loaded != BrainLoad::Missing && loaded != BrainLoad::Ready) return fault();
    if (!guard.allowImport(request)) return CommissioningResult::Unsafe;
    const auto written = store.installInitial(request.pairing, request.hasContext ? &request.context : nullptr);
    if (written == BrainWrite::Conflict) return CommissioningResult::Conflict;
    if (written != BrainWrite::Stored && written != BrainWrite::Unchanged) return fault();
    return finishPair(request, written == BrainWrite::Stored, guard);
}

CommissioningResult BoardCommissioning::importMotion(const CommissioningImport& request,
                                                     MotionStateStore& store, CommissioningGuard& guard) {
    if (faulted_ || store.faulted()) return fault();
    if (!validImport(request, v4::Role::Motion)) return CommissioningResult::Invalid;
    if (!guard.allowImport(request)) return CommissioningResult::Unsafe;
    bool alreadyPaired = false;
    const auto pair = inspectPair(request, v4::Role::Motion, alreadyPaired);
    if (pair != CommissioningResult::Installed && pair != CommissioningResult::AlreadyInstalled) return pair;
    auto legacy = inspectLegacyMotion(request);
    if (legacy != CommissioningResult::Installed) return legacy;
    const auto loaded = store.load(request.pairing);
    if (loaded == MotionLoad::Missing && alreadyPaired) {
        faulted_ = true;
        return CommissioningResult::StateMissing;
    }
    if (loaded != MotionLoad::Missing && loaded != MotionLoad::Ready) return fault();
    if (!guard.allowImport(request)) return CommissioningResult::Unsafe;
    // Re-read old evidence just before mutation. The guard's exclusive owner
    // prevents a live old producer or debug writer from changing it afterward.
    legacy = inspectLegacyMotion(request);
    if (legacy != CommissioningResult::Installed) return legacy;
    const auto written = store.installInitial(request.pairing, request.hasContext ? &request.context : nullptr);
    if (written == MotionWrite::Conflict) return CommissioningResult::Conflict;
    if (written != MotionWrite::Stored && written != MotionWrite::Unchanged) return fault();
    return finishPair(request, written == MotionWrite::Stored, guard);
}

} }
#endif
