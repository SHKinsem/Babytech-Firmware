#pragma once

#include <Arduino.h>
#include <WiFiClient.h>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

enum wl_status_t {
    WL_IDLE_STATUS = 0, WL_NO_SSID_AVAIL = 1, WL_SCAN_COMPLETED = 2,
    WL_CONNECTED = 3, WL_CONNECT_FAILED = 4, WL_CONNECTION_LOST = 5,
    WL_DISCONNECTED = 6, WL_NO_SHIELD = 255
};
enum wifi_mode_t { WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3 };
enum wifi_auth_mode_t {
    WIFI_AUTH_OPEN = 0, WIFI_AUTH_WEP = 1, WIFI_AUTH_WPA_PSK = 2,
    WIFI_AUTH_WPA2_PSK = 3, WIFI_AUTH_WPA_WPA2_PSK = 4,
    WIFI_AUTH_WPA2_ENTERPRISE = 5, WIFI_AUTH_WPA3_PSK = 6,
    WIFI_AUTH_WPA2_WPA3_PSK = 7
};
constexpr int16_t WIFI_SCAN_RUNNING = -1;
constexpr int16_t WIFI_SCAN_FAILED = -2;

// This standalone Motion SDK replacement must not be linked alongside the
// cloud-link FakeCloudIo.cpp definition of WiFi. Other SDK types are reused.
class FakeMotionWiFi {
public:
    struct Call {
        std::string method;
        uint32_t at;
        int first;
        int second;
    };
    struct Network {
        std::string ssid;
        int32_t rssi = -60;
        wifi_auth_mode_t security = WIFI_AUTH_WPA2_PSK;
    };
    int status() const { record("status"); return state; }
    String SSID() const { record("SSID"); return String(ssid.c_str()); }
    IPAddress localIP() const { record("localIP"); return address; }
    IPAddress softAPIP() const {
        record("softAPIP");
        return apStarted ? apAddress : IPAddress(0, 0, 0, 0);
    }
    int32_t RSSI() const { record("RSSI"); return rssi; }
    void persistent(bool enabled) { record("persistent", enabled); persistentEnabled = enabled; }
    void setAutoReconnect(bool enabled) { record("autoReconnect", enabled); autoReconnect = enabled; }
    bool setSleep(bool enabled) { record("sleep", enabled); sleepEnabled = enabled; return sleepOk; }
    bool mode(int value) {
        record("mode", value);
        if (!modeOk) return false;
        currentMode = value;
        if (!(value & WIFI_AP)) apStarted = false;
        if (!(value & WIFI_STA)) disconnect(false, false);
        return true;
    }
    wifi_mode_t getMode() const { return static_cast<wifi_mode_t>(currentMode); }
    bool setMinSecurity(int value) { record("security", value); minSecurity = value; return securityOk; }
    bool softAP(const char* network, const char* password = nullptr,
                int channel = 1, int hidden = 0, int maxConnections = 4) {
        record("softAP", channel, maxConnections);
        apSsid = network ? network : "";
        apPassword = password ? password : "";
        apChannel = channel;
        apHidden = hidden;
        apMaxConnections = maxConnections;
        apStarted = apStartOk && (currentMode & WIFI_AP);
        return apStarted;
    }
    int begin(const char* network, const char* password = nullptr) {
        record("begin");
        attemptedSsid = network ? network : "";
        attemptedPassword = password ? password : "";
        state = beginResult;
        ssid.clear();
        address = IPAddress(0, 0, 0, 0);
        if (onBegin) onBegin(*this);
        return state;
    }
    bool disconnect(bool turnOff = false, bool erase = false) {
        record("disconnect", turnOff, erase);
        if (!disconnectOk) return false;
        state = WL_DISCONNECTED;
        ssid.clear();
        address = IPAddress(0, 0, 0, 0);
        if (erase) { attemptedSsid.clear(); attemptedPassword.clear(); }
        if (turnOff) { currentMode = WIFI_OFF; apStarted = false; }
        return true;
    }
    void injectStation(int result, const std::string& network, IPAddress ip, int32_t strength = -60) {
        state = result;
        ssid = network;
        address = ip;
        rssi = strength;
    }
    int16_t scanNetworks(bool async = false, bool hidden = false) {
        record("scanNetworks", async, hidden);
        scanActive = scanStartOk;
        scanResult = scanStartOk ? WIFI_SCAN_RUNNING : WIFI_SCAN_FAILED;
        networks.clear();
        if (onScan) onScan(*this);
        // Tests must inject completion, even for a synchronous SDK request.
        return scanResult;
    }
    int16_t scanComplete() const { record("scanComplete"); return scanResult; }
    void injectScan(std::vector<Network> results) {
        networks = std::move(results);
        scanResult = static_cast<int16_t>(networks.size());
        scanActive = false;
    }
    void injectScanFailure() { scanResult = WIFI_SCAN_FAILED; scanActive = false; }
    void scanDelete() {
        record("scanDelete");
        networks.clear();
        scanResult = WIFI_SCAN_FAILED;
        scanActive = false;
    }
    String SSID(uint8_t index) const {
        record("scanSSID", index);
        return index < networks.size() ? String(networks[index].ssid.c_str()) : String();
    }
    int32_t RSSI(uint8_t index) const {
        record("scanRSSI", index);
        return index < networks.size() ? networks[index].rssi : 0;
    }
    wifi_auth_mode_t encryptionType(uint8_t index) const {
        record("scanSecurity", index);
        return index < networks.size() ? networks[index].security : WIFI_AUTH_OPEN;
    }

    int state = WL_DISCONNECTED;
    int beginResult = WL_IDLE_STATUS;
    std::string ssid;
    std::string attemptedSsid;
    std::string attemptedPassword;
    IPAddress address{0, 0, 0, 0};
    int32_t rssi = -60;
    int currentMode = WIFI_OFF;
    int minSecurity = WIFI_AUTH_WPA2_PSK;
    bool persistentEnabled = true;
    bool autoReconnect = true;
    bool sleepEnabled = true;
    bool modeOk = true;
    bool sleepOk = true;
    bool securityOk = true;
    bool disconnectOk = true;
    bool apStartOk = true;
    bool apStarted = false;
    IPAddress apAddress{192, 168, 4, 1};
    std::string apSsid;
    std::string apPassword;
    int apChannel = 1;
    int apHidden = 0;
    int apMaxConnections = 4;
    bool scanStartOk = true;
    bool scanActive = false;
    int16_t scanResult = WIFI_SCAN_FAILED;
    std::vector<Network> networks;
    mutable std::vector<Call> calls;
    std::function<void(FakeMotionWiFi&)> onBegin;
    std::function<void(FakeMotionWiFi&)> onScan;

private:
    void record(const char* method, int first = 0, int second = 0) const {
        calls.push_back({method, static_cast<uint32_t>(millis()), first, second});
    }
};

inline FakeMotionWiFi WiFi;
