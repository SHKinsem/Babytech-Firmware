#pragma once
#include "WiFiClient.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class PubSubClient {
public:
    using Callback = void (*)(char*, uint8_t*, unsigned);
    explicit PubSubClient(WiFiClient& socket);
    ~PubSubClient();
    void setCallback(Callback callback);
    bool setBufferSize(uint16_t size);
    void setSocketTimeout(uint16_t timeout);
    void setKeepAlive(uint16_t seconds);
    void setServer(const char* host, uint16_t port);
    bool connect(const char* id, const char* user, const char* password);
    bool connected() const { return connected_; }
    int state() const { return connected_ ? 0 : -1; }
    void disconnect();
    bool subscribe(const char* topic);
    bool loop();
    bool publish(const char* topic, const char* payload, bool retained = false);
    void deliver(const std::string& topic, const std::string& payload);
    void dropConnection() { connected_ = false; }
    size_t bufferSize() const { return buffer_.size(); }
    unsigned callbackChanges() const { return callbackChanges_; }
private:
    Callback callback_ = nullptr;
    bool connected_ = false;
    unsigned callbackChanges_ = 0;
    std::vector<uint8_t> buffer_;
};
