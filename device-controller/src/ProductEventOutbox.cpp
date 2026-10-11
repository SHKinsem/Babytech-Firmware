#include "ProductEventOutbox.h"
#include "RetryDeadline.h"

#include <nvs.h>
#include <esp_system.h>

namespace {
constexpr char kNamespace[] = "formulaevt";
constexpr char kPayloadKey[] = "payload";
constexpr uint32_t kRetryMs = 5000;
}

bool NvsProductEventStorage::read(std::string& value) {
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    size_t length = 0;
    esp_err_t result = nvs_get_str(handle, kPayloadKey, nullptr, &length);
    value.clear();
    if (result == ESP_OK && length > 1 && length <= 2048) {
        value.resize(length);
        result = nvs_get_str(handle, kPayloadKey, &value[0], &length);
        if (result == ESP_OK) value.resize(length - 1);
    } else if (result == ESP_OK) {
        result = ESP_ERR_INVALID_SIZE;
    }
    nvs_close(handle);
    return result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND;
}

bool NvsProductEventStorage::write(const std::string& value) {
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    const bool saved = nvs_set_str(handle, kPayloadKey, value.c_str()) == ESP_OK &&
        nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return saved;
}

bool NvsProductEventStorage::clear() {
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    const esp_err_t result = nvs_erase_key(handle, kPayloadKey);
    const bool removed = (result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND) &&
        nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return removed;
}

ProductEventOutbox::LegacyState ProductEventOutbox::inspectLegacyState() {
    nvs_handle_t handle;
    const esp_err_t opened = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (opened == ESP_ERR_NVS_NOT_FOUND) return LegacyState::Empty;
    if (opened != ESP_OK) return LegacyState::ReadError;
    size_t length = 0;
    const esp_err_t read = nvs_get_str(handle, kPayloadKey, nullptr, &length);
    nvs_close(handle);
    if (read == ESP_ERR_NVS_NOT_FOUND) return LegacyState::Empty;
    if (read != ESP_OK || length <= 1 || length > 2048) return LegacyState::ReadError;
    // Even an unparseable legacy record is evidence, never permission to erase
    // or initialize a new run. The paired legacy firmware settles it first.
    return LegacyState::Present;
}

void ProductEventOutbox::begin(const String& deviceId, motion::ProductSession& product) {
    char bootToken[17];
    snprintf(bootToken, sizeof(bootToken), "%08lx%08lx",
             static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()));
    if (!state_.begin(deviceId.c_str(), bootToken))
        Serial.println("[cloud] event storage unavailable or invalid; product start blocked");
    product.setStartGuard(&state_);
    product.setEventPending(state_.blocked());
    if (state_.blocked()) product.recoverAfterRestart(millis());
    retryAt_ = millis();
}

bool ProductEventOutbox::queue(const motion::ProductTerminal& terminal, uint32_t now) {
    retryAt_ = now;
    return state_.queue(terminal, now);
}

bool ProductEventOutbox::receiveReceipt(JsonVariantConst receipt, motion::ProductSession& product) {
    // Preserve recovery evidence through another MCU reset if Stop is unconfirmed.
    // Cloud will re-ack the next replay after all axes are proven stationary.
    if (!product.pendingEventSettled()) return false;
    if (!state_.receiveReceipt(receipt)) return false;
    product.setEventPending(false);
    return true;
}

void ProductEventOutbox::poll(CloudLink& cloud, motion::ProductSession& product, uint32_t now) {
    // Transport success is not a DB receipt. Drain results without clearing NVS.
    CloudLink::PublishResult result;
    while (cloud.takePublishResult(result)) {}
    if (!state_.pending() || !motion::retryDue(now, retryAt_)) return;
    retryAt_ = now + kRetryMs;
    product.setEventPending(true);
    // Retry flash independently of connectivity; an offline terminal must be durable.
    if (!state_.persistPending() || !cloud.connected()) return;
    cloud.publish("event", String(state_.payload().c_str()));
}
