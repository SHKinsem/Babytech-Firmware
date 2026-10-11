#pragma once

#include "../brain_state_store/nvs.h"

esp_err_t nvs_set_str(nvs_handle_t, const char*, const char*);
#define MOTION_NVS_INTEGER_DECLARATIONS(suffix, type) \
    esp_err_t nvs_get_##suffix(nvs_handle_t, const char*, type*); \
    esp_err_t nvs_set_##suffix(nvs_handle_t, const char*, type);
MOTION_NVS_INTEGER_DECLARATIONS(i8, int8_t)
MOTION_NVS_INTEGER_DECLARATIONS(u8, uint8_t)
MOTION_NVS_INTEGER_DECLARATIONS(i16, int16_t)
MOTION_NVS_INTEGER_DECLARATIONS(u16, uint16_t)
MOTION_NVS_INTEGER_DECLARATIONS(i32, int32_t)
MOTION_NVS_INTEGER_DECLARATIONS(u32, uint32_t)
MOTION_NVS_INTEGER_DECLARATIONS(i64, int64_t)
MOTION_NVS_INTEGER_DECLARATIONS(u64, uint64_t)
#undef MOTION_NVS_INTEGER_DECLARATIONS
