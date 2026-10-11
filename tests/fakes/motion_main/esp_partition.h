#pragma once

#include <cstddef>
#include <cstdint>

struct esp_partition_t {
    uint32_t address;
    uint32_t size;
    char label[17];
};

namespace fake_motion_ota {
// Metadata fixtures only. These do not model flash layout, writes or bootloader.
inline esp_partition_t runningPartition{0x10000, 0x400000, "host-running"};
inline esp_partition_t updatePartition{0x410000, 0x400000, "host-update"};
inline bool runningAvailable = true;
inline bool updateAvailable = false;
} // namespace fake_motion_ota

