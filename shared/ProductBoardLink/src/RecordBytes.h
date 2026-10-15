#pragma once

#include "BoardProtocolV4.h"
#include <cstring>

namespace babytech { namespace boardlink { namespace detail {

class ByteReader {
public:
    ByteReader(const uint8_t* bytes, size_t length) : bytes_(bytes), length_(length) {
        good_ = bytes != nullptr;
    }
    const uint8_t* take(size_t count) {
        if (!good_ || count > length_ - at_) { good_ = false; return nullptr; }
        const auto* result = bytes_ + at_;
        at_ += count;
        return result;
    }
    uint64_t integer(size_t width) {
        if (width > 8) { good_ = false; return 0; }
        const auto* source = take(width);
        if (!source) return 0;
        uint64_t result = 0;
        for (size_t i = 0; i < width; ++i) result |= uint64_t(source[i]) << (i * 8);
        return result;
    }
    bool text(char* output, size_t capacity) {
        const size_t length = size_t(integer(2));
        const auto* source = take(length);
        if (!source || length >= capacity || !v4::validUtf8(source, length)) {
            good_ = false;
            return false;
        }
        std::memcpy(output, source, length);
        output[length] = 0;
        return true;
    }
    bool done() const { return good_ && at_ == length_; }
private:
    const uint8_t* bytes_;
    size_t length_;
    size_t at_ = 0;
    bool good_ = true;
};

class ByteWriter {
public:
    ByteWriter(uint8_t* bytes, size_t capacity) : bytes_(bytes), capacity_(capacity) {}
    bool raw(const void* data, size_t length) {
        if (!good_ || !bytes_ || length > capacity_ - at_) { good_ = false; return false; }
        if (length) std::memcpy(bytes_ + at_, data, length);
        at_ += length;
        return true;
    }
    bool integer(uint64_t value, size_t width) {
        if (width > 8) { good_ = false; return false; }
        uint8_t bytes[8];
        for (size_t i = 0; i < width; ++i) { bytes[i] = uint8_t(value); value >>= 8; }
        return raw(bytes, width);
    }
    bool text(const char* value) {
        const size_t length = std::strlen(value);
        if (length > UINT16_MAX) { good_ = false; return false; }
        return integer(length, 2) && raw(value, length);
    }
    size_t size() const { return good_ ? at_ : 0; }
private:
    uint8_t* bytes_;
    size_t capacity_;
    size_t at_ = 0;
    bool good_ = true;
};

constexpr size_t kRecordHeaderSize = 12;
inline uint32_t recordCrc(const uint8_t* bytes, size_t length) {
    uint32_t crc = 0xffffffff;
    for (size_t i = 0; i < length; ++i) {
        if (i >= 8 && i < kRecordHeaderSize) continue;
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    return crc ^ 0xffffffff;
}

inline bool recordHeader(const uint8_t* bytes, size_t length, const char* magic) {
    if (!bytes || length < kRecordHeaderSize || std::memcmp(bytes, magic, 4)) return false;
    ByteReader header(bytes + 4, 8);
    return header.integer(2) == 1 && header.integer(2) == length - kRecordHeaderSize &&
        header.integer(4) == recordCrc(bytes, length);
}

inline void finishRecord(uint8_t* bytes, size_t length, const char* magic) {
    ByteWriter header(bytes, kRecordHeaderSize);
    header.raw(magic, 4);
    header.integer(1, 2);
    header.integer(length - kRecordHeaderSize, 2);
    header.integer(recordCrc(bytes, length), 4);
}

} } }
