#pragma once

#include "brain_mode.h"
#include "CloudLink.h"
#include "ProductBoardMessages.h"
#include "ProductContext.h"
#include "ProductEventMessages.h"
#include "brain_status.h"
#include "brain_station.h"
#include "brain_portal.h"
#include <ArduinoJson.h>

namespace babytech { namespace brain {

// The UI loop owns this coordinator and JSON buffers. CloudLink's worker owns
// Wi-Fi/MQTT I/O; communication crosses only its bounded, copying queues.
class BrainNetwork {
public:
    using CommandHandler = void(*)(void*, const boardlink::CloudCommand&, uint32_t generation, uint32_t nowMs);
    using StopHandler = void(*)(void*, const boardlink::CloudStop&, uint32_t generation, uint32_t nowMs);
    using ContextHandler = void(*)(void*, const boardlink::ProductContext&, uint32_t generation, uint32_t nowMs);
    using ReceiptHandler = void(*)(void*, const boardlink::CloudReceipt&);
    bool begin(const char* pairedDeviceId);
    // Call once after begin, including if pairing/MQTT startup is unavailable.
    bool beginProvisioning(const char* pairedDeviceId);
    void pollProvisioning(bool feeding, bool bootDown, uint32_t nowMs) {
        portal_.poll(station_, cloud_, feeding, bootDown, nowMs);
    }
    // Handlers receive trusted Current/Expired requests on the UI loop. An
    // expired duplicate must not overwrite an earlier actual acceptance ACK.
    // Only Current requests may start; handlers own dedup/deadline/result ACK.
    // References last only during the callback; copy any retained request.
    void setProductHandlers(CommandHandler command, StopHandler stop, void* context);
    // UI-loop delivery of validated full contexts/tombstones in the current
    // connection generation, independent of command-session TTL/availability.
    // No action, persistence or ACK is implied. Copy before the callback returns;
    // registration/removal is UI-loop-owned and independent of product handlers.
    void setContextHandler(ContextHandler handler, void* context);
    void setReceiptHandler(ReceiptHandler handler, void* context);
    // Re-evaluate on the UI loop at encoding, after inbound config/commands.
    void setCanStartHandler(bool (*handler)()) { canStartHandler_ = handler; }
    // Mode changes rotate immediately, invalidating old-mode queued traffic.
    void resetCommandSession() { if (started_) cloud_.requestReconnect(); }
    bool publishSimulationEvent(const v4::Pairing& pairing, const boardlink::TerminalEvent& event);
    // Real kind-12 terminal only, validated for this paired Brain/device. Keep
    // the original JSON bytes and enqueue in the snapshotted cloud generation;
    // historical results need no Motion status, context or command TTL gate.
    // True means queued, never delivery/stored proof; the caller retains it.
    bool publishTerminalEvent(const v4::Pairing& pairing, const v4::Message& message);
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
              uint32_t motionReceivedAtMs = 0, bool commandsEnabled = false, bool canStart = false,
              const SimulationStatus* simulation = nullptr);
    bool connected() const { return started_ && cloud_.connected(); }
    bool started() const { return started_; }
    void diagnostics(char* output, size_t capacity) const;
    bool configureWifi(const char* ssid, const char* password) {
        if (!station_.configure(ssid, password)) return false;
        if (started_) cloud_.requestReconnect();
        portal_.configurationChanged();
        return true;
    }
    bool configureMqtt(const char* host, uint16_t port, const char* user, const char* password) {
        if (!cloud_.configure(host, port, user, password)) return false;
        portal_.configurationChanged();
        return true;
    }
private:
    static void networkService(void* context);
    static void provisioningTask(void* context);
    bool publishStatus(const boardlink::Status* lastMotion, bool motionConnected,
                       const cloud::SessionSnapshot& session, uint32_t motionReceivedAtMs,
                       const char* challenge = nullptr, bool commandsEnabled = false, bool canStart = false,
                       const SimulationStatus* simulation = nullptr);
    void receiveCommand(uint32_t nowMs);
    bool publishAckForGeneration(const char* commandId, const char* command, uint64_t sequence,
                                 const char* session, bool accepted, const char* reason, uint32_t generation);
    void rejectCommand(const char* commandId, const char* command, uint64_t sequence,
                       const char* session, cloud::Freshness freshness);
    CloudLink cloud_;
    BrainStation station_;
    BrainPortal portal_;
    bool provisioningStarted_ = false;
    StaticJsonDocument<4096> status_;
    StaticJsonDocument<768> incomingJson_;
    CloudLink::Inbound inbound_;
    boardlink::ProductContext contextScratch_;
    char payload_[2048]{};
    CommandHandler commandHandler_ = nullptr;
    StopHandler stopHandler_ = nullptr;
    void* productContext_ = nullptr;
    ContextHandler contextHandler_ = nullptr;
    void* contextOwner_ = nullptr;
    ReceiptHandler receiptHandler_ = nullptr;
    bool (*canStartHandler_)() = nullptr;
    void* receiptOwner_ = nullptr;
    bool started_ = false;
    bool attempted_ = false;
    bool published_ = false;
    uint32_t generation_ = 0;
    uint32_t lastAttemptAtMs_ = 0;
    uint32_t lastPublishedAtMs_ = 0;
};

} }
