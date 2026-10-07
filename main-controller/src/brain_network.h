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
    using CommandHandler = void(*)(void*, const boardlink::CloudCommand&, uint32_t generation, uint32_t nowMs);
    using StopHandler = void(*)(void*, const boardlink::CloudStop&, uint32_t generation, uint32_t nowMs);
    bool begin(const char* pairedDeviceId);
    // Handlers receive trusted Current/Expired requests on the UI loop. An
    // expired duplicate must not overwrite an earlier actual acceptance ACK.
    // Only Current requests may start; handlers own dedup/deadline/result ACK.
    // References last only during the callback; copy any retained request.
    void setProductHandlers(CommandHandler command, StopHandler stop, void* context);
    cloud::Freshness checkFreshness(const char* session, uint32_t generation,
                                   uint32_t sampledAtMs, uint16_t ttlMs);
    // Trusted runtime only: publish a determined original result, not a new
    // action. Original session is preserved even after reconnect; no TTL gate.
    // False (including offline/full queue) leaves retention to the caller.
    // True means queued, not broker delivery. Fields are validated without
    // truncation; reason uses the existing 1..64-byte [A-Za-z0-9_] result code.
    bool publishAck(const char* commandId, const char* command, uint64_t sequence,
                    const char* originalSession, bool accepted, const char* reason);
    void poll(const boardlink::Status* lastMotion, bool motionConnected, uint32_t nowMs,
              uint32_t motionReceivedAtMs = 0, bool commandsEnabled = false, bool canStart = false);
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
                       const char* challenge = nullptr, bool commandsEnabled = false, bool canStart = false);
    void receiveCommand(uint32_t nowMs);
    bool publishAckForGeneration(const char* commandId, const char* command, uint64_t sequence,
                                 const char* session, bool accepted, const char* reason, uint32_t generation);
    void rejectCommand(const char* commandId, const char* command, uint64_t sequence,
                       const char* session, cloud::Freshness freshness);
    CloudLink cloud_;
    BrainStation station_;
    StaticJsonDocument<4096> status_;
    StaticJsonDocument<768> incomingJson_;
    CloudLink::Inbound inbound_;
    char payload_[2048]{};
    CommandHandler commandHandler_ = nullptr;
    StopHandler stopHandler_ = nullptr;
    void* productContext_ = nullptr;
    bool started_ = false;
    bool attempted_ = false;
    bool published_ = false;
    uint32_t generation_ = 0;
    uint32_t lastAttemptAtMs_ = 0;
    uint32_t lastPublishedAtMs_ = 0;
};

} }
#endif
