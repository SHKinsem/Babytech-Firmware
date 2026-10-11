#pragma once

#include <cstddef>

namespace motion {

constexpr size_t kCloudDeviceIdCapacity = 65;
// "devices/" + 64-byte ID + "/" + NUL.
constexpr size_t kCloudTopicPrefixCapacity = 8 + 64 + 1 + 1;
constexpr size_t kCloudTopicCapacity = 128;

inline bool validCloudDeviceId(const char* value, size_t length) {
    if (!value || length == 0 || length >= kCloudDeviceIdCapacity) return false;
    for (size_t i = 0; i < length; ++i) {
        const char c = value[i];
        const bool alphanumeric = (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alphanumeric && (i == 0 || (c != '_' && c != '-'))) return false;
    }
    return true;
}

}  // namespace motion
