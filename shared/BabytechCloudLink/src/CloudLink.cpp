#include "CloudLink.h"
#include "CloudCommandPriority.h"
#include "CloudInboundOrder.h"
#include "CloudConfigKind.h"
#include "RetryDeadline.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_random.h>
#include <cstring>

CloudLink* CloudLink::callbackOwner_ = nullptr;

namespace {
constexpr char kNamespace[] = "cloudcfg";
constexpr uint32_t kSettingsMagic = 0x42544331;
constexpr uint32_t kRetryMs = 5000;
constexpr size_t kMaxLegacyInboundPayload = 1535;
constexpr size_t kMaxV4InboundPayload = sizeof(CloudLink::Inbound::payload) - 1;
constexpr size_t kMaxOutboundPayload = 2047;
constexpr uint32_t kMotionSampleLifetimeMs = 1500;

template <size_t N>
bool copyBounded(char (&target)[N], const String& value) {
    if (value.isEmpty() || value.length() >= N) return false;
    value.toCharArray(target, N);
    return true;
}

template <size_t N>
bool copyConfigField(char (&target)[N], const char* value) {
    if (!value) return false;
    for (size_t i = 0; i < N; ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (!c) {
            target[i] = '\0';
            return i != 0;
        }
        if (c < 0x20 || c == 0x7f) return false;
        target[i] = static_cast<char>(c);
    }
    return false;
}

bool validHost(const String& host) {
    if (host.isEmpty() || host.length() >= 128) return false;
    for (unsigned int i = 0; i < host.length(); ++i) {
        const char c = host[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':')) return false;
    }
    return true;
}
}

bool CloudLink::loadSettings() {
    struct Record { uint32_t magic; Settings settings; } record{};
    Preferences preferences;
    if (!preferences.begin(kNamespace, true)) return false;
    const size_t length = preferences.getBytesLength("record");
    const size_t read = length == sizeof(record)
        ? preferences.getBytes("record", &record, sizeof(record)) : 0;
    preferences.end();
    if (read != sizeof(record) || record.magic != kSettingsMagic ||
        !memchr(record.settings.host, 0, sizeof(record.settings.host)) ||
        !memchr(record.settings.user, 0, sizeof(record.settings.user)) ||
        !memchr(record.settings.password, 0, sizeof(record.settings.password)) ||
        !validHost(String(record.settings.host)) || !record.settings.user[0] ||
        !record.settings.password[0] || !record.settings.port) return false;
    xSemaphoreTake(settingsLock_, portMAX_DELAY);
    settings_ = record.settings;
    ++settingsRevision_;
    xSemaphoreGive(settingsLock_);
    return true;
}

bool CloudLink::persistSettings(const Settings& settings) {
    struct Record { uint32_t magic; Settings settings; } record{kSettingsMagic, settings};
    Preferences preferences;
    bool saved = false;
    if (preferences.begin(kNamespace, false)) {
        saved = preferences.putBytes("record", &record, sizeof(record)) == sizeof(record);
        preferences.end();
        if (saved && preferences.begin(kNamespace, true)) {
            Record verified{};
            // Compare fields, not compiler padding; keep the existing record layout.
            saved = preferences.getBytesLength("record") == sizeof(verified) &&
                preferences.getBytes("record", &verified, sizeof(verified)) == sizeof(verified) &&
                verified.magic == record.magic && verified.settings.port == settings.port &&
                memcmp(verified.settings.host, settings.host, sizeof(settings.host)) == 0 &&
                memcmp(verified.settings.user, settings.user, sizeof(settings.user)) == 0 &&
                memcmp(verified.settings.password, settings.password, sizeof(settings.password)) == 0;
            preferences.end();
        } else {
            saved = false;
        }
    }
    return saved;
}

bool CloudLink::saveSettings(const Settings& settings) {
    if (!started_ || !settingsLock_) return false;
    // Running writers and worker snapshots share this lock; first setup has
    // only its local loop owner and does not allocate a worker or its resources.
    xSemaphoreTake(settingsLock_, portMAX_DELAY);
    const bool saved = persistSettings(settings);
    if (saved) {
        settings_ = settings;
        ++settingsRevision_;
    }
    xSemaphoreGive(settingsLock_);
    return saved;
}

bool CloudLink::configure(const char* host, uint16_t port, const char* user, const char* password) {
    if (!port) return false;
    Settings candidate;
    if (!copyConfigField(candidate.host, host) || !validHost(String(candidate.host)) ||
        !copyConfigField(candidate.user, user) || !copyConfigField(candidate.password, password)) return false;
    candidate.port = port;
    if (!started_) {
        if (callbackOwner_) return false;
        return persistSettings(candidate);
    }
    if (!saveSettings(candidate)) return false;
    requestReconnect();
    return true;
}

bool CloudLink::copySettings(Settings& settings, uint32_t* revision) const {
    if (!settingsLock_) return false;
    xSemaphoreTake(settingsLock_, portMAX_DELAY);
    settings = settings_;
    if (revision) *revision = settingsRevision_.load();
    xSemaphoreGive(settingsLock_);
    return settings.host[0] && settings.user[0] && settings.password[0];
}

bool CloudLink::configured() const {
    Settings current;
    return copySettings(current);
}

String CloudLink::host() const {
    Settings current;
    copySettings(current);
    return String(current.host);
}

uint16_t CloudLink::port() const {
    Settings current;
    copySettings(current);
    return current.port;
}

void CloudLink::begin(const String& deviceId) {
    start(deviceId, false);
}

bool CloudLink::beginV4(const String& deviceId, void (*networkService)(void*), void* networkContext) {
    if (started_ || callbackOwner_) return false;
    networkService_ = networkService;
    networkContext_ = networkContext;
    const bool started = start(deviceId, true);
    if (!started) {
        networkService_ = nullptr;
        networkContext_ = nullptr;
    }
    return started;
}

void CloudLink::releaseUnstartedResources() {
    // Only used before a worker exists. A running MQTT task is never torn down
    // from its consumer loop; identity replacement requires a controlled reboot.
    for (QueueHandle_t* queue : {&inbound_, &stopInbound_, &configInbound_, &outbound_, &results_}) {
        if (*queue) vQueueDelete(*queue);
        *queue = nullptr;
    }
    if (settingsLock_) vSemaphoreDelete(settingsLock_);
    settingsLock_ = nullptr;
    settings_ = Settings{};
    latestStatusPending_ = false;
    statusGeneration_ = 0;
    statusRevision_ = 0;
    settingsRevision_ = 0;
    session_.close();
    networkService_ = nullptr;
    networkContext_ = nullptr;
    if (callbackOwner_ == this) callbackOwner_ = nullptr;
}

bool CloudLink::start(const String& deviceId, bool v4SessionMode) {
    if (started_ || callbackOwner_) return false;
    if (!motion::validCloudDeviceId(deviceId.c_str(), deviceId.length())) {
        Serial.println("[cloud] invalid device ID; MQTT disabled");
        return false;
    }
    v4SessionMode_ = v4SessionMode;
    deviceId.toCharArray(deviceId_, sizeof(deviceId_));
    snprintf(topicPrefix_, sizeof(topicPrefix_), "devices/%s/", deviceId_);
    settingsLock_ = xSemaphoreCreateMutex();
    inbound_ = xQueueCreate(8, sizeof(Inbound));
    stopInbound_ = xQueueCreate(2, sizeof(Inbound));
    configInbound_ = xQueueCreate(1, sizeof(Inbound));
    outbound_ = xQueueCreate(8, sizeof(Outbound));
    results_ = xQueueCreate(4, sizeof(PublishResult));
    if (!settingsLock_ || !inbound_ || !stopInbound_ || !configInbound_ ||
        !outbound_ || !results_) {
        Serial.println("[cloud] queue allocation failed; MQTT disabled");
        releaseUnstartedResources();
        return false;
    }
    loadSettings();
    callbackOwner_ = this;
    client_.setCallback(mqttCallback);
    if (!client_.setBufferSize(2560)) {
        Serial.println("[cloud] packet buffer allocation failed; MQTT disabled");
        releaseUnstartedResources();
        return false;
    }
    client_.setSocketTimeout(1);
    client_.setKeepAlive(15);
    if (xTaskCreatePinnedToCore(taskEntry, "babytech-mqtt", 6144, this, 1, nullptr, 0) != pdPASS) {
        Serial.println("[cloud] MQTT task start failed");
        releaseUnstartedResources();
        return false;
    }
    started_ = true;
    return true;
}

bool CloudLink::openSession() {
    if (!v4SessionMode_) return true;
    uint8_t random[16];
    esp_fill_random(random, sizeof(random));
    char token[33];
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(random); ++i) {
        token[2 * i] = hex[random[i] >> 4];
        token[2 * i + 1] = hex[random[i] & 15];
    }
    token[32] = 0;
    xSemaphoreTake(settingsLock_, portMAX_DELAY);
    const bool opened = session_.open(token, sessionGeneration_.load(), millis());
    xSemaphoreGive(settingsLock_);
    return opened;
}

bool CloudLink::sessionSnapshot(babytech::cloud::SessionSnapshot& output) {
    if (!v4SessionMode_ || !settingsLock_ || !connected()) return false;
    babytech::cloud::SessionSnapshot value;
    xSemaphoreTake(settingsLock_, portMAX_DELAY);
    const bool valid = session_.snapshot(millis(), value);
    xSemaphoreGive(settingsLock_);
    if (!valid || !connected() || value.generation != sessionGeneration_.load()) return false;
    output = value;
    return true;
}

bool CloudLink::probeReply(const char* targetSession, const char* challenge,
                           uint32_t generation, babytech::cloud::ProbeReply& output) {
    if (!v4SessionMode_ || !settingsLock_ || !connected()) return false;
    babytech::cloud::ProbeReply value;
    xSemaphoreTake(settingsLock_, portMAX_DELAY);
    const bool valid = session_.probe(targetSession, challenge, generation, millis(), value);
    xSemaphoreGive(settingsLock_);
    if (!valid || !connected() || generation != sessionGeneration_.load()) return false;
    output = value;
    return true;
}

babytech::cloud::Freshness CloudLink::checkFreshness(const char* commandSession,
        uint32_t generation, uint32_t sampledAtMs, uint32_t ttlMs) {
    using babytech::cloud::Freshness;
    if (!v4SessionMode_ || !settingsLock_ || !connected()) return Freshness::Disconnected;
    xSemaphoreTake(settingsLock_, portMAX_DELAY);
    const Freshness result = session_.check(commandSession, generation, sampledAtMs, ttlMs, millis());
    xSemaphoreGive(settingsLock_);
    if (!connected()) return Freshness::Disconnected;
    if (generation != sessionGeneration_.load()) return Freshness::WrongSession;
    return result;
}

bool CloudLink::onApInterface(WebServer& server) const {
    return server.client().localIP() == WiFi.softAPIP();
}

void CloudLink::registerRoutes(WebServer& server, bool (*motionBusy)()) {
    server.on("/api/cloud", HTTP_GET, [this, &server]() {
        if (!onApInterface(server)) { server.send(403); return; }
        Settings current;
        copySettings(current);
        String body = F("{\"device_id\":\"");
        body += deviceId_;
        body += F("\",\"configured\":");
        body += configured() ? F("true") : F("false");
        body += F(",\"connected\":");
        body += connected() ? F("true") : F("false");
        body += F(",\"host\":\"");
        body += current.host;
        body += F("\",\"port\":");
        body += current.port;
        body += F(",\"dropped_inbound\":");
        body += droppedInbound();
        body += F(",\"dropped_config\":");
        body += droppedConfig();
        body += F("}");
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "application/json", body);
    });
    server.on("/api/cloud", HTTP_POST, [this, &server, motionBusy]() {
        if (!onApInterface(server)) { server.send(403); return; }
        if (!settingsLock_) { server.send(503, "text/plain", "cloud_unavailable"); return; }
        if (motionBusy && motionBusy()) { server.send(409, "text/plain", "motion_busy"); return; }
        const String host = server.arg("host");
        const String user = server.arg("username");
        const String password = server.arg("password");
        const long port = server.arg("port").toInt();
        Settings candidate;
        if (!validHost(host) || !copyBounded(candidate.user, user) ||
            !copyBounded(candidate.password, password) || port < 1 || port > 65535) {
            server.send(400, "text/plain", "invalid_cloud_config"); return;
        }
        host.toCharArray(candidate.host, sizeof(candidate.host));
        candidate.port = static_cast<uint16_t>(port);
        if (!saveSettings(candidate)) { server.send(500, "text/plain", "config_save_failed"); return; }
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "application/json", "{\"configured\":true}");
    });
}

bool CloudLink::take(Inbound& message) {
    if (!inbound_ || !stopInbound_ || !configInbound_) return false;
    for (;;) {
        const auto lane = motion::selectCloudInbound(
            uxQueueMessagesWaiting(stopInbound_) > 0,
            uxQueueMessagesWaiting(configInbound_) > 0,
            uxQueueMessagesWaiting(inbound_) > 0);
        if (lane == motion::CloudInboundLane::None) return false;
        QueueHandle_t selected = inbound_;
        if (lane == motion::CloudInboundLane::Stop) selected = stopInbound_;
        if (lane == motion::CloudInboundLane::Config) selected = configInbound_;
        const BaseType_t received = xQueueReceive(selected, &message, 0);
        if (received != pdTRUE) continue;
        if (message.generation == sessionGeneration_.load()) return true;
        ++droppedInbound_;
    }
}

bool CloudLink::takePublishResult(PublishResult& result) {
    return results_ && xQueueReceive(results_, &result, 0) == pdTRUE;
}

bool CloudLink::publish(const char* suffix, const String& payload, const char* tag) {
    if (v4SessionMode_) return false;
    return enqueuePublish(suffix, payload, 0, tag);
}

bool CloudLink::publishForSession(const char* suffix, const String& payload,
                                   uint32_t generation, const char* tag) {
    babytech::cloud::SessionSnapshot session;
    if (!sessionSnapshot(session) || generation != session.generation) return false;
    return enqueuePublish(suffix, payload, generation, tag);
}

bool CloudLink::enqueuePublish(const char* suffix, const String& payload,
                                uint32_t generation, const char* tag,
                                const StatusPublishOptions* status) {
    if (!outbound_ || !suffix || payload.length() > kMaxOutboundPayload) return false;
    Outbound message;
    if (snprintf(message.topic, sizeof(message.topic), "%s%s", topicPrefix_, suffix) >=
        static_cast<int>(sizeof(message.topic))) return false;
    payload.toCharArray(message.payload, sizeof(message.payload));
    message.generation = generation;
    if (tag) strlcpy(message.tag, tag, sizeof(message.tag));
    if (status) {
        message.hasMotionSample = status->hasMotionSample;
        message.motionReceivedAtMs = status->motionReceivedAtMs;
        if (!sampleCurrent(message)) return false;
        xSemaphoreTake(settingsLock_, portMAX_DELAY);
        // Validate and order both lanes atomically; no network I/O under this lock.
        babytech::cloud::SessionSnapshot current;
        if (!connected() || !session_.snapshot(millis(), current) ||
            current.generation != generation || generation != sessionGeneration_.load() ||
            !sampleCurrent(message)) {
            xSemaphoreGive(settingsLock_);
            return false;
        }
        const bool first = statusGeneration_ != generation || !statusRevision_;
        bool changed = first || message.hasMotionSample != statusHasMotionSample_;
        if (!first && message.hasMotionSample) {
            const uint32_t step = message.motionReceivedAtMs -
                (statusHasMotionSample_ ? statusMotionReceivedAtMs_ : statusStaleAtMs_);
            // Equal receipts may answer multiple challenges, but cannot undo stale.
            if (step >= UINT32_C(0x80000000) || (!statusHasMotionSample_ && !step)) {
                xSemaphoreGive(settingsLock_);
                return false;
            }
            changed = changed || step != 0;
        }
        if (!first && changed && statusRevision_ == UINT64_MAX) {
            xSemaphoreGive(settingsLock_);
            return false;
        }
        message.statusRevision = first ? 1 : statusRevision_ + (changed ? 1 : 0);
        bool accepted;
        if (status->probeReply) {
            accepted = xQueueSend(outbound_, &message, 0) == pdTRUE;
        } else {
            if (latestStatusPending_) publishResult(latestStatus_, false);
            latestStatus_ = message;
            latestStatusPending_ = true;
            accepted = true;
        }
        // A full FIFO must not invalidate the last projection that can still be sent.
        if (accepted) {
            statusGeneration_ = generation;
            statusRevision_ = message.statusRevision;
            statusHasMotionSample_ = message.hasMotionSample;
            statusMotionReceivedAtMs_ = message.motionReceivedAtMs;
            if (!message.hasMotionSample) statusStaleAtMs_ = millis();
        }
        xSemaphoreGive(settingsLock_);
        return accepted;
    }
    return xQueueSend(outbound_, &message, 0) == pdTRUE;
}

bool CloudLink::publishStatusForSession(const String& payload, uint32_t generation,
                                       const StatusPublishOptions& options, const char* tag) {
    babytech::cloud::SessionSnapshot current;
    if (!sessionSnapshot(current) || generation != current.generation) return false;
    return enqueuePublish("status", payload, generation, tag, &options);
}

bool CloudLink::sampleCurrent(const Outbound& message) {
    return !message.hasMotionSample ||
        uint32_t(millis() - message.motionReceivedAtMs) < kMotionSampleLifetimeMs;
}

bool CloudLink::statusCurrent(const Outbound& message) {
    if (!message.statusRevision) return true;  // Existing generic publish API.
    xSemaphoreTake(settingsLock_, portMAX_DELAY);
    const bool current = message.generation == statusGeneration_ &&
        message.statusRevision == statusRevision_;
    xSemaphoreGive(settingsLock_);
    return current;
}

void CloudLink::publishResult(const Outbound& message, bool accepted) {
    if (!message.tag[0]) return;
    PublishResult result;
    strlcpy(result.tag, message.tag, sizeof(result.tag));
    result.accepted = accepted;
    xQueueSend(results_, &result, 0);
}

bool CloudLink::takeLatestStatus(Outbound& message) {
    xSemaphoreTake(settingsLock_, portMAX_DELAY);
    const bool pending = latestStatusPending_;
    if (pending) {
        message = latestStatus_;
        latestStatusPending_ = false;
    }
    xSemaphoreGive(settingsLock_);
    return pending;
}

void CloudLink::requestReconnect() {
    ++sessionGeneration_;
    connected_ = false;
    reconnectRequested_ = true;
}

void CloudLink::mqttCallback(char* topic, uint8_t* payload, unsigned int length) {
    if (callbackOwner_) callbackOwner_->receive(topic, payload, length);
}

void CloudLink::receive(char* topic, uint8_t* payload, unsigned int length) {
    if (!inbound_ || !stopInbound_ || !configInbound_ || !topic || !payload ||
        length > (v4SessionMode_ ? kMaxV4InboundPayload : kMaxLegacyInboundPayload) ||
        strlen(topic) >= sizeof(Inbound::topic) || memchr(payload, 0, length)) return;
    if (strncmp(topic, topicPrefix_, strlen(topicPrefix_)) != 0) return;
    const char* suffix = topic + strlen(topicPrefix_);
    if (strcmp(suffix, "command") != 0 && strcmp(suffix, "config") != 0) return;
    Inbound& message = receiving_;
    strlcpy(message.topic, topic, sizeof(message.topic));
    memcpy(message.payload, payload, length);
    message.payload[length] = '\0';
    message.generation = sessionGeneration_.load();
    if (strcmp(suffix, "config") == 0) {
        const auto kind = motion::cloudConfigKind(payload, length, deviceId_, v4SessionMode_);
        if (kind == motion::CloudConfigKind::Invalid) return;
        if (kind == motion::CloudConfigKind::Receipt || kind == motion::CloudConfigKind::SessionProbe) {
            // Receipts and read-only probes can be retried by their senders.
            // Neither may overwrite the single retained-context lane.
            if (xQueueSend(inbound_, &message, 0) != pdTRUE) ++droppedInbound_;
            return;
        }
        if (xQueueSend(configInbound_, &message, 0) != pdTRUE) {
            // One retained topic per device: only its latest value is needed.
            xQueueOverwrite(configInbound_, &message);
            ++droppedInbound_;
            ++droppedConfig_;
        }
        return;
    }
    if (motion::isPriorityStopCommand(payload, length, deviceId_)) {
        if (xQueueSend(stopInbound_, &message, 0) != pdTRUE) {
            Inbound evicted;
            if (xQueueReceive(stopInbound_, &evicted, 0) == pdTRUE) ++droppedInbound_;
            if (xQueueSend(stopInbound_, &message, 0) != pdTRUE) ++droppedInbound_;
        }
        return;
    }
    if (xQueueSend(inbound_, &message, 0) != pdTRUE) ++droppedInbound_;
}

void CloudLink::taskEntry(void* context) {
    static_cast<CloudLink*>(context)->run();
}

void CloudLink::failPendingPublishes() {
    Outbound& message = sending_;
    while (xQueueReceive(outbound_, &message, 0) == pdTRUE) {
        publishResult(message, false);
    }
    if (takeLatestStatus(message)) publishResult(message, false);
}

void CloudLink::invalidateSession() {
    if (connected_.exchange(false)) ++sessionGeneration_;
    if (v4SessionMode_) {
        xSemaphoreTake(settingsLock_, portMAX_DELAY);
        session_.close();
        xSemaphoreGive(settingsLock_);
    }
}

void CloudLink::run() {
    Settings activeSettings;
    uint32_t appliedRevision = 0;
    uint32_t retryAt = millis();
    for (;;) {
        if (networkService_) networkService_(networkContext_);
        if (reconnectRequested_.exchange(false)) {
            if (client_.connected()) client_.disconnect();
            invalidateSession();
            failPendingPublishes();
            retryAt = millis();
        }
        if (WiFi.status() != WL_CONNECTED || !configured()) {
            if (client_.connected()) client_.disconnect();
            invalidateSession();
            failPendingPublishes();
            retryAt = millis();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (settingsRevision_.load() != appliedRevision) {
            if (client_.connected()) client_.disconnect();
            invalidateSession();
            failPendingPublishes();
            uint32_t snapshotRevision = 0;
            if (!copySettings(activeSettings, &snapshotRevision)) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            strlcpy(serverHost_, activeSettings.host, sizeof(serverHost_));
            client_.setServer(serverHost_, activeSettings.port);
            appliedRevision = snapshotRevision;
            retryAt = millis();
        }
        if (!client_.connected()) {
            invalidateSession();
            failPendingPublishes();
            if (!motion::retryDue(millis(), retryAt)) {
                vTaskDelay(pdMS_TO_TICKS(25));
                continue;
            }
            retryAt = millis() + kRetryMs;
            if (client_.connect(deviceId_, activeSettings.user, activeSettings.password)) {
                char topic[motion::kCloudTopicCapacity];
                snprintf(topic, sizeof(topic), "%scommand", topicPrefix_);
                const bool commands = client_.subscribe(topic);
                snprintf(topic, sizeof(topic), "%sconfig", topicPrefix_);
                const bool subscriptionsReady = commands && client_.subscribe(topic);
                connected_ = subscriptionsReady && openSession() && !reconnectRequested_.load();
                if (!connected()) client_.disconnect();
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (v4SessionMode_ && connected()) {
            babytech::cloud::SessionSnapshot snapshot;
            if (!sessionSnapshot(snapshot)) {
                client_.disconnect();
                invalidateSession();
                failPendingPublishes();
                retryAt = millis();
                continue;
            }
        }
        if (!client_.loop()) {
            invalidateSession();
            continue;
        }
        if (reconnectRequested_.load()) continue;
        Outbound& message = sending_;
        for (uint8_t i = 0; i < 2; ++i) {
            // Reserve the first opportunity for the latest periodic status;
            // always give the FIFO an opportunity even under status pressure.
            if (!(i == 0 && takeLatestStatus(message)) &&
                xQueueReceive(outbound_, &message, 0) != pdTRUE) break;
            babytech::cloud::SessionSnapshot currentSession;
            if (v4SessionMode_ &&
                (!sessionSnapshot(currentSession) || message.generation != currentSession.generation ||
                 !statusCurrent(message) || !sampleCurrent(message))) {
                publishResult(message, false);
                continue;
            }
            // This is a publish-entry guard, not cancellation of an in-flight TCP write.
            const bool accepted = client_.publish(message.topic, message.payload, false);
            publishResult(message, accepted);
            if (!accepted) {
                client_.disconnect();
                invalidateSession();
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
