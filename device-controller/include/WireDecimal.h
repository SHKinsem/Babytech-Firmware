#pragma once

#include <stddef.h>
#include <stdint.h>

namespace motion {

// Decimal text to an exact unsigned 0.1-unit wire field. A leading minus is
// carried separately for CD direction; sub-tenth nonzero digits cannot be
// encoded and are rejected rather than rounded.
inline bool parseWireTenths(const char* raw, size_t length, uint32_t maximum,
                            bool signedValue, uint32_t& out, bool& negative) {
    if (!raw || length == 0 || length > 32) return false;
    size_t i = 0;
    negative = false;
    if (raw[0] == '-' || raw[0] == '+') {
        if (!signedValue && raw[0] == '-') return false;
        negative = raw[0] == '-';
        i = 1;
    }
    if (i == length) return false;
    bool anyDigit = false;
    bool decimal = false;
    bool hasTenth = false;
    const uint32_t maximumWhole = maximum / 10;
    uint32_t whole = 0;
    uint8_t tenth = 0;
    for (; i < length; ++i) {
        const char c = raw[i];
        if (c == '.') {
            if (decimal) return false;
            decimal = true;
            continue;
        }
        if (c < '0' || c > '9') return false;
        anyDigit = true;
        const uint8_t digit = static_cast<uint8_t>(c - '0');
        if (!decimal) {
            if (whole > maximumWhole / 10 ||
                (whole == maximumWhole / 10 && digit > maximumWhole % 10))
                return false;
            whole = whole * 10 + digit;
        } else if (!hasTenth) {
            tenth = digit;
            hasTenth = true;
        } else if (digit != 0) {
            return false;
        }
    }
    if (!anyDigit) return false;
    const uint64_t scaled = static_cast<uint64_t>(whole) * 10 + tenth;
    if (scaled > maximum) return false;
    out = static_cast<uint32_t>(scaled);
    return true;
}

}  // namespace motion
