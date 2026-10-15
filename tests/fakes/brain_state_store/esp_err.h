#pragma once
#include <cstdint>

using esp_err_t = int32_t;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 0x1102;
constexpr esp_err_t ESP_ERR_NVS_TYPE_MISMATCH = 0x1103;
constexpr esp_err_t ESP_ERR_NVS_NOT_ENOUGH_SPACE = 0x1105;
constexpr esp_err_t ESP_ERR_NVS_REMOVE_FAILED = 0x1108;
constexpr esp_err_t ESP_ERR_NVS_INVALID_LENGTH = 0x110c;
