#include "MotionStateRecord.h"
#include "BoardPairingRecord.h"
#include "RecordBytes.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace babytech { namespace boardlink {
namespace {
template <size_t N> bool text(const char (&value)[N], bool required) {
    size_t length = 0;
    while (length < N && value[length]) ++length;
    return length < N && (!required || length) &&
        v4::validUtf8(reinterpret_cast<const uint8_t*>(value), length);
}
bool code(const char (&value)[65], bool required) {
    if (!text(value, required)) return false;
    for (const char* p = value; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) return false;
    return true;
}
bool zeroDigest(const uint8_t (&digest)[kProductDigestSize]) {
    for (uint8_t byte : digest) if (byte) return false;
    return true;
}
bool emptyRequest(const ProductRequest& r) {
    return r.source == v4::Source::LocalTouch && r.command == ProductCommand::None &&
        !r.sequence && !r.deviceId[0] && !r.commandId[0] && !r.babyId[0] && !r.profileVersion &&
        !r.waterMl && !r.temperatureC && r.powderGPer100Ml == 0 && !std::signbit(r.powderGPer100Ml);
}
bool samePairing(const v4::Pairing& a, const v4::Pairing& b) {
    return a.role == b.role && !std::strcmp(a.deviceId, b.deviceId) && !std::strcmp(a.epoch, b.epoch) &&
        !std::strcmp(a.localPhysicalId, b.localPhysicalId) && !std::strcmp(a.peerPhysicalId, b.peerPhysicalId);
}
bool validBarrier(const MotionContextBarrier& c) {
    if (!c.present)
        return !c.profileVersion && !c.cleared && zeroDigest(c.digest) && !c.babyId[0] &&
            c.powderGPer100Ml == 0 && !std::signbit(c.powderGPer100Ml);
    if (!c.profileVersion || c.profileVersion > INT32_MAX || zeroDigest(c.digest)) return false;
    if (c.cleared) return !c.babyId[0] && c.powderGPer100Ml == 0 && !std::signbit(c.powderGPer100Ml);
    return text(c.babyId, true) && std::isfinite(c.powderGPer100Ml) &&
        c.powderGPer100Ml >= 1 && c.powderGPer100Ml <= 50;
}
bool requestMatchesPair(const ProductRequest& r, const v4::Pairing& pair) {
    if (!validProductRequest(r) || std::strcmp(r.deviceId, pair.deviceId)) return false;
    if (r.source == v4::Source::CloudCommand) return true;
    v4::Pairing brain = pair;
    brain.role = v4::Role::Brain;
    // makeLocalCommandId only uses the role/epoch; mirror physical IDs as well.
    std::memcpy(brain.localPhysicalId, pair.peerPhysicalId, sizeof(brain.localPhysicalId));
    std::memcpy(brain.peerPhysicalId, pair.localPhysicalId, sizeof(brain.peerPhysicalId));
    char expected[129];
    return makeLocalCommandId(brain, r.sequence, expected) && !std::strcmp(expected, r.commandId);
}
bool matchesDigest(const ProductRequest& r, const uint8_t (&digest)[kProductDigestSize]) {
    uint8_t expected[kProductDigestSize];
    return requestDigest(r, expected) && !std::memcmp(expected, digest, sizeof(expected));
}
bool validResult(const MotionResult& r, uint64_t watermark, v4::Source source, const v4::Pairing& pair) {
    if (r.kind == MotionResultKind::None)
        return !watermark && emptyRequest(r.request) && zeroDigest(r.digest) && !r.stopSequence &&
            !r.stopCommandId[0] && !r.stopExecutionId[0] && !r.accepted && !r.reason[0] &&
            r.outcome == MotionOutcome::None;
    if (!watermark || !code(r.reason, true) || r.outcome > MotionOutcome::Failed) return false;
    const bool positive = !std::strcmp(r.reason, "accepted") || !std::strcmp(r.reason, "already_idle") ||
        !std::strcmp(r.reason, "already_clear");
    if (r.accepted != positive) return false;
    if (r.kind == MotionResultKind::CloudStop)
        return source == v4::Source::CloudCommand && r.stopSequence == watermark &&
            text(r.stopCommandId, true) && (!r.stopExecutionId[0] || validExecutionId(r.stopExecutionId)) &&
            (!r.accepted || std::strcmp(r.reason, "already_clear")) &&
            emptyRequest(r.request) && zeroDigest(r.digest) && r.outcome == MotionOutcome::None;
    if (r.kind != MotionResultKind::Ordinary || !requestMatchesPair(r.request, pair) ||
        r.request.source != source || r.request.sequence != watermark ||
        !matchesDigest(r.request, r.digest) || r.stopSequence || r.stopCommandId[0] || r.stopExecutionId[0])
        return false;
    if (r.accepted && (r.request.command == ProductCommand::CheckFirmwareUpdate ||
        (r.request.command == ProductCommand::ResetError ? std::strcmp(r.reason, "already_clear") != 0
                                                       : std::strcmp(r.reason, "accepted") != 0))) return false;
    return r.outcome == MotionOutcome::None || (r.accepted &&
        (r.request.command == ProductCommand::Initialize || r.request.command == ProductCommand::Clean));
}
bool validSlot(const MotionState& state, const MotionExecutionSlot& s) {
    if (s.kind == MotionSlotKind::Empty)
        return emptyRequest(s.request) && zeroDigest(s.digest) && !s.executionId[0] && !s.eventId[0] &&
            s.targetPowderG == 0 && !std::signbit(s.targetPowderG) && !s.completed && !s.uptimeMs &&
            !s.reason[0] && !s.errorCode[0];
    if (s.kind != MotionSlotKind::Intent && s.kind != MotionSlotKind::Terminal) return false;
    const auto& r = s.request;
    if (!requestMatchesPair(r, state.pairing) || !matchesDigest(r, s.digest) ||
        !validExecutionId(s.executionId) || (r.command != ProductCommand::Prepare &&
        r.command != ProductCommand::Initialize && r.command != ProductCommand::Clean)) return false;
    const bool cloud = r.source == v4::Source::CloudCommand;
    const auto watermark = cloud ? state.cloudSequence : state.localSequence;
    const auto& result = cloud ? state.cloudResult : state.localResult;
    if (r.sequence > watermark) return false;
    if (r.sequence == watermark && (result.kind != MotionResultKind::Ordinary || !result.accepted ||
        std::strcmp(result.reason, "accepted") || result.outcome != MotionOutcome::None ||
        !sameProductRequest(result.request, r))) return false;
    if (r.command == ProductCommand::Prepare) {
        char eventId[59];
        if (!text(s.eventId, true) || !makeProductEventId(state.pairing, r.source, r.sequence, eventId) ||
            std::strcmp(eventId, s.eventId) || !std::isfinite(s.targetPowderG) ||
            s.targetPowderG != productTargetPowderG(r) || !state.context.present ||
            r.profileVersion > state.context.profileVersion) return false;
        // Advancing or clearing context never rewrites an accepted run snapshot.
        if (r.profileVersion == state.context.profileVersion && (state.context.cleared ||
            std::strcmp(r.babyId, state.context.babyId) || r.powderGPer100Ml != state.context.powderGPer100Ml))
            return false;
    } else if (s.eventId[0] || s.targetPowderG != 0 || std::signbit(s.targetPowderG) ||
               s.kind == MotionSlotKind::Terminal) return false;
    if (s.kind == MotionSlotKind::Intent)
        return !s.completed && !s.uptimeMs && !s.reason[0] && !s.errorCode[0];
    return s.completed ? !s.reason[0] && !s.errorCode[0] : code(s.reason, true) && code(s.errorCode, false);
}
bool hasRequiredIntent(const MotionResult& result, const MotionExecutionSlot& slot) {
    if (result.kind != MotionResultKind::Ordinary || !result.accepted || result.outcome != MotionOutcome::None ||
        (result.request.command != ProductCommand::Initialize && result.request.command != ProductCommand::Clean))
        return true;
    // Non-feeding operations have no receipt phase: clearing their journal
    // must atomically record the outcome while this result is still retained.
    return slot.kind == MotionSlotKind::Intent && sameProductRequest(result.request, slot.request);
}
bool sameResult(const MotionResult& a, const MotionResult& b) {
    if (a.kind != b.kind) return false;
    if (a.kind == MotionResultKind::None) return true;
    if (a.accepted != b.accepted || std::strcmp(a.reason, b.reason) || a.outcome != b.outcome) return false;
    return a.kind == MotionResultKind::Ordinary ? sameProductRequest(a.request, b.request) :
        a.stopSequence == b.stopSequence && !std::strcmp(a.stopCommandId, b.stopCommandId) &&
        !std::strcmp(a.stopExecutionId, b.stopExecutionId);
}
bool sameSlot(const MotionExecutionSlot& x, const MotionExecutionSlot& y) {
    return x.kind == y.kind && (x.kind == MotionSlotKind::Empty ||
        (sameProductRequest(x.request, y.request) &&
         !std::strcmp(x.executionId, y.executionId) && !std::strcmp(x.eventId, y.eventId) &&
         x.targetPowderG == y.targetPowderG && x.completed == y.completed && x.uptimeMs == y.uptimeMs &&
         !std::strcmp(x.reason, y.reason) && !std::strcmp(x.errorCode, y.errorCode)));
}
bool validQueue(const MotionState& state) {
    if (state.pendingResultCount > kMotionResultQueueCapacity) return false;
    const bool reserved = state.slot.kind != MotionSlotKind::Empty &&
        state.slot.request.command == ProductCommand::Prepare;
    if (reserved && state.pendingResultCount == kMotionResultQueueCapacity) return false;
    for (size_t n = 0; n < kMotionResultQueueCapacity; ++n) {
        const auto& item = state.pendingResults[n];
        if (!validSlot(state, item)) return false;
        if (n >= state.pendingResultCount) {
            if (item.kind != MotionSlotKind::Empty) return false;
            continue;
        }
        if (item.kind != MotionSlotKind::Terminal) return false;
        if (state.slot.kind != MotionSlotKind::Empty &&
            (!std::strcmp(item.eventId, state.slot.eventId) ||
             !std::strcmp(item.executionId, state.slot.executionId))) return false;
        for (size_t before = 0; before < n; ++before) {
            const auto& prior = state.pendingResults[before];
            if (!std::strcmp(item.eventId, prior.eventId) ||
                !std::strcmp(item.executionId, prior.executionId)) return false;
            if (item.request.source == prior.request.source &&
                item.request.sequence <= prior.request.sequence) return false;
        }
        if (state.slot.kind != MotionSlotKind::Empty &&
            item.request.source == state.slot.request.source &&
            item.request.sequence >= state.slot.request.sequence) return false;
    }
    return true;
}
bool putRequest(detail::ByteWriter& w, const ProductRequest& r, const uint8_t* digest) {
    uint8_t bytes[kRequestIdentityMaxSize];
    const size_t length = encodeRequestIdentity(r, bytes, sizeof(bytes));
    return length && w.integer(length, 2) && w.raw(bytes, length) && w.raw(digest, kProductDigestSize);
}
bool getRequest(detail::ByteReader& reader, ProductRequest& r, uint8_t* digest) {
    const size_t length = size_t(reader.integer(2));
    if (!decodeRequestIdentity(reader.take(length), length, r)) return false;
    const auto* source = reader.take(kProductDigestSize);
    if (!source) return false;
    std::memcpy(digest, source, kProductDigestSize);
    return true;
}
bool putFloat(detail::ByteWriter& writer, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return writer.integer(bits, 4);
}
float getFloat(detail::ByteReader& reader) {
    const uint32_t bits = uint32_t(reader.integer(4));
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
bool putResult(detail::ByteWriter& w, const MotionResult& r) {
    if (!w.integer(uint8_t(r.kind), 1)) return false;
    if (r.kind == MotionResultKind::None) return true;
    if (r.kind == MotionResultKind::Ordinary) {
        if (!putRequest(w, r.request, r.digest)) return false;
    } else if (!w.integer(r.stopSequence, 8) || !w.text(r.stopCommandId) || !w.text(r.stopExecutionId)) return false;
    return w.integer(r.accepted, 1) && w.text(r.reason) && w.integer(uint8_t(r.outcome), 1);
}
bool getResult(detail::ByteReader& reader, MotionResult& r) {
    r.kind = MotionResultKind(reader.integer(1));
    if (r.kind == MotionResultKind::None) return true;
    if (r.kind == MotionResultKind::Ordinary) {
        if (!getRequest(reader, r.request, r.digest)) return false;
    } else if (r.kind == MotionResultKind::CloudStop) {
        r.stopSequence = reader.integer(8);
        if (!reader.text(r.stopCommandId, sizeof(r.stopCommandId)) ||
            !reader.text(r.stopExecutionId, sizeof(r.stopExecutionId))) return false;
    } else return false;
    const auto accepted = reader.integer(1);
    if (accepted > 1 || !reader.text(r.reason, sizeof(r.reason))) return false;
    r.accepted = accepted == 1;
    r.outcome = MotionOutcome(reader.integer(1));
    return true;
}
bool putSlot(detail::ByteWriter& w, const MotionExecutionSlot& s) {
    if (!w.integer(uint8_t(s.kind), 1)) return false;
    if (s.kind != MotionSlotKind::Empty && (!putRequest(w, s.request, s.digest) ||
        !w.text(s.executionId) || !w.text(s.eventId) || !putFloat(w, s.targetPowderG))) return false;
    return s.kind != MotionSlotKind::Terminal || (w.integer(s.completed, 1) && w.integer(s.uptimeMs, 4) &&
        w.text(s.reason) && w.text(s.errorCode));
}
bool getSlot(detail::ByteReader& reader, MotionExecutionSlot& s) {
    s.kind = MotionSlotKind(reader.integer(1));
    if (s.kind > MotionSlotKind::Terminal) return false;
    if (s.kind != MotionSlotKind::Empty) {
        if (!getRequest(reader, s.request, s.digest) || !reader.text(s.executionId, sizeof(s.executionId)) ||
            !reader.text(s.eventId, sizeof(s.eventId))) return false;
        s.targetPowderG = getFloat(reader);
    }
    if (s.kind == MotionSlotKind::Terminal) {
        const auto completed = reader.integer(1);
        if (completed > 1) return false;
        s.completed = completed == 1;
        s.uptimeMs = uint32_t(reader.integer(4));
        if (!reader.text(s.reason, sizeof(s.reason)) || !reader.text(s.errorCode, sizeof(s.errorCode))) return false;
    }
    return true;
}
}

bool validExecutionId(const char (&value)[33]) {
    bool nonzero = false;
    for (size_t i = 0; i < 32; ++i) {
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return false;
        nonzero = nonzero || value[i] != '0';
    }
    return !value[32] && nonzero;
}

float productTargetPowderG(const ProductRequest& request) {
    return std::round(request.waterMl * request.powderGPer100Ml / 10.0f) / 10.0f;
}

bool makeProductEventId(const v4::Pairing& pairing, v4::Source source, uint64_t sequence, char (&output)[59]) {
    if (!v4::validPairing(pairing) || !sequence || sequence > v4::kMaxSequence ||
        (source != v4::Source::CloudCommand && source != v4::Source::LocalTouch)) return false;
    char value[59]{};
    const int length = std::snprintf(value, sizeof(value), "evt-%s-%c-%llu", pairing.epoch,
        source == v4::Source::CloudCommand ? 'c' : 'l', static_cast<unsigned long long>(sequence));
    if (length <= 0 || size_t(length) >= sizeof(value)) return false;
    std::memcpy(output, value, sizeof(value));
    return true;
}

bool makeMotionContextBarrier(const ProductContext& context, MotionContextBarrier& output) {
    if (!validProductContext(context)) return false;
    MotionContextBarrier candidate;
    if (!contextDigest(context, candidate.digest)) return false;
    candidate.present = true;
    candidate.profileVersion = context.profileVersion;
    candidate.cleared = context.cleared;
    std::memcpy(candidate.babyId, context.babyId, sizeof(candidate.babyId));
    candidate.powderGPer100Ml = context.powderGPer100Ml;
    if (!validBarrier(candidate)) return false;
    output = candidate;
    return true;
}

bool sameMotionContextBarrier(const MotionContextBarrier& a, const MotionContextBarrier& b) {
    return validBarrier(a) && validBarrier(b) && a.present == b.present &&
        a.profileVersion == b.profileVersion && a.cleared == b.cleared &&
        !std::memcmp(a.digest, b.digest, sizeof(a.digest)) && !std::strcmp(a.babyId, b.babyId) &&
        a.powderGPer100Ml == b.powderGPer100Ml;
}

bool validMotionState(const MotionState& state) {
    return v4::validPairing(state.pairing) && state.pairing.role == v4::Role::Motion &&
        validBarrier(state.context) && state.cloudSequence <= v4::kMaxSequence &&
        state.localSequence <= v4::kMaxSequence &&
        validResult(state.cloudResult, state.cloudSequence, v4::Source::CloudCommand, state.pairing) &&
        validResult(state.localResult, state.localSequence, v4::Source::LocalTouch, state.pairing) &&
        validSlot(state, state.slot) && validQueue(state) &&
        hasRequiredIntent(state.cloudResult, state.slot) && hasRequiredIntent(state.localResult, state.slot);
}

bool sameMotionState(const MotionState& a, const MotionState& b) {
    if (!validMotionState(a) || !validMotionState(b) || !samePairing(a.pairing, b.pairing) ||
        !sameMotionContextBarrier(a.context, b.context) || a.cloudSequence != b.cloudSequence ||
        a.localSequence != b.localSequence || !sameResult(a.cloudResult, b.cloudResult) ||
        !sameResult(a.localResult, b.localResult) || !sameSlot(a.slot, b.slot) ||
        a.pendingResultCount != b.pendingResultCount) return false;
    for (size_t n = 0; n < a.pendingResultCount; ++n)
        if (!sameSlot(a.pendingResults[n], b.pendingResults[n])) return false;
    return true;
}

size_t encodeMotionState(const MotionState& state, uint8_t* output, size_t capacity) {
    if (!output || !validMotionState(state)) return 0;
    std::unique_ptr<uint8_t[]> bytes(new (std::nothrow) uint8_t[kMotionStateMaxSize]);
    if (!bytes) return 0;
    uint8_t pair[134];
    detail::ByteWriter w(bytes.get() + detail::kRecordHeaderSize, kMotionStateMaxSize - detail::kRecordHeaderSize);
    const size_t pairLength = encodePairingRecord(state.pairing, pair, sizeof(pair));
    if (!pairLength || !w.integer(pairLength, 2) || !w.raw(pair, pairLength) ||
        !w.integer(state.context.present, 1)) return 0;
    const auto& c = state.context;
    if (c.present && (!w.integer(c.profileVersion, 4) || !w.integer(c.cleared, 1) ||
        !w.raw(c.digest, sizeof(c.digest)) || !w.text(c.babyId) || !putFloat(w, c.powderGPer100Ml))) return 0;
    if (!w.integer(state.cloudSequence, 8) || !w.integer(state.localSequence, 8) ||
        !putResult(w, state.cloudResult) || !putResult(w, state.localResult) ||
        !putSlot(w, state.slot) || !w.integer(state.pendingResultCount, 1)) return 0;
    for (size_t n = 0; n < state.pendingResultCount; ++n)
        if (!putSlot(w, state.pendingResults[n])) return 0;
    const size_t length = detail::kRecordHeaderSize + w.size();
    if (length > capacity) return 0;
    detail::finishRecord(bytes.get(), length, "BMS2");
    std::memcpy(output, bytes.get(), length);
    return length;
}

bool decodeMotionState(const uint8_t* bytes, size_t length, MotionState& output) {
    if (length > kMotionStateMaxSize) return false;
    const bool legacy = length <= kMotionStateV1MaxSize && detail::recordHeader(bytes, length, "BMS1");
    if (!legacy && !detail::recordHeader(bytes, length, "BMS2")) return false;
    detail::ByteReader reader(bytes + detail::kRecordHeaderSize, length - detail::kRecordHeaderSize);
    std::unique_ptr<MotionState> storage(new (std::nothrow) MotionState);
    if (!storage) return false;
    auto& candidate = *storage;
    const size_t pairLength = size_t(reader.integer(2));
    if (!decodePairingRecord(reader.take(pairLength), pairLength, candidate.pairing)) return false;
    const auto present = reader.integer(1);
    if (present > 1) return false;
    auto& c = candidate.context;
    c.present = present == 1;
    if (c.present) {
        c.profileVersion = uint32_t(reader.integer(4));
        const auto cleared = reader.integer(1);
        const auto* digest = reader.take(sizeof(c.digest));
        if (cleared > 1 || !digest || !reader.text(c.babyId, sizeof(c.babyId))) return false;
        c.cleared = cleared == 1;
        std::memcpy(c.digest, digest, sizeof(c.digest));
        c.powderGPer100Ml = getFloat(reader);
    }
    candidate.cloudSequence = reader.integer(8);
    candidate.localSequence = reader.integer(8);
    if (!getResult(reader, candidate.cloudResult) || !getResult(reader, candidate.localResult)) return false;
    if (!getSlot(reader, candidate.slot)) return false;
    if (!legacy) {
        candidate.pendingResultCount = uint8_t(reader.integer(1));
        if (candidate.pendingResultCount > kMotionResultQueueCapacity) return false;
        for (size_t n = 0; n < candidate.pendingResultCount; ++n)
            if (!getSlot(reader, candidate.pendingResults[n])) return false;
    }
    if (!reader.done() || !validMotionState(candidate)) return false;
    output = candidate;
    return true;
}

} }
