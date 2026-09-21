#include "BoardProtocol.h"
#include <cstring>

namespace babytech {
namespace {
constexpr uint8_t kMagic0 = 0x42;
constexpr uint8_t kMagic1 = 0x4D;

uint16_t read16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
uint32_t read32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
void write16(uint8_t* p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
}
void write32(uint8_t* p, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (i * 8));
}
bool knownType(uint8_t value) {
    return value == static_cast<uint8_t>(Type::GetStatus) ||
           value == static_cast<uint8_t>(Type::Status);
}
}  // namespace

uint16_t crc16(const uint8_t* data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

size_t encode(Type type, uint32_t sequence, const uint8_t* payload,
              size_t length, uint8_t* output, size_t capacity) {
    const size_t total = kHeaderSize + length + 2;
    if (!output || length > kMaxPayload || capacity < total ||
        (length != 0 && !payload) || !knownType(static_cast<uint8_t>(type))) return 0;
    output[0] = kMagic0;
    output[1] = kMagic1;
    output[2] = kVersion;
    output[3] = static_cast<uint8_t>(type);
    write16(output + 4, static_cast<uint16_t>(length));
    write32(output + 6, sequence);
    if (length) std::memcpy(output + kHeaderSize, payload, length);
    write16(output + total - 2, crc16(output, total - 2));
    return total;
}

bool Parser::push(uint8_t byte, Frame& frame) {
    if (size_ == 0) {
        if (byte == kMagic0) buffer_[size_++] = byte;
        return false;
    }
    if (size_ == 1 && byte != kMagic1) {
        size_ = byte == kMagic0 ? 1 : 0;
        return false;
    }
    buffer_[size_++] = byte;
    if ((size_ == 3 && buffer_[2] != kVersion) ||
        (size_ == 4 && !knownType(buffer_[3]))) {
        reset();
        return false;
    }
    if (size_ < kHeaderSize) return false;
    const uint16_t length = read16(buffer_ + 4);
    if (length > kMaxPayload) {
        reset();
        return false;
    }
    const size_t total = kHeaderSize + length + 2;
    if (size_ < total) return false;
    reset();
    if (read16(buffer_ + total - 2) != crc16(buffer_, total - 2)) return false;
    frame.type = static_cast<Type>(buffer_[3]);
    frame.sequence = read32(buffer_ + 6);
    frame.length = length;
    if (length) std::memcpy(frame.payload, buffer_ + kHeaderSize, length);
    return true;
}

bool readStatus(const Frame& frame, Status& status) {
    if (frame.type != Type::Status || frame.length != 6 ||
        frame.payload[4] != static_cast<uint8_t>(MotionState::NotConfigured) ||
        (frame.payload[5] & 0xFC) != 0) return false;
    status.uptimeMs = read32(frame.payload);
    status.state = static_cast<MotionState>(frame.payload[4]);
    status.motorsAvailable = (frame.payload[5] & 1) != 0;
    status.sensorsAvailable = (frame.payload[5] & 2) != 0;
    return true;
}

size_t respondToStatusQuery(const Frame& request, uint32_t uptimeMs,
                           uint8_t* output, size_t capacity) {
    if (request.type != Type::GetStatus || request.length != 0) return 0;
    uint8_t payload[6]{};
    write32(payload, uptimeMs);
    payload[4] = static_cast<uint8_t>(MotionState::NotConfigured);
    return encode(Type::Status, request.sequence, payload, sizeof(payload), output, capacity);
}
}  // namespace babytech
