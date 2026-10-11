#pragma once
#include "esp_err.h"
esp_err_t nvs_flash_init();
esp_err_t nvs_flash_erase();
esp_err_t nvs_flash_erase_partition(const char*);
