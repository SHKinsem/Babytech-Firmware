#include "BrainStateRecord.h"
#include "BoardPairingRecord.h"
#include "RecordBytes.h"

#include <cstring>

namespace babytech { namespace boardlink {
namespace {
bool samePairing(const v4::Pairing& left, const v4::Pairing& right) {
    return left.role == right.role && !std::strcmp(left.deviceId, right.deviceId) &&
        !std::strcmp(left.epoch, right.epoch) && !std::strcmp(left.localPhysicalId, right.localPhysicalId) &&
        !std::strcmp(left.peerPhysicalId, right.peerPhysicalId);
}
bool emptyContext(const ProductContext& c) {
    return !c.deviceId[0] && !c.profileVersion && !c.cleared && !c.babyId[0] &&
        !c.babyName[0] && !c.formulaBrand[0] && !c.waterMl && !c.temperatureC && c.powderGPer100Ml == 0;
}
bool emptyRequest(const ProductRequest& r) {
    return r.source == v4::Source::LocalTouch && r.command == ProductCommand::None &&
        !r.sequence && !r.deviceId[0] && !r.commandId[0] && !r.babyId[0] && !r.profileVersion &&
        !r.waterMl && !r.temperatureC && r.powderGPer100Ml == 0;
}
bool zeroDigest(const uint8_t (&digest)[kProductDigestSize]) {
    for (uint8_t byte : digest) if (byte) return false;
    return true;
}
bool putBlob(detail::ByteWriter& writer, const uint8_t* bytes, size_t length) {
    return length && writer.integer(length, 2) && writer.raw(bytes, length);
}
}

bool validBrainState(const BrainState& state) {
    if (!v4::validPairing(state.pairing) || state.pairing.role != v4::Role::Brain ||
        state.localSequence > v4::kMaxSequence) return false;
    if (state.hasContext) {
        if (!validProductContext(state.context) ||
            std::strcmp(state.context.deviceId, state.pairing.deviceId)) return false;
    } else if (!emptyContext(state.context)) return false;
    if (!state.pending) return emptyRequest(state.pendingRequest) && zeroDigest(state.pendingDigest);
    const auto& request = state.pendingRequest;
    if (!validProductRequest(request) || request.source != v4::Source::LocalTouch ||
        request.sequence != state.localSequence || std::strcmp(request.deviceId, state.pairing.deviceId))
        return false;
    char expectedId[129];
    if (!makeLocalCommandId(state.pairing, request.sequence, expectedId) ||
        std::strcmp(request.commandId, expectedId)) return false;
    if (request.command == ProductCommand::Prepare) {
        if (!state.hasContext || request.profileVersion > state.context.profileVersion) return false;
        // A later context/clear must not rewrite an already persisted intent.
        if (request.profileVersion == state.context.profileVersion &&
            (state.context.cleared || std::strcmp(request.babyId, state.context.babyId) ||
             request.powderGPer100Ml != state.context.powderGPer100Ml)) return false;
    }
    uint8_t expectedDigest[kProductDigestSize];
    return requestDigest(request, expectedDigest) &&
        !std::memcmp(expectedDigest, state.pendingDigest, sizeof(expectedDigest));
}

bool sameBrainState(const BrainState& left, const BrainState& right) {
    return validBrainState(left) && validBrainState(right) && samePairing(left.pairing, right.pairing) &&
        left.hasContext == right.hasContext && (!left.hasContext || sameProductContext(left.context, right.context)) &&
        left.localSequence == right.localSequence && left.pending == right.pending &&
        (!left.pending || sameProductRequest(left.pendingRequest, right.pendingRequest));
}

size_t encodeBrainState(const BrainState& state, uint8_t* output, size_t capacity) {
    if (!output || !validBrainState(state)) return 0;
    uint8_t record[kBrainStateMaxSize];
    uint8_t blob[kContextIdentityMaxSize];
    detail::ByteWriter writer(record + detail::kRecordHeaderSize,
                               sizeof(record) - detail::kRecordHeaderSize);
    size_t length = encodePairingRecord(state.pairing, blob, sizeof(blob));
    if (!putBlob(writer, blob, length) || !writer.integer(state.hasContext, 1)) return 0;
    if (state.hasContext) {
        length = encodeContextIdentity(state.context, blob, sizeof(blob));
        if (!putBlob(writer, blob, length)) return 0;
    }
    if (!writer.integer(state.localSequence, 8) || !writer.integer(state.pending, 1)) return 0;
    if (state.pending) {
        length = encodeRequestIdentity(state.pendingRequest, blob, sizeof(blob));
        if (!putBlob(writer, blob, length) || !writer.raw(state.pendingDigest, sizeof(state.pendingDigest))) return 0;
    }
    length = detail::kRecordHeaderSize + writer.size();
    if (length > capacity) return 0;
    detail::finishRecord(record, length, "BBS1");
    std::memcpy(output, record, length);
    return length;
}

bool decodeBrainState(const uint8_t* bytes, size_t length, BrainState& output) {
    if (length > kBrainStateMaxSize || !detail::recordHeader(bytes, length, "BBS1")) return false;
    detail::ByteReader reader(bytes + detail::kRecordHeaderSize, length - detail::kRecordHeaderSize);
    BrainState candidate;
    size_t blobLength = size_t(reader.integer(2));
    if (!decodePairingRecord(reader.take(blobLength), blobLength, candidate.pairing)) return false;
    const auto hasContext = reader.integer(1);
    if (hasContext > 1) return false;
    candidate.hasContext = hasContext == 1;
    if (candidate.hasContext) {
        blobLength = size_t(reader.integer(2));
        if (!decodeContextIdentity(reader.take(blobLength), blobLength, candidate.context)) return false;
    }
    candidate.localSequence = reader.integer(8);
    const auto pending = reader.integer(1);
    if (pending > 1) return false;
    candidate.pending = pending == 1;
    if (candidate.pending) {
        blobLength = size_t(reader.integer(2));
        if (!decodeRequestIdentity(reader.take(blobLength), blobLength, candidate.pendingRequest)) return false;
        const auto* digest = reader.take(sizeof(candidate.pendingDigest));
        if (!digest) return false;
        std::memcpy(candidate.pendingDigest, digest, sizeof(candidate.pendingDigest));
    }
    if (!reader.done() || !validBrainState(candidate)) return false;
    output = candidate;
    return true;
}

} }
