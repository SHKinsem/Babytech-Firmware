#pragma once
#include <stdint.h>

namespace motion {
// Persist one versioned blob, not individual keys: settings change atomically.
struct DebugLimits {
    uint32_t version = 1;
    uint32_t maxSpeedTenths = 1200;
    uint32_t maxAccelRpmS = 240;
    uint32_t maxCurrentMa = 5000;
    uint32_t maxAngleTenths = 36000;
    uint32_t maxMoveDurationMs = 60000;
    uint32_t experimentDurationMs = 5000; // 0: user-controlled continuous trial
};
inline bool validDebugLimits(const DebugLimits& v) {
    return v.version == 1 && v.maxSpeedTenths >= 1 && v.maxSpeedTenths <= 30000 &&
        v.maxAccelRpmS >= 1 && v.maxAccelRpmS <= 65535 &&
        v.maxCurrentMa >= 100 && v.maxCurrentMa <= 5000 &&
        v.maxAngleTenths >= 1 && v.maxAngleTenths <= 3600000 &&
        v.maxMoveDurationMs >= 1000 && v.maxMoveDurationMs <= 3600000 &&
        v.maxMoveDurationMs % 1000 == 0 &&
        v.experimentDurationMs <= 3600000 && v.experimentDurationMs % 1000 == 0;
}
} // namespace motion
