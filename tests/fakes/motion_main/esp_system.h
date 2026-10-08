#pragma once
#include <esp_err.h>
#include <cstddef>
#include <cstdint>
inline uint32_t esp_random() { static uint32_t n = 0x16543; return ++n; }
inline void esp_fill_random(void* p, size_t n) { auto* b = static_cast<uint8_t*>(p); while (n--) *b++ = uint8_t(esp_random()); }
inline int esp_reset_reason() { return 1; }
