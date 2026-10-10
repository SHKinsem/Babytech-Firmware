#pragma once
#include <esp_err.h>
#include <cstddef>
#include <cstdint>
namespace motion_io { inline uint32_t randomCounter = 0x16543; }
inline uint32_t esp_random() { return ++motion_io::randomCounter; }
inline void esp_fill_random(void* p, size_t n) { auto* b = static_cast<uint8_t*>(p); while (n--) *b++ = uint8_t(esp_random()); }
inline int esp_reset_reason() { return 1; }
