#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_err.h"

using nvs_handle_t = uint32_t;
enum nvs_open_mode_t { NVS_READONLY = 0, NVS_READWRITE = 1 };
esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* output, size_t* length);
void nvs_close(nvs_handle_t handle);
// Traps: these must never be called by the read-only adapter.
esp_err_t nvs_set_blob(nvs_handle_t, const char*, const void*, size_t);
esp_err_t nvs_erase_key(nvs_handle_t, const char*);
esp_err_t nvs_erase_all(nvs_handle_t);
esp_err_t nvs_commit(nvs_handle_t);
