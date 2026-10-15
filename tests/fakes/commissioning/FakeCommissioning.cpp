#include "FakeCommissioning.h"
#include "esp_mac.h"
#include <cstring>

namespace fake_commissioning {
std::array<uint8_t, 6> mac;
unsigned macCalls = 0, failMacCall = 0, stringQueries = 0, stringReads = 0;
void reset() {
    mac = {{0x01, 0x23, 0x45, 0xab, 0xcd, 0xef}};
    macCalls = failMacCall = stringQueries = stringReads = 0;
}
}

esp_err_t esp_read_mac(uint8_t* mac, esp_mac_type_t type) {
    fake_brain::check(mac && type == ESP_MAC_WIFI_STA, "invalid MAC request");
    if (++fake_commissioning::macCalls == fake_commissioning::failMacCall) return ESP_FAIL;
    std::memcpy(mac, fake_commissioning::mac.data(), 6);
    return ESP_OK;
}

// STRING operations share the real fault backend's call sequence, handles,
// persisted bytes and faults. No replacement blob/write/commit implementation.
esp_err_t nvs_get_str(nvs_handle_t handle, const char* key, char* out, size_t* length) {
    using namespace fake_brain;
    const auto found = io.handles.find(handle);
    check(found != io.handles.end() && key && length, "invalid string read");
    auto& opened = found->second;
    const size_t capacity = *length;
    check(!out || capacity <= 2048, "unbounded legacy string read");
    if (out) ++fake_commissioning::stringReads;
    else ++fake_commissioning::stringQueries;
    const Op op = out ? Op::Read : Op::Query;
    const Call call{op, count(op) + 1, opened.name, key, handle};
    io.calls.push_back(call);
    if (io.before) io.before(call);
    std::optional<Fault> fault;
    for (auto& candidate : io.faults) {
        if (candidate.op == op && candidate.occurrence == call.occurrence) {
            check(!candidate.hit, "string fault applied twice");
            candidate.hit = true;
            fault = candidate;
        }
    }
    if (fault && fault->error != ESP_OK) {
        if (out) std::memset(out, 0xe5, capacity);
        return fault->error;
    }
    const auto space = io.disk.find(opened.name);
    const auto pending = opened.pending.find(key);
    const Value* value = pending != opened.pending.end() ? &pending->second : nullptr;
    if (!value && space != io.disk.end()) {
        const auto stored = space->second.find(key);
        if (stored != space->second.end()) value = &stored->second;
    }
    if (!value) return ESP_ERR_NVS_NOT_FOUND;
    if (value->type != Type::String) return ESP_ERR_NVS_TYPE_MISMATCH;
    *length = fault && fault->reportedLength ? *fault->reportedLength : value->bytes.size();
    if (!out) return ESP_OK;
    if (capacity < value->bytes.size()) return ESP_ERR_NVS_INVALID_LENGTH;
    if (!value->bytes.empty()) std::memcpy(out, value->bytes.data(), value->bytes.size());
    return ESP_OK;
}
