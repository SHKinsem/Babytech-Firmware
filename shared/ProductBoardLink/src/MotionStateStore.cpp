#include "MotionStateStore.h"

#ifdef ARDUINO
#include <nvs.h>
#include <cstring>

namespace babytech { namespace boardlink {
namespace {
bool samePairing(const v4::Pairing& left, const v4::Pairing& right) {
    return left.role == right.role && !std::strcmp(left.deviceId, right.deviceId) &&
        !std::strcmp(left.epoch, right.epoch) &&
        !std::strcmp(left.localPhysicalId, right.localPhysicalId) &&
        !std::strcmp(left.peerPhysicalId, right.peerPhysicalId);
}

// Bounded, lossless input copy. Optional null strings mean empty, never a
// truncated identifier/reason that could compare equal to a different request.
template <size_t N>
bool copyText(const char* source, char (&output)[N], bool required) {
    if (!source) source = "";
    size_t length = 0;
    while (length < N && source[length]) ++length;
    if (length == N || (required && !length) ||
        !v4::validUtf8(reinterpret_cast<const uint8_t*>(source), length)) return false;
    std::memset(output, 0, N);
    std::memcpy(output, source, length);
    return true;
}

bool positiveReason(const char* reason) {
    return !std::strcmp(reason, "accepted") || !std::strcmp(reason, "already_idle") ||
        !std::strcmp(reason, "already_clear");
}

bool validCode(const char (&value)[65], bool required) {
    if (required && !value[0]) return false;
    for (const char* at = value; *at; ++at)
        if (!((*at >= 'a' && *at <= 'z') || (*at >= 'A' && *at <= 'Z') ||
              (*at >= '0' && *at <= '9') || *at == '_')) return false;
    return true;
}

bool validDecisionReason(ProductCommand command, bool accepted, const char* reason) {
    if (!accepted) return !positiveReason(reason);
    if (command == ProductCommand::CheckFirmwareUpdate) return false;
    if (command == ProductCommand::ResetError) return !std::strcmp(reason, "already_clear");
    return !std::strcmp(reason, "accepted");
}

bool motionCommand(ProductCommand command) {
    return command == ProductCommand::Initialize || command == ProductCommand::Prepare ||
        command == ProductCommand::Clean;
}

bool localIdentity(const v4::Pairing& motionPairing, const ProductRequest& request) {
    if (request.source != v4::Source::LocalTouch) return true;
    // makeLocalCommandId validates a Brain pairing: mirror the verified pair,
    // preserving the same device and epoch, not an independently chosen epoch.
    v4::Pairing brain = motionPairing;
    brain.role = v4::Role::Brain;
    std::memcpy(brain.localPhysicalId, motionPairing.peerPhysicalId, sizeof(brain.localPhysicalId));
    std::memcpy(brain.peerPhysicalId, motionPairing.localPhysicalId, sizeof(brain.peerPhysicalId));
    char commandId[129];
    return makeLocalCommandId(brain, request.sequence, commandId) &&
        !std::strcmp(commandId, request.commandId);
}

MotionLoad readRecord(nvs_handle_t handle, uint8_t (&bytes)[kMotionStateMaxSize],
                      MotionState& output) {
    size_t length = 0;
    esp_err_t error = nvs_get_blob(handle, "record", nullptr, &length);
    if (error == ESP_ERR_NVS_NOT_FOUND) return MotionLoad::Missing;
    if (error == ESP_ERR_NVS_TYPE_MISMATCH) return MotionLoad::Corrupt;
    if (error != ESP_OK) return MotionLoad::IoError;
    if (!length || length > sizeof(bytes)) return MotionLoad::Corrupt;
    const size_t expectedLength = length;
    std::memset(bytes, 0xff, length);
    error = nvs_get_blob(handle, "record", bytes, &length);
    if (error == ESP_ERR_NVS_TYPE_MISMATCH) return MotionLoad::Corrupt;
    // A disappeared key or changed length on the second read is an I/O fault,
    // not evidence that commissioning can safely create a blank record.
    if (error != ESP_OK || length != expectedLength) return MotionLoad::IoError;
    return decodeMotionState(bytes, length, output) ? MotionLoad::Ready : MotionLoad::Corrupt;
}
}

MotionLoad MotionStateStore::readStored() {
    nvs_handle_t handle;
    const esp_err_t error = nvs_open("productstate", NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return MotionLoad::Missing;
    if (error != ESP_OK) return MotionLoad::IoError;
    const MotionLoad result = readRecord(handle, buffer_, observed_);
    nvs_close(handle);
    return result;
}

MotionLoad MotionStateStore::latchLoadFault(MotionLoad reason) {
    faulted_ = true;
    faultReason_ = reason;
    return reason;
}

MotionWrite MotionStateStore::latchWriteFault() {
    latchLoadFault(MotionLoad::IoError);
    return MotionWrite::StorageFault;
}

MotionLoad MotionStateStore::load(const v4::Pairing& verifiedPairing) {
    if (faulted_) return faultReason_;
    if (!v4::validPairing(verifiedPairing) || verifiedPairing.role != v4::Role::Motion)
        return latchLoadFault(MotionLoad::IdentityMismatch);
    if (ready_ && !samePairing(state_.pairing, verifiedPairing))
        return latchLoadFault(MotionLoad::IdentityMismatch);
    const MotionLoad loaded = readStored();
    if (loaded == MotionLoad::Missing)
        return ready_ ? latchLoadFault(MotionLoad::IoError) : MotionLoad::Missing;
    if (loaded != MotionLoad::Ready) return latchLoadFault(loaded);
    if (!samePairing(observed_.pairing, verifiedPairing))
        return latchLoadFault(MotionLoad::IdentityMismatch);
    if (ready_) {
        if (!sameMotionState(observed_, state_)) return latchLoadFault(MotionLoad::IoError);
        return MotionLoad::Ready;
    }
    state_ = observed_;
    ready_ = true;
    return MotionLoad::Ready;
}

bool MotionStateStore::checkCurrent() {
    const MotionLoad loaded = readStored();
    if (loaded == MotionLoad::Ready && sameMotionState(observed_, state_)) return true;
    latchLoadFault(loaded == MotionLoad::Ready && !samePairing(observed_.pairing, state_.pairing)
        ? MotionLoad::IdentityMismatch
        : (loaded == MotionLoad::Corrupt ? MotionLoad::Corrupt : MotionLoad::IoError));
    return false;
}

MotionWrite MotionStateStore::writeState(bool initial) {
    nvs_handle_t handle;
    if (nvs_open("productstate", NVS_READWRITE, &handle) != ESP_OK)
        return latchWriteFault();

    // Recheck through the write handle too. This detects unexpected writers;
    // it is not a lock or a concurrent compare-and-swap transaction.
    const MotionLoad checked = readRecord(handle, buffer_, observed_);
    if (initial && checked == MotionLoad::Ready) {
        nvs_close(handle);
        if (!sameMotionState(observed_, working_)) return MotionWrite::Conflict;
        state_ = observed_;
        ready_ = true;
        return MotionWrite::Unchanged;
    }
    if ((initial && checked != MotionLoad::Missing) ||
        (!initial && (checked != MotionLoad::Ready || !sameMotionState(observed_, state_)))) {
        nvs_close(handle);
        latchLoadFault(checked == MotionLoad::Ready && !samePairing(observed_.pairing, state_.pairing)
            ? MotionLoad::IdentityMismatch
            : (checked == MotionLoad::Corrupt ? MotionLoad::Corrupt : MotionLoad::IoError));
        return MotionWrite::StorageFault;
    }

    // The recheck overwrote buffer_: only encode after it has finished.
    const size_t length = encodeMotionState(working_, buffer_, sizeof(buffer_));
    if (!length || length > sizeof(buffer_)) {
        nvs_close(handle);
        return latchWriteFault();
    }
    const esp_err_t written = nvs_set_blob(handle, "record", buffer_, length);
    const esp_err_t committed = written == ESP_OK ? nvs_commit(handle) : written;
    nvs_close(handle);
    if (written != ESP_OK || committed != ESP_OK) return latchWriteFault();

    // A fresh read-only handle, decode and full comparison are required before
    // publishing RAM state or reporting Stored. An uncertain write is latched.
    const MotionLoad loaded = readStored();
    if (loaded != MotionLoad::Ready || !sameMotionState(observed_, working_)) {
        latchLoadFault(loaded == MotionLoad::Ready && !samePairing(observed_.pairing, working_.pairing)
            ? MotionLoad::IdentityMismatch
            : (loaded == MotionLoad::Corrupt ? MotionLoad::Corrupt : MotionLoad::IoError));
        return MotionWrite::StorageFault;
    }
    state_ = observed_;
    ready_ = true;
    return MotionWrite::Stored;
}

MotionWrite MotionStateStore::installInitial(const v4::Pairing& verifiedPairing,
                                            const ProductContext* imported) {
    if (faulted_) return MotionWrite::StorageFault;
    if (!v4::validPairing(verifiedPairing) || verifiedPairing.role != v4::Role::Motion)
        return MotionWrite::Invalid;
    working_ = MotionState{};
    working_.pairing = verifiedPairing;
    if (imported) {
        if (!validProductContext(*imported) ||
            std::strcmp(imported->deviceId, verifiedPairing.deviceId)) return MotionWrite::Invalid;
        if (!makeMotionContextBarrier(*imported, working_.context)) return latchWriteFault();
    }
    const MotionLoad loaded = readStored();
    if (loaded != MotionLoad::Ready && loaded != MotionLoad::Missing) {
        latchLoadFault(loaded);
        return MotionWrite::StorageFault;
    }
    if (ready_ && (loaded != MotionLoad::Ready || !sameMotionState(observed_, state_)))
        return latchWriteFault();
    if (loaded == MotionLoad::Ready) {
        if (!sameMotionState(observed_, working_)) return MotionWrite::Conflict;
        state_ = observed_;
        ready_ = true;
        return MotionWrite::Unchanged;
    }
    return writeState(true);
}

MotionWrite MotionStateStore::saveContext(const ProductContext& context) {
    if (!ready() || !checkCurrent()) return MotionWrite::StorageFault;
    if (!validProductContext(context) ||
        std::strcmp(context.deviceId, state_.pairing.deviceId)) return MotionWrite::Invalid;
    working_ = state_;
    if (!makeMotionContextBarrier(context, working_.context)) return latchWriteFault();
    if (state_.context.present && context.profileVersion <= state_.context.profileVersion)
        return sameMotionContextBarrier(working_.context, state_.context)
            ? MotionWrite::Unchanged : MotionWrite::Conflict;
    // An updated cache/tombstone never rewrites an already accepted snapshot.
    return writeState(false);
}

MotionWrite MotionStateStore::recordDecision(const ProductRequest& request, bool accepted,
                                            const char* reason, const char* executionId) {
    if (!ready() || !checkCurrent()) return MotionWrite::StorageFault;
    if (!validProductRequest(request) || std::strcmp(request.deviceId, state_.pairing.deviceId) ||
        !localIdentity(state_.pairing, request)) return MotionWrite::Invalid;
    char reasonText[65];
    if (!copyText(reason, reasonText, true) || !validCode(reasonText, true)) return MotionWrite::Invalid;
    const bool cloud = request.source == v4::Source::CloudCommand;
    const uint64_t watermark = cloud ? state_.cloudSequence : state_.localSequence;
    const MotionResult& previous = cloud ? state_.cloudResult : state_.localResult;
    uint8_t digest[kProductDigestSize];
    if (!requestDigest(request, digest)) return latchWriteFault();
    if (request.sequence < watermark) return MotionWrite::Expired;
    if (request.sequence == watermark)
        return previous.kind == MotionResultKind::Ordinary &&
            sameProductRequest(previous.request, request) &&
            !std::memcmp(previous.digest, digest, sizeof(digest)) &&
            previous.accepted == accepted && !std::strcmp(previous.reason, reasonText)
            ? MotionWrite::Unchanged : MotionWrite::Conflict;

    if (!validDecisionReason(request.command, accepted, reasonText)) return MotionWrite::Invalid;
    const bool intent = accepted && motionCommand(request.command);
    char executionText[33];
    if (!copyText(executionId, executionText, intent) ||
        (intent ? !validExecutionId(executionText) : executionText[0] != '\0'))
        return MotionWrite::Invalid;
    if (intent && state_.slot.kind != MotionSlotKind::Empty) return MotionWrite::Busy;
    if (accepted && request.command == ProductCommand::Prepare &&
        state_.pendingResultCount == kMotionResultQueueCapacity) return MotionWrite::QueueFull;
    if (accepted && request.command == ProductCommand::Prepare &&
        (!state_.context.present || state_.context.cleared ||
         request.profileVersion != state_.context.profileVersion ||
         std::strcmp(request.babyId, state_.context.babyId) ||
         request.powderGPer100Ml != state_.context.powderGPer100Ml))
        return MotionWrite::ContextRequired;

    working_ = state_;
    MotionResult& result = cloud ? working_.cloudResult : working_.localResult;
    result = MotionResult{};
    result.kind = MotionResultKind::Ordinary;
    result.request = request;
    std::memcpy(result.digest, digest, sizeof(digest));
    result.accepted = accepted;
    std::memcpy(result.reason, reasonText, sizeof(reasonText));
    (cloud ? working_.cloudSequence : working_.localSequence) = request.sequence;
    if (intent) {
        working_.slot = MotionExecutionSlot{};
        working_.slot.kind = MotionSlotKind::Intent;
        working_.slot.request = request;
        std::memcpy(working_.slot.digest, digest, sizeof(digest));
        std::memcpy(working_.slot.executionId, executionText, sizeof(executionText));
        if (request.command == ProductCommand::Prepare) {
            if (!makeProductEventId(state_.pairing, request.source, request.sequence,
                                    working_.slot.eventId)) return latchWriteFault();
            working_.slot.targetPowderG = productTargetPowderG(request);
        }
    }
    return writeState(false);
}

MotionWrite MotionStateStore::finishFeeding(const char* executionId, bool completed,
                                           const char* reason, const char* errorCode,
                                           uint32_t uptimeMs) {
    if (!ready() || !checkCurrent()) return MotionWrite::StorageFault;
    char executionText[33], reasonText[65], errorText[65];
    if (!copyText(executionId, executionText, true) || !validExecutionId(executionText) ||
        !copyText(reason, reasonText, false) || !validCode(reasonText, false) ||
        !copyText(errorCode, errorText, false) || !validCode(errorText, false))
        return MotionWrite::Invalid;
    const MotionExecutionSlot& slot = state_.slot;
    if (slot.kind == MotionSlotKind::Empty || slot.request.command != ProductCommand::Prepare ||
        std::strcmp(slot.executionId, executionText)) return MotionWrite::Conflict;
    if (slot.kind == MotionSlotKind::Terminal)
        return slot.completed == completed && slot.uptimeMs == uptimeMs &&
            !std::strcmp(slot.reason, reasonText) && !std::strcmp(slot.errorCode, errorText)
            ? MotionWrite::Unchanged : MotionWrite::Conflict;
    if (completed ? (reasonText[0] || errorText[0]) : !reasonText[0]) return MotionWrite::Invalid;
    working_ = state_;
    working_.slot.kind = MotionSlotKind::Terminal;
    working_.slot.completed = completed;
    working_.slot.uptimeMs = uptimeMs;
    std::memcpy(working_.slot.reason, reasonText, sizeof(reasonText));
    std::memcpy(working_.slot.errorCode, errorText, sizeof(errorText));
    return writeState(false);
}

MotionWrite MotionStateStore::archiveFeeding(bool stationary) {
    if (!ready() || !checkCurrent()) return MotionWrite::StorageFault;
    if (state_.slot.kind == MotionSlotKind::Empty) return MotionWrite::Unchanged;
    if (!stationary || state_.slot.kind != MotionSlotKind::Terminal) return MotionWrite::Busy;
    if (state_.pendingResultCount == kMotionResultQueueCapacity) return MotionWrite::QueueFull;
    working_ = state_;
    working_.pendingResults[working_.pendingResultCount++] = working_.slot;
    working_.slot = MotionExecutionSlot{};
    return writeState(false);
}

MotionWrite MotionStateStore::finishOperation(const char* executionId, MotionOutcome outcome,
                                             bool stationary) {
    if (!ready() || !checkCurrent()) return MotionWrite::StorageFault;
    char executionText[33];
    if (!copyText(executionId, executionText, true) || !validExecutionId(executionText) ||
        (outcome != MotionOutcome::Succeeded && outcome != MotionOutcome::Interrupted &&
         outcome != MotionOutcome::Failed)) return MotionWrite::Invalid;
    if (!stationary) return MotionWrite::Busy;
    const MotionExecutionSlot& slot = state_.slot;
    if (slot.kind != MotionSlotKind::Intent ||
        (slot.request.command != ProductCommand::Initialize && slot.request.command != ProductCommand::Clean) ||
        std::strcmp(slot.executionId, executionText)) return MotionWrite::Conflict;
    working_ = state_;
    MotionResult& result = slot.request.source == v4::Source::CloudCommand
        ? working_.cloudResult : working_.localResult;
    if (result.kind == MotionResultKind::Ordinary && sameProductRequest(result.request, slot.request) &&
        !std::memcmp(result.digest, slot.digest, sizeof(slot.digest))) result.outcome = outcome;
    working_.slot = MotionExecutionSlot{};
    return writeState(false);
}

MotionWrite MotionStateStore::acknowledge(const char* eventId, bool completed, bool stationary) {
    if (!ready() || !checkCurrent()) return MotionWrite::StorageFault;
    char eventText[59];
    if (!copyText(eventId, eventText, true)) return MotionWrite::Invalid;
    for (size_t index = 0; index < state_.pendingResultCount; ++index) {
        const auto& event = state_.pendingResults[index];
        if (std::strcmp(event.eventId, eventText)) continue;
        if (event.completed != completed) return MotionWrite::Conflict;
        working_ = state_;
        for (size_t next = index + 1; next < working_.pendingResultCount; ++next)
            working_.pendingResults[next - 1] = working_.pendingResults[next];
        working_.pendingResults[--working_.pendingResultCount] = MotionExecutionSlot{};
        return writeState(false);
    }
    if (!stationary) return MotionWrite::Busy;
    if (state_.slot.kind != MotionSlotKind::Terminal || state_.slot.completed != completed ||
        std::strcmp(state_.slot.eventId, eventText)) return MotionWrite::Conflict;
    working_ = state_;
    working_.slot = MotionExecutionSlot{};
    return writeState(false);
}

MotionWrite MotionStateStore::recordCloudStop(uint64_t sequence, const char* commandId,
                                             const char* executionId, bool accepted,
                                             const char* reason, bool stationary) {
    // The immediate hardware Stop must already have happened independently of
    // every check and return below, including storage faults and old sequences.
    if (!ready() || !checkCurrent()) return MotionWrite::StorageFault;
    char commandText[129], executionText[33], reasonText[65];
    if (!sequence || sequence > v4::kMaxSequence ||
        !copyText(commandId, commandText, true) ||
        !copyText(executionId, executionText, false) ||
        (executionText[0] && !validExecutionId(executionText)) ||
        !copyText(reason, reasonText, true) || !validCode(reasonText, true)) return MotionWrite::Invalid;
    if (!stationary) return MotionWrite::Busy;
    if (sequence < state_.cloudSequence) return MotionWrite::Expired;
    const MotionResult& previous = state_.cloudResult;
    if (sequence == state_.cloudSequence)
        return previous.kind == MotionResultKind::CloudStop && previous.stopSequence == sequence &&
            !std::strcmp(previous.stopCommandId, commandText) &&
            !std::strcmp(previous.stopExecutionId, executionText) && previous.accepted == accepted &&
            !std::strcmp(previous.reason, reasonText) ? MotionWrite::Unchanged : MotionWrite::Conflict;
    if (accepted ? (std::strcmp(reasonText, "accepted") && std::strcmp(reasonText, "already_idle"))
                 : positiveReason(reasonText)) return MotionWrite::Invalid;
    working_ = state_;
    MotionResult& result = working_.cloudResult;
    result = MotionResult{};
    result.kind = MotionResultKind::CloudStop;
    result.stopSequence = sequence;
    std::memcpy(result.stopCommandId, commandText, sizeof(commandText));
    std::memcpy(result.stopExecutionId, executionText, sizeof(executionText));
    result.accepted = accepted;
    std::memcpy(result.reason, reasonText, sizeof(reasonText));
    working_.cloudSequence = sequence;
    return writeState(false);
}

} }
#endif
