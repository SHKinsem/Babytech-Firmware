#pragma once

#include "../brain_state_store/nvs.h"
esp_err_t nvs_set_str(nvs_handle_t, const char*, const char*);
