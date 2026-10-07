#include "BoardProtocolV4.h"
#include "BoardProtocol.h"

#include <cstring>

namespace babytech { namespace v4 {
namespace {
const uint8_t kMagic[] = {'B', 'T', 'M', '4'};

uint64_t readLe(const uint8_t* bytes, size_t count) {
    uint64_t result = 0;
    for (size_t i = 0; i < count; ++i) result |= uint64_t(bytes[i]) << (8 * i);
    return result;
}

void writeLe(uint8_t* bytes, uint64_t value, size_t count) {
    for (size_t i = 0; i < count; ++i) bytes[i] = uint8_t(value >> (8 * i));
}

bool sameKey(const Frame& frame, const Message& message) {
    return frame.senderBoot == message.senderBoot &&
           frame.receiverBoot == message.receiverBoot &&
           frame.messageId == message.messageId;
}

void copyHeader(const Frame& frame, Message& message) {
    message.kind = frame.kind;
    message.senderBoot = frame.senderBoot;
    message.receiverBoot = frame.receiverBoot;
    message.messageId = frame.messageId;
    message.length = frame.total;
}

bool validStop(const StopRequest& request) {
    if (request.source != Source::CloudCommand && request.source != Source::LocalTouch)
        return false;
    if ((request.source == Source::CloudCommand &&
         (request.sequence == 0 || request.sequence > kMaxSequence)) ||
        (request.source == Source::LocalTouch && request.sequence != 0)) return false;
    if (request.scope != StopScope::Product && request.scope != StopScope::Workbench &&
        request.scope != StopScope::Idle) return false;
    bool hasExecution = false;
    for (uint8_t byte : request.executionId) hasExecution |= byte != 0;
    if (hasExecution == (request.scope == StopScope::Idle)) return false;
    return request.commandIdLength > 0 && request.commandIdLength <= 128 &&
           validUtf8(reinterpret_cast<const uint8_t*>(request.commandId),
                     request.commandIdLength);
}
}  // namespace

bool isControl(Kind kind) {
    return kind == Kind::Stop || kind == Kind::Heartbeat || kind == Kind::StatusQuery ||
           kind == Kind::LinkAck || kind == Kind::LinkReject;
}

bool validFrame(const Frame& frame) {
    if (uint8_t(frame.kind) < uint8_t(Kind::Hello) ||
        uint8_t(frame.kind) > uint8_t(Kind::MigrationMaintenance) || !frame.senderBoot ||
        (!frame.receiverBoot && frame.kind != Kind::Hello && frame.kind != Kind::Discovery) || !frame.messageId ||
        frame.total > kMaxMessage || frame.length > kMaxFragment ||
        frame.offset > frame.total || frame.length > frame.total - frame.offset) return false;
    if (frame.kind == Kind::Heartbeat || frame.kind == Kind::StatusQuery)
        return frame.total == 0 && frame.offset == 0 && frame.length == 0;
    if (!frame.total || !frame.length) return false;
    return !isControl(frame.kind) || (frame.offset == 0 && frame.length == frame.total);
}

size_t encode(const Frame& frame, uint8_t* output, size_t capacity) {
    const size_t size = kHeaderSize + frame.length + 2;
    if (!output || capacity < size || !validFrame(frame)) return 0;
    std::memcpy(output, kMagic, sizeof(kMagic));
    output[4] = 4;
    output[5] = uint8_t(frame.kind);
    output[6] = output[7] = 0;
    writeLe(output + 8, frame.senderBoot, 8);
    writeLe(output + 16, frame.receiverBoot, 8);
    writeLe(output + 24, frame.messageId, 4);
    writeLe(output + 28, frame.total, 2);
    writeLe(output + 30, frame.offset, 2);
    writeLe(output + 32, frame.length, 2);
    if (frame.length) std::memcpy(output + kHeaderSize, frame.payload, frame.length);
    writeLe(output + size - 2, babytech::crc16(output + 4, size - 6), 2);
    return size;
}

bool fragment(const Message& message, size_t offset, Frame& result) {
    if (message.length > kMaxMessage || offset > message.length ||
        (message.length && offset == message.length)) return false;
    Frame frame;
    frame.kind = message.kind;
    frame.senderBoot = message.senderBoot;
    frame.receiverBoot = message.receiverBoot;
    frame.messageId = message.messageId;
    frame.total = message.length;
    frame.offset = uint16_t(offset);
    const size_t remaining = message.length - offset;
    frame.length = uint16_t(remaining > kMaxFragment ? kMaxFragment : remaining);
    if (!validFrame(frame)) return false;
    if (frame.length) std::memcpy(frame.payload, message.payload + offset, frame.length);
    result = frame;
    return true;
}

void Parser::reset() { size_ = 0; }

void Parser::discard(size_t count) {
    size_ -= count;
    std::memmove(bytes_, bytes_ + count, size_);
}

bool Parser::push(uint8_t byte, uint32_t nowMs, Frame& result) {
    if (size_ && uint32_t(nowMs - lastByteAt_) >= kByteTimeoutMs) reset();
    lastByteAt_ = nowMs;
    if (size_ == sizeof(bytes_)) discard(1);
    bytes_[size_++] = byte;
    while (size_) {
        const size_t prefix = size_ < sizeof(kMagic) ? size_ : sizeof(kMagic);
        if (std::memcmp(bytes_, kMagic, prefix) != 0) { discard(1); continue; }
        if (size_ < 8) return false;
        if (bytes_[4] != 4 || bytes_[6] || bytes_[7]) { discard(1); continue; }
        if (size_ < kHeaderSize) return false;
        Frame frame;
        frame.kind = Kind(bytes_[5]);
        frame.senderBoot = readLe(bytes_ + 8, 8);
        frame.receiverBoot = readLe(bytes_ + 16, 8);
        frame.messageId = uint32_t(readLe(bytes_ + 24, 4));
        frame.total = uint16_t(readLe(bytes_ + 28, 2));
        frame.offset = uint16_t(readLe(bytes_ + 30, 2));
        frame.length = uint16_t(readLe(bytes_ + 32, 2));
        if (!validFrame(frame)) { discard(1); continue; }
        const size_t size = kHeaderSize + frame.length + 2;
        if (size_ < size) return false;
        if (readLe(bytes_ + size - 2, 2) != babytech::crc16(bytes_ + 4, size - 6)) {
            discard(1);
            continue;
        }
        if (frame.length) std::memcpy(frame.payload, bytes_ + kHeaderSize, frame.length);
        result = frame;
        discard(size);
        return true;
    }
    return false;
}

void Assembler::reset() { active_ = false; received_ = 0; }

bool Assembler::expire(uint32_t nowMs) {
    if (!active_ || uint32_t(nowMs - startedAt_) < kMessageTimeoutMs) return false;
    reset();
    return true;
}

AssemblyResult Assembler::accept(const Frame& frame, uint32_t nowMs, Message& result) {
    expire(nowMs);
    if (!validFrame(frame)) return AssemblyResult::Invalid;
    if (isControl(frame.kind)) {
        copyHeader(frame, result);
        if (frame.length) std::memcpy(result.payload, frame.payload, frame.length);
        return AssemblyResult::Complete;
    }
    if (active_) {
        if (!sameKey(frame, pending_)) return AssemblyResult::Busy;
        if (frame.kind != pending_.kind || frame.total != pending_.length) {
            reset();
            return AssemblyResult::Invalid;
        }
    } else {
        if (frame.offset) return AssemblyResult::Invalid;
        copyHeader(frame, pending_);
        received_ = 0;
        startedAt_ = nowMs;
        active_ = true;
    }
    if (frame.offset < received_) {
        if (frame.length <= received_ - frame.offset &&
            std::memcmp(pending_.payload + frame.offset, frame.payload, frame.length) == 0)
            return AssemblyResult::DuplicateFragment;
        reset();
        return AssemblyResult::Invalid;
    }
    if (frame.offset != received_) {
        reset();
        return AssemblyResult::Invalid;
    }
    std::memcpy(pending_.payload + received_, frame.payload, frame.length);
    received_ += frame.length;
    if (received_ != pending_.length) return AssemblyResult::Incomplete;
    result = pending_;
    reset();
    return AssemblyResult::Complete;
}

bool validUtf8(const uint8_t* bytes, size_t size) {
    if (!bytes && size) return false;
    for (size_t i = 0; i < size;) {
        const uint8_t first = bytes[i++];
        if (!first) return false;
        if (first < 0x80) continue;
        uint32_t scalar, minimum;
        size_t continuation;
        if (first >= 0xc2 && first <= 0xdf) {
            scalar = first & 0x1f; minimum = 0x80; continuation = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            scalar = first & 0x0f; minimum = 0x800; continuation = 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            scalar = first & 0x07; minimum = 0x10000; continuation = 3;
        } else return false;
        if (continuation > size - i) return false;
        while (continuation--) {
            const uint8_t next = bytes[i++];
            if ((next & 0xc0) != 0x80) return false;
            scalar = (scalar << 6) | (next & 0x3f);
        }
        if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff))
            return false;
    }
    return true;
}

size_t encodeStop(const StopRequest& request, uint8_t* output, size_t capacity) {
    const size_t size = 27 + request.commandIdLength;
    if (!output || capacity < size || !validStop(request)) return 0;
    output[0] = uint8_t(request.source);
    writeLe(output + 1, request.sequence, 8);
    output[9] = uint8_t(request.scope);
    std::memcpy(output + 10, request.executionId, sizeof(request.executionId));
    output[26] = request.commandIdLength;
    std::memcpy(output + 27, request.commandId, request.commandIdLength);
    return size;
}

bool decodeStop(const uint8_t* payload, size_t size, StopRequest& result) {
    if (!payload || size < 28 || size > 155 || size != size_t(27 + payload[26]))
        return false;
    StopRequest request;
    request.source = Source(payload[0]);
    request.sequence = readLe(payload + 1, 8);
    request.scope = StopScope(payload[9]);
    std::memcpy(request.executionId, payload + 10, sizeof(request.executionId));
    request.commandIdLength = payload[26];
    std::memcpy(request.commandId, payload + 27, request.commandIdLength);
    if (!validStop(request)) return false;
    result = request;
    return true;
}

} }  // namespace babytech::v4
