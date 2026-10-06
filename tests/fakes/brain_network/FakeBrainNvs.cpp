#include "FakeBrainNvs.h"
#include "FakeCloudIo.h"
#include <cstring>

namespace fake {
BrainNvs nvs;
void BrainNvs::seed(const std::string& key, const std::string& value) {
    values[key] = std::vector<char>(value.begin(), value.end());
    values[key].push_back(0);
}
}
namespace {
void record(const char* operation, const char* key = "") {
    fake::nvs.calls.push_back({operation, key, fake::io.inWorker});
}
void validHandle(nvs_handle_t handle) {
    fake::check(handle == 1 && fake::nvs.opened, "invalid/closed raw NVS handle");
}
esp_err_t rejectWrite() {
    ++fake::nvs.writes;
    fake::check(false, "read-only Brain attempted NVS write/erase/commit");
    return ESP_FAIL;
}
}
esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* handle) {
    record("open", name);
    fake::check(std::strcmp(name, "wifi-cfg") == 0, "station read wrong NVS namespace");
    fake::check(mode == NVS_READONLY || fake::nvs.allowWrites, "station opened NVS for writing");
    fake::check(!fake::nvs.opened, "raw NVS handle already open");
    if (fake::nvs.openError != ESP_OK) return fake::nvs.openError;
    if (!fake::nvs.exists && mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
    fake::nvs.exists = true;
    fake::nvs.writable = mode == NVS_READWRITE;
    *handle = 1;
    fake::nvs.opened = true;
    return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t handle, const char* key, char* output, size_t* length) {
    validHandle(handle);
    record("get", key);
    const auto error = fake::nvs.readErrors.find(key);
    if (error != fake::nvs.readErrors.end()) return error->second;
    const auto found = fake::nvs.values.find(key);
    if (found == fake::nvs.values.end()) return ESP_ERR_NVS_NOT_FOUND;
    const size_t capacity = *length;
    const auto reported = fake::nvs.reportedLengths.find(key);
    *length = reported == fake::nvs.reportedLengths.end() ? found->second.size() : reported->second;
    if (!output) return ESP_OK;
    if (capacity < found->second.size()) return ESP_ERR_NVS_INVALID_LENGTH;
    std::memcpy(output, found->second.data(), found->second.size());
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) {
    validHandle(handle);
    record("close");
    fake::nvs.opened = false;
}
esp_err_t nvs_set_str(nvs_handle_t handle, const char* key, const char* value) {
    validHandle(handle);
    if (!fake::nvs.allowWrites || !fake::nvs.writable) return rejectWrite();
    record("set", key);
    ++fake::nvs.writes;
    fake::nvs.seed(key, value);
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t, const char*) { return rejectWrite(); }
esp_err_t nvs_erase_all(nvs_handle_t) { return rejectWrite(); }
esp_err_t nvs_commit(nvs_handle_t handle) {
    validHandle(handle);
    if (!fake::nvs.allowWrites || !fake::nvs.writable) return rejectWrite();
    record("commit");
    ++fake::nvs.writes;
    return ESP_OK;
}
