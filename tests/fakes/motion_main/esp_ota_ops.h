#pragma once

#include <esp_err.h>
#include "esp_partition.h"

enum esp_ota_img_states_t {
    ESP_OTA_IMG_NEW = 0,
    ESP_OTA_IMG_PENDING_VERIFY = 1,
    ESP_OTA_IMG_VALID = 2,
    ESP_OTA_IMG_INVALID = 3,
    ESP_OTA_IMG_ABORTED = 4,
    ESP_OTA_IMG_UNDEFINED = -1
};

namespace fake_motion_ota {
inline esp_ota_img_states_t imageState = ESP_OTA_IMG_VALID;
inline esp_err_t stateResult = ESP_OK;
inline unsigned confirmCalls = 0;
inline unsigned rollbackCalls = 0;
} // namespace fake_motion_ota

inline const esp_partition_t* esp_ota_get_running_partition() {
    return fake_motion_ota::runningAvailable ? &fake_motion_ota::runningPartition : nullptr;
}

inline const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*) {
    return fake_motion_ota::updateAvailable ? &fake_motion_ota::updatePartition : nullptr;
}

inline esp_err_t esp_ota_get_state_partition(const esp_partition_t* partition,
                                             esp_ota_img_states_t* state) {
    if (!partition || partition != esp_ota_get_running_partition() || !state) return ESP_FAIL;
    if (fake_motion_ota::stateResult == ESP_OK) *state = fake_motion_ota::imageState;
    return fake_motion_ota::stateResult;
}

inline esp_err_t esp_ota_mark_app_valid_cancel_rollback() {
    ++fake_motion_ota::confirmCalls;
    return ESP_FAIL; // No real bootloader/NVS confirmation was performed.
}

inline esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot() {
    ++fake_motion_ota::rollbackCalls;
    return ESP_FAIL; // No real partition switch or reboot was performed.
}
