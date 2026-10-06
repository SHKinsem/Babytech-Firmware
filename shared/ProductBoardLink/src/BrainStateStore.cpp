#include "BrainStateStore.h"

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

BrainLoad readRecord(nvs_handle_t handle, uint8_t (&bytes)[kBrainStateMaxSize],
                     BrainState& output) {
    size_t length = 0;
    esp_err_t error = nvs_get_blob(handle, "record", nullptr, &length);
    if (error == ESP_ERR_NVS_NOT_FOUND) return BrainLoad::Missing;
    if (error == ESP_ERR_NVS_TYPE_MISMATCH) return BrainLoad::Corrupt;
    if (error != ESP_OK) return BrainLoad::IoError;
    if (!length || length > sizeof(bytes)) return BrainLoad::Corrupt;
    const size_t expectedLength = length;
    std::memset(bytes, 0xff, length);
    error = nvs_get_blob(handle, "record", bytes, &length);
    if (error == ESP_ERR_NVS_TYPE_MISMATCH) return BrainLoad::Corrupt;
    // A disappearing key or a changed length during the read is not Missing.
    if (error != ESP_OK || length != expectedLength) return BrainLoad::IoError;
    return decodeBrainState(bytes, length, output) ? BrainLoad::Ready : BrainLoad::Corrupt;
}
}

BrainLoad BrainStateStore::readStored() {
    nvs_handle_t handle;
    const esp_err_t error = nvs_open("brainstate", NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return BrainLoad::Missing;
    if (error != ESP_OK) return BrainLoad::IoError;
    const BrainLoad result = readRecord(handle, buffer_, observed_);
    nvs_close(handle);
    return result;
}

BrainLoad BrainStateStore::latchLoadFault(BrainLoad reason) {
    faulted_ = true;
    faultReason_ = reason;
    return reason;
}

BrainWrite BrainStateStore::latchWriteFault() {
    latchLoadFault(BrainLoad::IoError);
    return BrainWrite::StorageFault;
}

BrainLoad BrainStateStore::load(const v4::Pairing& verifiedPairing) {
    if (faulted_) return faultReason_;
    if (!v4::validPairing(verifiedPairing) || verifiedPairing.role != v4::Role::Brain)
        return latchLoadFault(BrainLoad::IdentityMismatch);
    if (ready_ && !samePairing(state_.pairing, verifiedPairing))
        return latchLoadFault(BrainLoad::IdentityMismatch);
    const BrainLoad loaded = readStored();
    if (loaded == BrainLoad::Missing)
        return ready_ ? latchLoadFault(BrainLoad::IoError) : BrainLoad::Missing;
    if (loaded != BrainLoad::Ready) return latchLoadFault(loaded);
    if (!samePairing(observed_.pairing, verifiedPairing))
        return latchLoadFault(BrainLoad::IdentityMismatch);
    if (ready_) {
        if (!sameBrainState(observed_, state_)) return latchLoadFault(BrainLoad::IoError);
        return BrainLoad::Ready;
    }
    state_ = observed_;
    ready_ = true;
    return BrainLoad::Ready;
}

bool BrainStateStore::checkCurrent() {
    const BrainLoad loaded = readStored();
    if (loaded == BrainLoad::Ready && sameBrainState(observed_, state_)) return true;
    latchLoadFault(loaded == BrainLoad::Corrupt ? BrainLoad::Corrupt : BrainLoad::IoError);
    return false;
}

BrainWrite BrainStateStore::writeState(const BrainState& next, bool initial) {
    nvs_handle_t handle;
    if (nvs_open("brainstate", NVS_READWRITE, &handle) != ESP_OK)
        return latchWriteFault();

    // Recheck through the write handle as well. This detects unexpected writers
    // or deletion, but does not provide concurrency between the check and set.
    const BrainLoad checked = readRecord(handle, buffer_, observed_);
    if (initial && checked == BrainLoad::Ready) {
        nvs_close(handle);
        if (!sameBrainState(observed_, next)) return BrainWrite::Conflict;
        state_ = observed_;
        ready_ = true;
        return BrainWrite::Unchanged;
    }
    if ((initial && checked != BrainLoad::Missing) ||
        (!initial && (checked != BrainLoad::Ready || !sameBrainState(observed_, state_)))) {
        nvs_close(handle);
        if (checked == BrainLoad::Corrupt) {
            latchLoadFault(BrainLoad::Corrupt);
            return BrainWrite::StorageFault;
        }
        return latchWriteFault();
    }

    // The recheck used buffer_, so encode only after it has finished.
    const size_t length = encodeBrainState(next, buffer_, sizeof(buffer_));
    if (!length || length > sizeof(buffer_)) {
        nvs_close(handle);
        return latchWriteFault();
    }
    const esp_err_t written = nvs_set_blob(handle, "record", buffer_, length);
    const esp_err_t committed = written == ESP_OK ? nvs_commit(handle) : written;
    nvs_close(handle);
    if (written != ESP_OK || committed != ESP_OK) return latchWriteFault();

    // Only a fresh read-only open/decode/compare may publish the new RAM state.
    if (readStored() != BrainLoad::Ready || !sameBrainState(observed_, next))
        return latchWriteFault();
    state_ = observed_;
    ready_ = true;
    return BrainWrite::Stored;
}

BrainWrite BrainStateStore::installInitial(const v4::Pairing& verifiedPairing,
                                         const ProductContext* imported) {
    if (faulted_) return BrainWrite::StorageFault;
    if (!v4::validPairing(verifiedPairing) || verifiedPairing.role != v4::Role::Brain)
        return BrainWrite::Invalid;
    BrainState initial;
    initial.pairing = verifiedPairing;
    if (imported) {
        initial.hasContext = true;
        initial.context = *imported;
    }
    if (!validBrainState(initial)) return BrainWrite::Invalid;

    const BrainLoad loaded = readStored();
    if (loaded != BrainLoad::Ready && loaded != BrainLoad::Missing) {
        latchLoadFault(loaded);
        return BrainWrite::StorageFault;
    }
    if (ready_ && (loaded != BrainLoad::Ready || !sameBrainState(observed_, state_)))
        return latchWriteFault();
    if (loaded == BrainLoad::Ready) {
        if (!sameBrainState(observed_, initial)) return BrainWrite::Conflict;
        state_ = observed_;
        ready_ = true;
        return BrainWrite::Unchanged;
    }
    return writeState(initial, true);
}

BrainWrite BrainStateStore::saveContext(const ProductContext& context) {
    if (!ready()) return BrainWrite::StorageFault;
    if (!checkCurrent()) return BrainWrite::StorageFault;
    if (!validProductContext(context) ||
        std::strcmp(context.deviceId, state_.pairing.deviceId)) return BrainWrite::Invalid;
    if (state_.hasContext && context.profileVersion <= state_.context.profileVersion)
        return sameProductContext(context, state_.context)
            ? BrainWrite::Unchanged : BrainWrite::Conflict;
    BrainState next = state_;
    next.hasContext = true;
    next.context = context;
    // A newer context/tombstone must not change a reserved request's snapshot.
    return writeState(next, false);
}

BrainWrite BrainStateStore::reserveLocal(const ProductRequest& request) {
    if (!ready()) return BrainWrite::StorageFault;
    if (!checkCurrent()) return BrainWrite::StorageFault;
    if (state_.pending) return BrainWrite::Busy;
    if (state_.localSequence >= v4::kMaxSequence) return BrainWrite::Exhausted;
    if (!validProductRequest(request) || request.source != v4::Source::LocalTouch ||
        request.sequence != state_.localSequence + 1 ||
        std::strcmp(request.deviceId, state_.pairing.deviceId)) return BrainWrite::Invalid;
    char commandId[129];
    if (!makeLocalCommandId(state_.pairing, request.sequence, commandId) ||
        std::strcmp(commandId, request.commandId)) return BrainWrite::Invalid;
    if (request.command == ProductCommand::Prepare &&
        (!state_.hasContext || state_.context.cleared ||
         std::strcmp(request.babyId, state_.context.babyId) ||
         request.profileVersion != state_.context.profileVersion ||
         request.powderGPer100Ml != state_.context.powderGPer100Ml))
        return BrainWrite::ContextRequired;

    BrainState next = state_;
    if (!requestDigest(request, next.pendingDigest)) return latchWriteFault();
    next.pending = true;
    next.localSequence = request.sequence;
    next.pendingRequest = request;
    return writeState(next, false);
}

BrainWrite BrainStateStore::clearPending(const ProductRequest& request) {
    if (!ready()) return BrainWrite::StorageFault;
    if (!checkCurrent()) return BrainWrite::StorageFault;
    if (!validProductRequest(request)) return BrainWrite::Invalid;
    if (!state_.pending || !sameProductRequest(request, state_.pendingRequest))
        return BrainWrite::Conflict;
    uint8_t digest[kProductDigestSize];
    if (!requestDigest(request, digest)) return latchWriteFault();
    if (std::memcmp(digest, state_.pendingDigest, sizeof(digest))) return BrainWrite::Conflict;

    BrainState next = state_;
    next.pending = false;
    next.pendingRequest = ProductRequest{};
    std::memset(next.pendingDigest, 0, sizeof(next.pendingDigest));
    return writeState(next, false);
}

} }
#endif
