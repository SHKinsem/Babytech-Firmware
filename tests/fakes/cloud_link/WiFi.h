#pragma once
#include "WiFiClient.h"
#include "Arduino.h"
#include <vector>

constexpr int WL_CONNECTED = 3;
constexpr int WIFI_STA = 1;
constexpr int WIFI_AP_STA = 3;
constexpr int WIFI_AUTH_OPEN = 0;
constexpr int WIFI_AUTH_WPA_PSK = 2;
constexpr int WIFI_SCAN_RUNNING = -1;
constexpr int WIFI_SCAN_FAILED = -2;
struct FakeNetwork { std::string ssid; int32_t rssi; int encryption = WIFI_AUTH_WPA_PSK; };
struct FakeWiFiCall {
    std::string method;
    bool worker;
    uint32_t at;
    int first;
    int second;
};
class FakeWiFi {
public:
    int status() const;
    IPAddress softAPIP() const { return IPAddress(0xC0A80401); }
    String SSID() const;
    String SSID(uint8_t index) const;
    int32_t RSSI(uint8_t index) const;
    int encryptionType(uint8_t index) const;
    int scanNetworks(bool async);
    int scanComplete() const;
    void scanDelete();
    int scanStartResult = WIFI_SCAN_RUNNING;
    int scanResult = WIFI_SCAN_RUNNING;
    std::vector<FakeNetwork> networks;
    IPAddress localIP() const;
    void persistent(bool enabled);
    void setAutoReconnect(bool enabled);
    bool mode(int value);
    bool setMinSecurity(int value);
    int begin(const char* ssid, const char* password);
    bool disconnect(bool turnOff = false, bool erase = false);
    bool softAPConfig(IPAddress address, IPAddress gateway, IPAddress subnet);
    bool softAP(const char* name);
    bool softAPdisconnect(bool off);
    bool apOk = true;
    std::string apName;
    int state = WL_CONNECTED;
    std::string ssid;
    std::string attemptedSsid;
    std::string attemptedPassword;
    IPAddress address{0};
    mutable std::vector<FakeWiFiCall> calls;
private:
    void record(const char* method, int first = 0, int second = 0) const;
};
extern FakeWiFi WiFi;
