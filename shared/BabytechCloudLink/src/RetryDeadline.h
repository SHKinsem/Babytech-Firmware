#pragma once

#include <cstdint>

namespace motion {

// Valid when retry intervals are shorter than half of millis()' wrap period.
inline bool retryDue(uint32_t now, uint32_t dueAt) {
    return static_cast<int32_t>(now - dueAt) >= 0;
}

}  // namespace motion
