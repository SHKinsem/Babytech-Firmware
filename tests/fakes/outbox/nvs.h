#pragma once
#include <cstddef>
#include <cstdint>

using esp_err_t = int;
using nvs_handle_t = uint32_t;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 0x1102;
constexpr esp_err_t ESP_ERR_INVALID_SIZE = 0x104;
constexpr int NVS_READWRITE = 1;
constexpr int NVS_READONLY = 0;
esp_err_t nvs_open(const char*, int, nvs_handle_t*);
esp_err_t nvs_get_str(nvs_handle_t, const char*, char*, size_t*);
esp_err_t nvs_set_str(nvs_handle_t, const char*, const char*);
esp_err_t nvs_erase_key(nvs_handle_t, const char*);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
