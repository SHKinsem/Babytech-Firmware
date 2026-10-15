#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace babytech { namespace boardlink { namespace detail {

// ArduinoJson 6 leaves some controls unescaped. Bound the final escaped bytes,
// not measureJson's shorter result. Callers use scratch for atomic output.
struct BoundedJsonWriter {
    uint8_t* output;
    size_t capacity;
    size_t length = 0;
    bool overflow = false;

    size_t write(uint8_t value) {
        const size_t width = value < 0x20 ? 6 : 1;
        if (overflow || width > capacity - length) {
            overflow = true;
            return 0;
        }
        if (width == 6) {
            constexpr char hex[] = "0123456789abcdef";
            std::memcpy(output + length, "\\u00", 4);
            output[length + 4] = hex[value >> 4];
            output[length + 5] = hex[value & 15];
        } else output[length] = value;
        length += width;
        return 1;
    }

    size_t write(const uint8_t* data, size_t size) {
        size_t written = 0;
        while (written < size && write(data[written])) ++written;
        return written;
    }
};

} } }
