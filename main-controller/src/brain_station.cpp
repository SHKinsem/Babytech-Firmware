#include "brain_mode.h"

#include "brain_station.h"
#include <WiFi.h>
#include <nvs.h>
#include <cstring>

namespace babytech { namespace brain {
namespace {
constexpr uint32_t kJoinTimeoutMs = 15000;
constexpr uint32_t kRetryMs = 30000;

// Nonblocking for both owners, including while another owner is in NVS I/O.
class ConfigGuard {
public:
    explicit ConfigGuard(std::atomic_flag& busy)
        : busy_(busy), acquired_(!busy.test_and_set(std::memory_order_acquire)) {}
    ~ConfigGuard() { if (acquired_) busy_.clear(std::memory_order_release); }
    explicit operator bool() const { return acquired_; }
private:
    std::atomic_flag& busy_;
    bool acquired_;
};

bool validCredentials(const char* ssid, const char* password) {
    if (!ssid || !password) return false;
    const size_t ssidBytes = strnlen(ssid, 33);
    const size_t passwordBytes = strnlen(password, 65);
    if (!ssidBytes || ssidBytes > 32 || passwordBytes > 64 ||
        (passwordBytes && passwordBytes < 8)) return false;
    if (passwordBytes == 64) {
        for (size_t i = 0; i < passwordBytes; ++i) {
            const char c = password[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'))) return false;
        }
    }
    return true;
}

bool readCredentials(char (&ssid)[33], char (&password)[65]) {
    nvs_handle_t handle;
    if (nvs_open("wifi-cfg", NVS_READONLY, &handle) != ESP_OK) return false;
    size_t ssidLength = sizeof(ssid);
    size_t passwordLength = sizeof(password);
    const esp_err_t ssidResult = nvs_get_str(handle, "ssid", ssid, &ssidLength);
    const esp_err_t passwordResult = nvs_get_str(handle, "pass", password, &passwordLength);
    nvs_close(handle);
    if (ssidResult != ESP_OK || passwordResult != ESP_OK || ssidLength < 2 ||
        ssidLength > sizeof(ssid) || passwordLength < 1 || passwordLength > sizeof(password) ||
        ssid[ssidLength - 1] || password[passwordLength - 1] ||
        std::strlen(ssid) + 1 != ssidLength || std::strlen(password) + 1 != passwordLength)
        return false;
    return validCredentials(ssid, password);
}
}

void BrainStation::loadSnapshot() {
    if (snapshotLoaded_) return;
    snapshotValid_ = readCredentials(savedSsid_, savedPassword_);
    snapshotLoaded_ = true;
}

bool BrainStation::configure(const char* ssid, const char* password) {
    if (!validCredentials(ssid, password)) return false;
    char requestedSsid[33]{};
    char requestedPassword[65]{};
    std::memcpy(requestedSsid, ssid, std::strlen(ssid) + 1);
    std::memcpy(requestedPassword, password, std::strlen(password) + 1);
    ConfigGuard guard(configBusy_);
    if (!guard) return false;
    // Preserve the pre-write baseline even if configure precedes the first poll.
    loadSnapshot();
    nvs_handle_t handle;
    if (nvs_open("wifi-cfg", NVS_READWRITE, &handle) != ESP_OK) return false;
    const bool written = nvs_set_str(handle, "ssid", requestedSsid) == ESP_OK &&
                         nvs_set_str(handle, "pass", requestedPassword) == ESP_OK &&
                         nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    if (!written) return false;
    char verifiedSsid[33]{};
    char verifiedPassword[65]{};
    if (!readCredentials(verifiedSsid, verifiedPassword) ||
        std::strcmp(requestedSsid, verifiedSsid) ||
        std::strcmp(requestedPassword, verifiedPassword)) return false;
    // Never reload directly from possibly half-written keys after a failed save.
    std::memcpy(savedSsid_, verifiedSsid, sizeof(savedSsid_));
    std::memcpy(savedPassword_, verifiedPassword, sizeof(savedPassword_));
    snapshotValid_ = true;
    reloadRequested_.store(true, std::memory_order_release);
    return true;
}

void BrainStation::service(void* context) {
    static_cast<BrainStation*>(context)->poll();
}

bool BrainStation::copySsid(char (&output)[33]) {
    ConfigGuard guard(configBusy_);
    if (!guard) return false;
    loadSnapshot();
    std::memset(output, 0, sizeof(output));
    if (snapshotValid_) std::memcpy(output, savedSsid_, sizeof(output));
    return true;
}

void BrainStation::provisioningAp(bool enabled) {
    apActive_ = enabled;
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    WiFi.mode(enabled ? WIFI_AP_STA : WIFI_STA);
}

void BrainStation::poll() {
    bool reload = false;
    bool initializeRadio = false;
    if (!loaded_ || reloadRequested_.load(std::memory_order_acquire)) {
        ConfigGuard guard(configBusy_);
        if (guard) {
            loadSnapshot();
            // An old STA association cannot confirm a newly saved configuration.
            connected_.store(false);
            reload = reloadRequested_.exchange(false, std::memory_order_acq_rel);
            initializeRadio = !configured_ && snapshotValid_;
            configured_ = snapshotValid_;
            hasCredentials_.store(configured_);
            if (configured_) {
                std::memcpy(ssid_, savedSsid_, sizeof(ssid_));
                std::memcpy(password_, savedPassword_, sizeof(password_));
            }
            loaded_ = true;
            credentialsKnown_.store(true);
        }
    }
    // No configuration lock is held over radio/SDK calls.
    if (initializeRadio) {
        WiFi.persistent(false);
        WiFi.setAutoReconnect(false);
        WiFi.mode(apActive_ ? WIFI_AP_STA : WIFI_STA);
    }
    if (reload) {
        WiFi.disconnect(false, false);
        joining_ = false;
        attempted_ = false;
    }
    connected_.store(false);
    if (!configured_) return;
    const uint32_t now = millis();
    if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid_ &&
        WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        joining_ = false;
        attemptAtMs_ = now;
        connected_.store(true);
        return;
    }
    if (joining_) {
        if (uint32_t(now - attemptAtMs_) < kJoinTimeoutMs) return;
        WiFi.disconnect(false, false);
        joining_ = false;
    }
    if (attempted_ && uint32_t(now - attemptAtMs_) < kRetryMs) return;
    attempted_ = true;
    joining_ = true;
    attemptAtMs_ = now;
    WiFi.setMinSecurity(password_[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN);
    WiFi.begin(ssid_, password_);
}

} }
