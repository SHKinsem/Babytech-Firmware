#pragma once

#include <cstdint>
#include "esp_err.h"

enum esp_mac_type_t { ESP_MAC_WIFI_STA = 0 };
esp_err_t esp_read_mac(uint8_t* mac, esp_mac_type_t type);
