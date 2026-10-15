#include "FakeLegacyNvs.h"
#include "nvs_flash.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>

namespace fake {
State io;
void reset() { io = State{}; }
void verify() {
    assert(!io.handleOpen);
    for (const auto& fault : io.faults) assert(fault.used);
}
size_t count(Op op) { return std::count(io.calls.begin(), io.calls.end(), op); }
Fault* call(Op op) {
    io.calls.push_back(op);
    for (auto& fault : io.faults) {
        if (fault.op == op && !fault.used) {
            fault.used = true;
            return &fault;
        }
    }
    return nullptr;
}
}  // namespace fake

esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* handle) {
    assert(std::strcmp(name, "productctx") == 0);
    // Forbidden APIs abort even if assertions are accidentally disabled.
    if (mode != NVS_READONLY) std::abort();
    assert(handle && !fake::io.handleOpen);
    auto* fault = fake::call(fake::Op::Open);
    if (fault && fault->error != ESP_OK) return fault->error;
    if (!fake::io.disk.count(name)) return ESP_ERR_NVS_NOT_FOUND;
    fake::io.handleOpen = true;
    *handle = 17;
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char* key, char* output, size_t* length) {
    assert(handle == 17 && fake::io.handleOpen && length);
    assert(std::strcmp(key, "payload") == 0);
    auto* fault = fake::call(output ? fake::Op::Read : fake::Op::Query);
    if (fault && fault->error != ESP_OK) return fault->error;
    const auto& space = fake::io.disk.at("productctx");
    const auto found = space.find(key);
    if (found == space.end()) return ESP_ERR_NVS_NOT_FOUND;
    if (found->second.type != fake::Type::String) return ESP_ERR_NVS_TYPE_MISMATCH;
    const auto& bytes = found->second.bytes;
    if (output) {
        if (*length < bytes.size()) return ESP_ERR_NVS_INVALID_LENGTH;
        const size_t copied = fault ? std::min(bytes.size(), fault->copyLimit) : bytes.size();
        if (copied) std::memcpy(output, bytes.data(), copied);
    }
    *length = fault && fault->overrideLength ? fault->length : bytes.size();
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) {
    assert(handle == 17 && fake::io.handleOpen);
    fake::call(fake::Op::Close);
    fake::io.handleOpen = false;
}

esp_err_t nvs_get_blob(nvs_handle_t, const char*, void*, size_t*) { std::abort(); }
esp_err_t nvs_set_str(nvs_handle_t, const char*, const char*) { std::abort(); }
esp_err_t nvs_set_blob(nvs_handle_t, const char*, const void*, size_t) { std::abort(); }
esp_err_t nvs_commit(nvs_handle_t) { std::abort(); }
esp_err_t nvs_erase_key(nvs_handle_t, const char*) { std::abort(); }
esp_err_t nvs_erase_all(nvs_handle_t) { std::abort(); }
esp_err_t nvs_flash_erase() { std::abort(); }
esp_err_t nvs_flash_erase_partition(const char*) { std::abort(); }
