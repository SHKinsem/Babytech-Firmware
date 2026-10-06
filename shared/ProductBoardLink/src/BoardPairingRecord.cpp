#include "BoardPairingRecord.h"

#include <cstring>

namespace babytech { namespace boardlink {
namespace {
constexpr uint32_t kMagic = 0x31505442;
constexpr size_t kHeaderSize = 12;
constexpr size_t kDeviceOffset = 70;
constexpr size_t kMaxEncodedSize = kDeviceOffset + 64;
static_assert(kMaxEncodedSize <= kPairingRecordMaxSize, "Pairing record budget");

uint32_t readLe(const uint8_t* data, size_t count) {
    uint32_t value = 0;
    for (size_t i = 0; i < count; ++i) value |= uint32_t(data[i]) << (8 * i);
    return value;
}

void writeLe(uint8_t* data, uint32_t value, size_t count) {
    for (size_t i = 0; i < count; ++i) data[i] = uint8_t(value >> (8 * i));
}

uint32_t recordCrc(const uint8_t* data, size_t length) {
    uint32_t crc = 0xffffffff;
    for (size_t i = 0; i < length; ++i) {
        if (i >= 8 && i < kHeaderSize) continue;
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    return crc ^ 0xffffffff;
}
}  // namespace

size_t encodePairingRecord(const v4::Pairing& pairing, uint8_t* out, size_t capacity) {
    if (!out || !v4::validPairing(pairing)) return 0;
    const size_t deviceLength = std::strlen(pairing.deviceId);
    const size_t length = kDeviceOffset + deviceLength;
    if (capacity < length) return 0;

    uint8_t record[kMaxEncodedSize]{};
    writeLe(record, kMagic, 4);
    writeLe(record + 4, 1, 2);
    writeLe(record + 6, uint32_t(length - kHeaderSize), 2);
    record[12] = uint8_t(pairing.role);
    record[13] = uint8_t(deviceLength);
    std::memcpy(record + 14, pairing.epoch, 32);
    std::memcpy(record + 46, pairing.localPhysicalId, 12);
    std::memcpy(record + 58, pairing.peerPhysicalId, 12);
    std::memcpy(record + kDeviceOffset, pairing.deviceId, deviceLength);
    writeLe(record + 8, recordCrc(record, length), 4);
    std::memcpy(out, record, length);
    return length;
}

bool decodePairingRecord(const uint8_t* data, size_t length, v4::Pairing& out) {
    if (!data || length < kDeviceOffset + 1 || length > kMaxEncodedSize) return false;
    if (readLe(data, 4) != kMagic || readLe(data + 4, 2) != 1 ||
        readLe(data + 6, 2) != length - kHeaderSize ||
        readLe(data + 8, 4) != recordCrc(data, length)) return false;
    const size_t deviceLength = data[13];
    if (deviceLength < 1 || deviceLength > 64 ||
        length != kDeviceOffset + deviceLength ||
        std::memchr(data + kDeviceOffset, 0, deviceLength)) return false;

    v4::Pairing candidate{};
    candidate.role = static_cast<v4::Role>(data[12]);
    std::memcpy(candidate.epoch, data + 14, 32);
    std::memcpy(candidate.localPhysicalId, data + 46, 12);
    std::memcpy(candidate.peerPhysicalId, data + 58, 12);
    std::memcpy(candidate.deviceId, data + kDeviceOffset, deviceLength);
    if (!v4::validPairing(candidate)) return false;
    out = candidate;
    return true;
}

} }  // namespace babytech::boardlink
