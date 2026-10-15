#include "brain_station.h"
#include "WiFi.h"
#include "nvs.h"
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

using babytech::brain::BrainStation;

namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct Nvs {
    std::map<std::string, std::string> values;
    std::string fault;
    bool opened = false;
    bool writeStarted = false;
    nvs_open_mode_t mode = NVS_READONLY;
    unsigned sets = 0;
    unsigned commits = 0;
    unsigned opens = 0;
    unsigned gets = 0;
    std::function<void()> onSet;
} nvs;
bool worker = false;
uint32_t now = 1000;
unsigned cases = 0;
std::function<void()> onRadio;

void reset(bool seeded = true) {
    nvs = Nvs{};
    nvs.values["unrelated"] = "preserved";
    if (seeded) {
        nvs.values["ssid"] = "old-network";
        nvs.values["pass"] = "old-password";
    }
    WiFi = FakeWiFi{};
    worker = false;
    now = 1000;
    onRadio = nullptr;
}
void poll(BrainStation& station) {
    const bool previous = worker;
    worker = true;
    station.poll();
    worker = previous;
}
size_t calls(const char* method) {
    size_t count = 0;
    for (const auto& call : WiFi.calls) {
        check(call.worker, "caller touched radio");
        if (call.method == method) ++count;
        if (call.method == "disconnect")
            check(!call.first && !call.second, "disconnect turned off radio or erased config");
    }
    return count;
}
void connected() {
    WiFi.state = WL_CONNECTED;
    WiFi.ssid = WiFi.attemptedSsid;
    WiFi.address = IPAddress(1);
}
bool configure(BrainStation& station, const char* ssid, const char* password) {
    const auto before = WiFi.calls.size();
    const bool result = station.configure(ssid, password);
    check(WiFi.calls.size() == before, "configure performed radio I/O");
    check(!nvs.opened, "NVS handle leaked");
    check(nvs.values.at("unrelated") == "preserved", "unrelated key changed");
    return result;
}
void validation() {
    const std::string unicode = "\xe5\xa5\xb6";
    std::string maxUnicode;
    for (unsigned i = 0; i < 10; ++i) maxUnicode += unicode;
    maxUnicode += "ab"; // Ten 3-byte UTF-8 characters plus two ASCII bytes.
    const std::pair<std::string, bool> ssids[] = {
        {"", false}, {"x", true}, {std::string(32, 's'), true},
        {std::string(33, 's'), false}, {maxUnicode, true}, {maxUnicode + "x", false}};
    const std::pair<std::string, bool> passwords[] = {
        {"", true}, {"1234567", false}, {"12345678", true},
        {std::string(63, 'p'), true}, {std::string(64, 'a'), true},
        {std::string(32, 'F') + std::string(32, '0'), true},
        {std::string(64, 'g'), false}, {std::string(65, 'a'), false},
        {unicode + unicode, false}, {unicode + unicode + unicode, true}};
    for (const auto& ssid : ssids) for (const auto& password : passwords) {
        reset(false);
        BrainStation station;
        const bool valid = ssid.second && password.second;
        check(configure(station, ssid.first.c_str(), password.first.c_str()) == valid,
              "credential validation mismatch");
        if (!valid) check(nvs.opens == 0, "invalid input reached NVS");
        else check(nvs.sets == 2 && nvs.commits == 1, "save retried or skipped writes");
        poll(station);
        check(calls("begin") == (valid ? 1u : 0u), "invalid reload or missing reload");
        if (valid) {
            check(WiFi.attemptedSsid == ssid.first && WiFi.attemptedPassword == password.first,
                  "saved bytes changed");
            for (const auto& call : WiFi.calls) if (call.method == "security")
                check(call.first == (password.first.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK),
                      "wrong open/PSK security");
        }
        ++cases;
    }
    reset(false);
    BrainStation station;
    check(!configure(station, nullptr, "") && !configure(station, "ok", nullptr),
          "null credentials accepted");
    char ssid[33];
    char password[65];
    std::memset(ssid, 's', sizeof(ssid));
    std::memset(password, 'a', sizeof(password));
    check(!configure(station, ssid, "") && !configure(station, "ok", password),
          "unterminated bounded input accepted");
    check(nvs.opens == 0, "invalid bounded input reached NVS");
    ++cases;
}
void reloads() {
    reset();
    BrainStation station;
    poll(station);
    connected();
    check(configure(station, "next-network", "next-password"), "save failed");
    check(calls("begin") == 1 && calls("disconnect") == 0, "caller scheduled radio synchronously");
    poll(station);
    check(calls("begin") == 2 && calls("disconnect") == 1, "reload did not immediately rejoin");
    check(calls("mode") == 1, "reload reset radio mode");
    check(WiFi.attemptedSsid == "next-network", "wrong reload");
    connected();
    poll(station);
    check(calls("begin") == 2 && calls("disconnect") == 1, "reload repeated");
    check(configure(station, "next-network", "changed-password"), "password-only save failed");
    poll(station);
    check(calls("begin") == 3 && WiFi.attemptedPassword == "changed-password",
          "connected SSID suppressed password change");
    check(configure(station, "superseded", "") && configure(station, "latest", ""),
          "coalesced saves failed");
    poll(station);
    check(calls("begin") == 4 && WiFi.attemptedSsid == "latest", "latest save did not win");
    BrainStation reboot;
    poll(reboot);
    check(WiFi.attemptedSsid == "latest", "new instance did not load persisted credentials");
    ++cases;
}
void failures() {
    for (const char* fault : {"open-write", "set-ssid", "set-pass", "commit", "open-read",
                             "get-ssid", "get-pass", "mismatch-ssid", "mismatch-pass",
                             "bad-length", "embedded-nul"}) {
        for (unsigned scenario = 0; scenario < 4; ++scenario) {
            reset(scenario != 2);
            BrainStation station;
            // 0: connected; 1: a previous successful save still pending;
            // 2: no baseline; 3: caller runs before the first worker poll.
            if (scenario != 3) poll(station);
            if (scenario == 0) connected();
            if (scenario == 1) check(configure(station, "verified", "verified-password"),
                                      "previous save failed");
            nvs.fault = fault;
            nvs.writeStarted = false; // Readback faults must not corrupt the initial baseline read.
            const unsigned sets = nvs.sets;
            const unsigned commits = nvs.commits;
            check(!configure(station, "failed-network", "failed-password"), "fault reported success");
            check(nvs.sets - sets <= 2 && nvs.commits - commits <= 1, "caller retried failed persistence");
            if (nvs.fault == "set-pass")
                check(nvs.values.at("ssid") == "failed-network", "fake hid partial multi-key write");
            nvs.fault.clear();
            poll(station);
            if (scenario == 0) check(calls("begin") == 1 && calls("disconnect") == 0,
                                     "failed save disrupted connection");
            if (scenario == 1) check(WiFi.attemptedSsid == "verified" && calls("begin") == 2,
                                     "failed save poisoned previous pending success");
            if (scenario == 2) check(calls("begin") == 0, "failed save activated absent config");
            if (scenario == 3)
                check(WiFi.attemptedSsid == "old-network", "first poll loaded partially written keys");
            if (scenario == 0 || scenario == 1) {
                WiFi.state = 0;
                now += 31000;
                poll(station);
                check(WiFi.attemptedSsid == (scenario == 0 ? "old-network" : "verified"),
                      "normal retry used failed credentials");
            }
            check(configure(station, "repaired", ""), "save failure permanently gated configuration");
            poll(station);
            check(WiFi.attemptedSsid == "repaired", "failed save could not recover");
            ++cases;
        }
    }
}
void interleavings() {
    reset();
    BrainStation station;
    poll(station);
    connected();
    check(configure(station, "pending", ""), "pending save failed");
    bool ran = false;
    nvs.onSet = [&] {
        nvs.onSet = nullptr;
        ran = true;
        check(!station.configure("competing", ""), "busy caller did not return false");
        poll(station); // A pending reload while another save owns NVS must defer.
        check(calls("begin") == 1 && calls("disconnect") == 0, "worker read during write");
    };
    check(station.configure("latest", ""), "outer save failed");
    check(ran, "write interleaving did not run");
    poll(station);
    check(WiFi.attemptedSsid == "latest", "deferred worker lost latest reload");
    onRadio = [&] {
        onRadio = nullptr;
        check(station.configure("radio-callback", ""), "config lock held over radio");
    };
    check(configure(station, "before-callback", ""), "pre-callback save failed");
    poll(station);
    check(WiFi.attemptedSsid == "before-callback", "caller mutated worker radio buffers");
    poll(station);
    check(WiFi.attemptedSsid == "radio-callback", "reload published during radio I/O lost");
    ++cases;
}
void durabilityAndRecovery() {
    reset();
    BrainStation station;
    poll(station);
    connected();
    nvs.fault = "set-pass";
    check(!configure(station, "partial", "new-password"), "partial save accepted");
    poll(station);
    check(calls("begin") == 1, "failed save reconnected");
    nvs.fault.clear();
    BrainStation reboot;
    poll(reboot);
    check(WiFi.attemptedSsid == "partial" && WiFi.attemptedPassword == "old-password",
          "test must expose the documented legacy multi-key durability limit");
    check(configure(reboot, "repaired", ""), "partial pair could not be repaired");
    poll(reboot);
    check(WiFi.attemptedSsid == "repaired" && WiFi.attemptedPassword.empty(), "repair not applied");
    ++cases;

    reset();
    nvs.values["ssid"] = std::string(40, 'x');
    BrainStation corrupt;
    poll(corrupt);
    check(calls("begin") == 0, "corrupt boot config used");
    check(configure(corrupt, "repaired", ""), "corrupt baseline gated repair");
    poll(corrupt);
    check(calls("begin") == 1, "corrupt baseline recovery failed");
    ++cases;
}
void retryTiming() {
    for (uint32_t start : {1000u, UINT32_MAX - 5000u}) {
        reset();
        now = start;
        BrainStation station;
        poll(station);
        now = start + 14999u;
        poll(station);
        check(calls("disconnect") == 0, "join timed out early");
        now = start + 15000u;
        poll(station);
        check(calls("disconnect") == 1 && calls("begin") == 1, "timeout/retry changed");
        now = start + 30000u;
        poll(station);
        check(calls("begin") == 2, "normal offline retry broken");
        check(configure(station, "immediate", ""), "save during backoff failed");
        poll(station);
        check(calls("begin") == 3 && WiFi.attemptedSsid == "immediate", "reload waited for retry timer");
        ++cases;
    }
}
} // namespace

FakeWiFi WiFi;
uint32_t millis() { return now; }
void FakeWiFi::record(const char* method, int first, int second) const {
    check(worker, "radio access outside worker");
    calls.push_back({method, worker, now, first, second});
    if (onRadio) { const auto callback = onRadio; callback(); }
}
int FakeWiFi::status() const { record("status"); return state; }
String FakeWiFi::SSID() const { record("SSID"); return String(ssid); }
IPAddress FakeWiFi::localIP() const { record("localIP"); return address; }
void FakeWiFi::persistent(bool value) { record("persistent", value); }
void FakeWiFi::setAutoReconnect(bool value) { record("autoReconnect", value); }
bool FakeWiFi::mode(int value) { record("mode", value); return true; }
bool FakeWiFi::setMinSecurity(int value) { record("security", value); return true; }
int FakeWiFi::begin(const char* network, const char* password) {
    record("begin");
    attemptedSsid = network;
    attemptedPassword = password;
    return state;
}
bool FakeWiFi::disconnect(bool off, bool erase) {
    record("disconnect", off, erase);
    state = 0;
    return true;
}
esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* handle) {
    check(std::strcmp(name, "wifi-cfg") == 0 && !nvs.opened, "wrong namespace or concurrent NVS access");
    ++nvs.opens;
    if (mode == NVS_READWRITE) nvs.writeStarted = true;
    if ((mode == NVS_READONLY && nvs.writeStarted && nvs.fault == "open-read") ||
        (mode == NVS_READWRITE && nvs.fault == "open-write")) return ESP_FAIL;
    nvs.opened = true;
    nvs.mode = mode;
    *handle = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) {
    check(handle == 1 && nvs.opened, "invalid close");
    nvs.opened = false;
}
esp_err_t nvs_get_str(nvs_handle_t handle, const char* key, char* output, size_t* length) {
    check(handle == 1 && nvs.opened, "read closed NVS");
    ++nvs.gets;
    if (nvs.writeStarted && nvs.fault == std::string("get-") + key) return ESP_FAIL;
    const auto found = nvs.values.find(key);
    if (found == nvs.values.end()) return ESP_ERR_NVS_NOT_FOUND;
    std::string value = found->second;
    if (nvs.writeStarted && nvs.fault == std::string("mismatch-") + key) value += "x";
    if (nvs.writeStarted && nvs.fault == "embedded-nul") value.insert(1, 1, '\0');
    const size_t capacity = *length;
    *length = value.size() + 1;
    if (!output) return ESP_OK;
    if (capacity < *length) return ESP_ERR_NVS_INVALID_LENGTH;
    std::memcpy(output, value.c_str(), *length);
    if (nvs.writeStarted && nvs.fault == "bad-length") *length = capacity + 1;
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t handle, const char* key, const char* value) {
    check(handle == 1 && nvs.opened && nvs.mode == NVS_READWRITE, "write invalid NVS");
    check(std::strcmp(key, "ssid") == 0 || std::strcmp(key, "pass") == 0, "unrelated key written");
    ++nvs.sets;
    if (nvs.onSet) { const auto callback = nvs.onSet; callback(); }
    if (nvs.fault == std::string("set-") + key) return ESP_FAIL;
    nvs.values[key] = value; // Eager writes deliberately survive later set/commit failures.
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
    check(handle == 1 && nvs.opened && nvs.mode == NVS_READWRITE, "commit invalid NVS");
    ++nvs.commits;
    return nvs.fault == "commit" ? ESP_FAIL : ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t, const char*) { throw std::runtime_error("unexpected erase"); }
esp_err_t nvs_erase_all(nvs_handle_t) { throw std::runtime_error("unexpected namespace erase"); }

int main() {
    try {
        validation();
        reloads();
        failures();
        interleavings();
        durabilityAndRecovery();
        retryTiming();
        std::cout << "PASS " << cases << " BrainStation cases (production source)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL BrainStation: " << error.what() << '\n';
        return 1;
    }
}
