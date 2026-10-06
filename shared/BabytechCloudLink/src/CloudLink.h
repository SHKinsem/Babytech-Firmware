#pragma once

#include "CloudIdentity.h"
#include "CloudSession.h"

#include <Arduino.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <WiFiClient.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

// MQTT owns its socket on core 0. Consumers drain queues in their own loop;
// callbacks must never touch motors or LVGL. One lifetime-long instance/MCU.
class CloudLink {
public:
    struct Inbound {
        char topic[motion::kCloudTopicCapacity] = {};
        char payload[2048] = {};
        uint32_t generation = 0;
    };
    struct PublishResult {
        char tag[64] = {};
        bool accepted = false;
    };
    struct StatusPublishOptions {
        bool probeReply = false;
        bool hasMotionSample = false;
        // Original Brain-local receipt time, never probe/enqueue time or Motion uptime.
        uint32_t motionReceivedAtMs = 0;
    };

    void begin(const String& deviceId);
    // Optional cooperative Wi-Fi service runs only on the network worker.
    // It must not call LVGL, UART/motors or reenter CloudLink.
    bool beginV4(const String& deviceId, void (*networkService)(void*) = nullptr,
                 void* networkContext = nullptr);
    bool sessionSnapshot(babytech::cloud::SessionSnapshot& output);
    bool probeReply(const char* targetSession, const char* challenge,
                    uint32_t generation, babytech::cloud::ProbeReply& output);
    babytech::cloud::Freshness checkFreshness(const char* commandSession,
        uint32_t generation, uint32_t sampledAtMs, uint32_t ttlMs);
    void registerRoutes(WebServer& server, bool (*motionBusy)());
    bool take(Inbound& message);
    bool takePublishResult(PublishResult& result);
    bool publish(const char* suffix, const String& payload, const char* tag = nullptr);
    // v4 callers must use the generation captured with the payload's session.
    // A stale queued status/probe reply must not be sent on a new connection.
    bool publishForSession(const char* suffix, const String& payload,
                           uint32_t generation, const char* tag = nullptr);
    // Periodic status replaces the pending periodic value; probes keep FIFO order.
    // With hasMotionSample, age must be <1500 ms at enqueue and publish entry.
    // Leave hasMotionSample false only for absent/stale telemetry encoded conservatively.
    // One producer must submit current projections in observation order. Older
    // samples are rejected; a stale projection requires a strictly later receipt
    // to restore freshness (a same-millisecond recovery waits for the next sample).
    // Superseded status/probe projections are discarded before publish entry.
    // This cannot retract bytes or impose a deadline once socket writing starts.
    bool publishStatusForSession(const String& payload, uint32_t generation,
                                const StatusPublishOptions& options, const char* tag = nullptr);
    void requestReconnect();
    bool connected() const { return connected_.load() && !reconnectRequested_.load(); }
    uint32_t sessionGeneration() const { return sessionGeneration_.load(); }
    bool configured() const;
    // Local provisioning only, after successful begin/beginV4. Nonempty C strings:
    // host/password <=127 bytes, user <=63; no ASCII controls; host uses DNS/IP characters.
    // False preserves RAM settings; NVS may already contain an unverified write.
    bool configure(const char* host, uint16_t port, const char* user, const char* password);
    String host() const;
    uint16_t port() const;
    const char* deviceId() const { return deviceId_; }
    uint32_t droppedInbound() const { return droppedInbound_.load(); }
    uint32_t droppedConfig() const { return droppedConfig_.load(); }

private:
    struct Settings {
        char host[128] = {};
        char user[64] = {};
        char password[128] = {};
        uint16_t port = 1883;
    };
    struct Outbound {
        char topic[motion::kCloudTopicCapacity] = {};
        char payload[2048] = {};
        char tag[64] = {};
        uint32_t generation = 0;
        bool hasMotionSample = false;
        uint32_t motionReceivedAtMs = 0;
        uint64_t statusRevision = 0;
    };

    static void taskEntry(void* context);
    static void mqttCallback(char* topic, uint8_t* payload, unsigned int length);
    void run();
    bool start(const String& deviceId, bool v4SessionMode);
    void releaseUnstartedResources();
    bool openSession();
    bool enqueuePublish(const char* suffix, const String& payload,
                        uint32_t generation, const char* tag,
                        const StatusPublishOptions* status = nullptr);
    static bool sampleCurrent(const Outbound& message);
    bool statusCurrent(const Outbound& message);
    void publishResult(const Outbound& message, bool accepted);
    bool takeLatestStatus(Outbound& message);
    void invalidateSession();
    void failPendingPublishes();
    void receive(char* topic, uint8_t* payload, unsigned int length);
    bool loadSettings();
    bool saveSettings(const Settings& settings);
    bool copySettings(Settings& settings, uint32_t* revision = nullptr) const;
    bool onApInterface(WebServer& server) const;

    char deviceId_[motion::kCloudDeviceIdCapacity] = {};
    char topicPrefix_[motion::kCloudTopicPrefixCapacity] = {};
    char serverHost_[128] = {};
    Settings settings_;
    mutable SemaphoreHandle_t settingsLock_ = nullptr;
    QueueHandle_t inbound_ = nullptr;
    QueueHandle_t stopInbound_ = nullptr;
    QueueHandle_t configInbound_ = nullptr;
    QueueHandle_t outbound_ = nullptr;
    QueueHandle_t results_ = nullptr;
    // Protected by settingsLock_; never hold it during network I/O.
    Outbound latestStatus_;
    bool latestStatusPending_ = false;
    uint32_t statusGeneration_ = 0;
    uint64_t statusRevision_ = 0;
    bool statusHasMotionSample_ = false;
    uint32_t statusMotionReceivedAtMs_ = 0;
    uint32_t statusStaleAtMs_ = 0;
    // Worker-only, non-reentrant buffers: callback nesting must not stack two
    // KB-scale messages on top of run()'s frame in the 6 KiB MQTT task.
    Inbound receiving_;
    Outbound sending_;
    WiFiClient socket_;
    PubSubClient client_{socket_};
    std::atomic<bool> connected_{false};
    std::atomic<bool> reconnectRequested_{false};
    std::atomic<uint32_t> sessionGeneration_{1};
    std::atomic<uint32_t> settingsRevision_{0};
    std::atomic<uint32_t> droppedInbound_{0};
    std::atomic<uint32_t> droppedConfig_{0};
    babytech::cloud::CloudSession session_;
    bool v4SessionMode_ = false;
    std::atomic<bool> started_{false};
    void (*networkService_)(void*) = nullptr;
    void* networkContext_ = nullptr;
    static CloudLink* callbackOwner_;
};
