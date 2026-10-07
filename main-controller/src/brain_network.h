#pragma once

#include "brain_mode.h"
#if BABYTECH_BOARD_LINK_V4
#include "CloudLink.h"
#include "ProductBoardMessages.h"
#include "brain_station.h"
#include <ArduinoJson.h>

namespace babytech { namespace brain {

// The UI loop owns this coordinator and JSON buffers. CloudLink's worker owns
// Wi-Fi/MQTT I/O; communication crosses only its bounded, copying queues.
class BrainNetwork {
public:
    bool begin(const char* pairedDeviceId);
    void poll(const boardlink::Status* lastMotion, bool motionConnected, uint32_t nowMs,
              uint32_t motionReceivedAtMs = 0);
    bool connected() const { return started_ && cloud_.connected(); }
    bool started() const { return started_; }
    bool configureWifi(const char* ssid, const char* password) {
        if (!station_.configure(ssid, password)) return false;
        if (started_) cloud_.requestReconnect();
        return true;
    }
    bool configureMqtt(const char* host, uint16_t port, const char* user, const char* password) {
        return cloud_.configure(host, port, user, password);
    }
private:
    bool publishStatus(const boardlink::Status* lastMotion, bool motionConnected,
                       const cloud::SessionSnapshot& session, uint32_t motionReceivedAtMs,
                       const char* challenge = nullptr);
    void receiveCommand();
    void rejectCommand(const char* commandId, const char* command, uint64_t sequence,
                       const char* session, uint32_t sampledAtMs, uint16_t ttlMs);
    CloudLink cloud_;
    BrainStation station_;
    StaticJsonDocument<4096> status_;
    StaticJsonDocument<768> incomingJson_;
    CloudLink::Inbound inbound_;
    char payload_[2048]{};
    bool started_ = false;
    bool attempted_ = false;
    bool published_ = false;
    uint32_t generation_ = 0;
    uint32_t lastAttemptAtMs_ = 0;
    uint32_t lastPublishedAtMs_ = 0;
};

} }
#endif
