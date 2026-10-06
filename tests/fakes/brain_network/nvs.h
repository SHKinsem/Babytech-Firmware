#pragma once
#include <cstddef>
#include <cstdint>

using esp_err_t = int32_t;
using nvs_handle_t = uint32_t;
enum nvs_open_mode_t { NVS_READONLY, NVS_READWRITE };
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 0x1102;
constexpr esp_err_t ESP_ERR_NVS_INVALID_LENGTH = 0x110c;

esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* handle);
esp_err_t nvs_get_str(nvs_handle_t handle, const char* key, char* output, size_t* length);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_set_str(nvs_handle_t handle, const char* key, const char* value);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key);
esp_err_t nvs_erase_all(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);
