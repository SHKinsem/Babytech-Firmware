#include "LegacyContextStore.h"

#ifdef ARDUINO
#include <nvs.h>
#include <cstring>

namespace babytech { namespace boardlink {
namespace {
constexpr size_t kMaxEncodedLength = 2047;

bool legacyControl(uint8_t c) {
    return (c >= 1 && c <= 7) || c == 11 || (c >= 14 && c <= 31);
}

// Only old ArduinoJson's literal uncommon controls are repaired, never general
// relaxed JSON. Validate/count before touching the existing 2048-byte buffer.
bool normalizeLegacy(char* bytes, size_t length, size_t& encodedLength) {
    bool quoted = false;
    bool escaped = false;
    encodedLength = 0;
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i) {
        const uint8_t c = bytes[i];
        const bool uncommon = legacyControl(c);
        if (!c || (uncommon && (!quoted || escaped)) ||
            (quoted && c < 0x20 && !uncommon)) return false;
        const size_t width = uncommon ? 6 : 1;
        if (width > kMaxEncodedLength - encodedLength) return false;
        encodedLength += width;
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
    }
    if (quoted) return false;
    if (encodedLength == length) return true;
    // First pass proved every raw uncommon control is unescaped inside a string.
    // Expanding backwards keeps all unread source bytes intact; NVS is untouched.
    size_t at = encodedLength;
    bytes[at] = '\0';
    for (size_t i = length; i > 0;) {
        const uint8_t c = bytes[--i];
        if (legacyControl(c)) {
            at -= 6;
            std::memcpy(bytes + at, "\\u00", 4);
            bytes[at + 4] = hex[c >> 4];
            bytes[at + 5] = hex[c & 15];
        } else bytes[--at] = char(c);
    }
    return true;
}

LegacyContextLoad readContext(nvs_handle_t handle, const char* expectedDeviceId,
                              ProductContext& output) {
    size_t length = 0;
    esp_err_t error = nvs_get_str(handle, "payload", nullptr, &length);
    if (error == ESP_ERR_NVS_NOT_FOUND) return LegacyContextLoad::Missing;
    if (error != ESP_OK) return LegacyContextLoad::IoError;
    constexpr size_t kStoredCapacity = kMaxEncodedLength + 1;  // Includes NUL.
    if (!length || length > kStoredCapacity) return LegacyContextLoad::Corrupt;
    char bytes[kStoredCapacity];
    // A short SDK copy must not accidentally acquire a terminator from the stack.
    std::memset(bytes, 0xff, sizeof(bytes));
    const size_t expectedLength = length;
    error = nvs_get_str(handle, "payload", bytes, &length);
    if (error != ESP_OK || length != expectedLength) return LegacyContextLoad::IoError;
    if (bytes[length - 1] != '\0' || std::memchr(bytes, '\0', length - 1))
        return LegacyContextLoad::Corrupt;
    size_t encodedLength;
    if (!normalizeLegacy(bytes, length - 1, encodedLength)) return LegacyContextLoad::Corrupt;
    return decodeProductContext(reinterpret_cast<const uint8_t*>(bytes), encodedLength,
                                expectedDeviceId, output)
        ? LegacyContextLoad::Ready : LegacyContextLoad::Corrupt;
}
}

LegacyContextLoad loadLegacyProductContext(const char* expectedDeviceId,
                                          ProductContext& output) {
    nvs_handle_t handle;
    const esp_err_t error = nvs_open("productctx", NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return LegacyContextLoad::Missing;
    if (error != ESP_OK) return LegacyContextLoad::IoError;
    const auto result = readContext(handle, expectedDeviceId, output);
    nvs_close(handle);
    return result;
}

} }
#endif
