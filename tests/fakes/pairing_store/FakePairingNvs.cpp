#include "FakePairingNvs.h"
#include "esp_mac.h"
#include "nvs_flash.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace fake {
State io;
void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "Fake NVS contract: %s\n", message);
        std::abort();
    }
}
void reset() {
    check(io.handles.empty(), "leaked handle before reset");
    io = State{};
}
void reboot() {
    Database disk = io.disk;
    const auto mac = io.mac;
    const bool early = io.durableOnSet;
    io = State{};
    io.disk = std::move(disk);
    io.mac = mac;
    io.durableOnSet = early;
}
unsigned count(Op op) {
    return unsigned(std::count_if(io.calls.begin(), io.calls.end(),
                                  [op](const Call& call) { return call.op == op; }));
}
void fail(Op op, unsigned occurrence, esp_err_t error, bool apply,
          std::optional<size_t> reportedLength) {
    io.faults.push_back({op, occurrence, error, apply, reportedLength, false});
}
void verifyFaults() {
    for (const auto& fault : io.faults) check(fault.hit, "injected fault never reached");
}
}  // namespace fake

namespace {
using namespace fake;
std::optional<Fault> enter(Op op, const std::string& name = {},
                           const std::string& key = {}, nvs_handle_t handle = 0) {
    const Call call{op, count(op) + 1, name, key, handle};
    io.calls.push_back(call);
    if (io.before) io.before(call);
    for (auto& fault : io.faults) {
        if (fault.op == op && fault.occurrence == call.occurrence) {
            check(!fault.hit, "fault applied twice");
            fault.hit = true;
            return fault;
        }
    }
    return std::nullopt;
}
Handle& getHandle(nvs_handle_t handle, bool write = false) {
    auto found = io.handles.find(handle);
    check(found != io.handles.end(), "invalid/closed handle");
    check(!write || found->second.mode == NVS_READWRITE, "mutation on read-only handle");
    return found->second;
}
[[noreturn]] esp_err_t forbiddenErase() {
    enter(Op::Erase);
    check(false, "erase is forbidden");
    std::abort();
}
}  // namespace

esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* out) {
    check(name && out, "null open argument");
    check(mode == NVS_READONLY || mode == NVS_READWRITE, "invalid open mode");
    const auto fault = enter(mode == NVS_READONLY ? Op::OpenRO : Op::OpenRW, name);
    if (fault && fault->error != ESP_OK) return fault->error;
    if (!io.disk.count(name)) {
        if (mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
        io.disk[name] = {};
    }
    *out = io.nextHandle++;
    io.handles.emplace(*out, Handle{name, mode, {}});
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* out, size_t* length) {
    auto& opened = getHandle(handle);
    check(key && length, "null read argument");
    const size_t capacity = *length;
    const auto fault = enter(out ? Op::Read : Op::Query, opened.name, key, handle);
    if (fault && fault->error != ESP_OK) {
        if (out) std::memset(out, 0xe5, capacity);
        return fault->error;
    }
    const auto pending = opened.pending.find(key);
    const auto& values = io.disk.at(opened.name);
    const auto stored = values.find(key);
    const Value* value = pending != opened.pending.end() ? &pending->second :
        (stored != values.end() ? &stored->second : nullptr);
    if (!value) return ESP_ERR_NVS_NOT_FOUND;
    if (value->type != Type::Blob) return ESP_ERR_NVS_TYPE_MISMATCH;
    *length = fault && fault->reportedLength ? *fault->reportedLength : value->bytes.size();
    if (!out) return ESP_OK;
    if (capacity < value->bytes.size()) return ESP_ERR_NVS_INVALID_LENGTH;
    if (!value->bytes.empty()) std::memcpy(out, value->bytes.data(), value->bytes.size());
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* data, size_t length) {
    auto& opened = getHandle(handle, true);
    check(key && data && length, "invalid set argument");
    const auto fault = enter(Op::Set, opened.name, key, handle);
    if (!fault || fault->error == ESP_OK || fault->apply) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        const Value value{Bytes(bytes, bytes + length), Type::Blob};
        opened.pending[key] = value;
        // A failed API call can still have changed persistent storage.
        if (io.durableOnSet || (fault && fault->apply)) io.disk[opened.name][key] = value;
    }
    return fault ? fault->error : ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle) {
    auto& opened = getHandle(handle, true);
    const auto fault = enter(Op::Commit, opened.name, {}, handle);
    if (!fault || fault->error == ESP_OK || fault->apply) {
        for (const auto& entry : opened.pending) io.disk[opened.name][entry.first] = entry.second;
        opened.pending.clear();
    }
    return fault ? fault->error : ESP_OK;
}

void nvs_close(nvs_handle_t handle) {
    const auto name = getHandle(handle).name;
    enter(Op::Close, name, {}, handle);
    io.handles.erase(handle);
}

esp_err_t esp_read_mac(uint8_t* mac, esp_mac_type_t type) {
    check(mac && type == ESP_MAC_WIFI_STA, "invalid MAC request");
    const auto fault = enter(Op::Mac);
    if (fault && fault->error != ESP_OK) return fault->error;
    std::memcpy(mac, io.mac.data(), io.mac.size());
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t, const char*) { return forbiddenErase(); }
esp_err_t nvs_erase_all(nvs_handle_t) { return forbiddenErase(); }
esp_err_t nvs_flash_erase() { return forbiddenErase(); }
esp_err_t nvs_flash_erase_partition(const char*) { return forbiddenErase(); }
