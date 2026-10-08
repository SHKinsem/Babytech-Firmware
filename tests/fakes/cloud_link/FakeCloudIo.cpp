#include "FakeCloudIo.h"
#include "Arduino.h"
#include "Preferences.h"
#include "WiFi.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

struct FakeQueue {
    size_t capacity;
    size_t itemSize;
    std::deque<std::vector<uint8_t>> items;
};
struct FakeSemaphore {
    std::mutex mutex;
    std::condition_variable released;
    std::thread::id owner;
};

namespace {
std::map<QueueHandle_t, std::unique_ptr<FakeQueue>> queues;
std::map<SemaphoreHandle_t, std::unique_ptr<FakeSemaphore>> semaphores;
std::vector<std::pair<QueueHandle_t, std::vector<uint8_t>>> deferredSends;
bool allocationFails() {
    ++fake::io.allocationCalls;
    return fake::io.failAllocation == fake::io.allocationCalls;
}
FakeQueue& queueAt(QueueHandle_t queue) {
    fake::check(queues.count(queue) == 1, "invalid/deleted queue handle");
    return *queue;
}
FakeSemaphore& semaphoreAt(SemaphoreHandle_t semaphore) {
    fake::check(semaphores.count(semaphore) == 1, "invalid/deleted semaphore handle");
    return *semaphore;
}
}

namespace fake {
Io io;
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
size_t liveQueues() { return queues.size(); }
size_t liveSemaphores() { return semaphores.size(); }
void completeDeferredSends() {
    for (const auto& send : deferredSends)
        check(xQueueSend(send.first, send.second.data(), 0) == pdTRUE, "deferred queue send failed");
    deferredSends.clear();
}
void runWorker() {
    check(io.tasks.size() == 1, "expected exactly one captured worker");
    io.inWorker = true;
    try {
        io.tasks.front().entry(io.tasks.front().context);
    } catch (const StopWorker&) {
        io.inWorker = false;
        return;
    } catch (...) {
        io.inWorker = false;
        throw;
    }
    io.inWorker = false;
    throw std::runtime_error("production worker returned unexpectedly");
}
void cleanupLifetimeResources() {
    for (const auto& entry : semaphores)
        check(entry.second->owner == std::thread::id{}, "worker stopped while holding a semaphore");
    check(io.openPreferences == 0, "Preferences handle leak");
    check(deferredSends.empty(), "uncompleted deferred queue send");
    queues.clear();
    semaphores.clear();
    io.tasks.clear();
    io.onDelay = nullptr;
    io.onLoop = nullptr;
    io.onPreferencesWrite = nullptr;
    io.onSemaphoreWait = nullptr;
    io.onConnect = nullptr;
}
}  // namespace fake

FakeSerial Serial;
FakeWiFi WiFi;
uint32_t millis() { return fake::io.now; }
void FakeSerial::println(const char* message) { fake::io.serial.emplace_back(message); }

void FakeWiFi::record(const char* method, int first, int second) const {
    calls.push_back({method, fake::io.inWorker, millis(), first, second});
}
int FakeWiFi::status() const { record("status"); return state; }
String FakeWiFi::SSID() const { record("SSID"); return String(ssid); }
IPAddress FakeWiFi::localIP() const { record("localIP"); return address; }
void FakeWiFi::persistent(bool enabled) { record("persistent", enabled); }
void FakeWiFi::setAutoReconnect(bool enabled) { record("autoReconnect", enabled); }
bool FakeWiFi::mode(int value) { record("mode", value); return true; }
bool FakeWiFi::setMinSecurity(int value) { record("security", value); return true; }
int FakeWiFi::begin(const char* network, const char* password) {
    record("begin");
    attemptedSsid = network;
    attemptedPassword = password;
    return state;
}
bool FakeWiFi::disconnect(bool turnOff, bool erase) {
    record("disconnect", turnOff, erase);
    state = 0;
    return true;
}

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t itemSize) {
    if (allocationFails()) return nullptr;
    auto value = std::make_unique<FakeQueue>();
    value->capacity = length;
    value->itemSize = itemSize;
    QueueHandle_t handle = value.get();
    queues.emplace(handle, std::move(value));
    return handle;
}
void vQueueDelete(QueueHandle_t queue) {
    queueAt(queue);
    queues.erase(queue);
}
BaseType_t xQueueSend(QueueHandle_t queue, const void* item, TickType_t wait) {
    fake::check(wait == 0, "fake supports only nonblocking queue operations");
    auto& value = queueAt(queue);
    ++fake::io.queueSendCalls;
    if (value.items.size() == value.capacity) {
        ++fake::io.queueSendFailures;
        return pdFALSE;
    }
    const auto* data = static_cast<const uint8_t*>(item);
    if (fake::io.deferNextQueueSend) {
        fake::io.deferNextQueueSend = false;
        deferredSends.emplace_back(queue, std::vector<uint8_t>(data, data + value.itemSize));
        return pdTRUE;
    }
    value.items.emplace_back(data, data + value.itemSize);
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void* item, TickType_t wait) {
    fake::check(wait == 0, "fake supports only nonblocking queue operations");
    auto& value = queueAt(queue);
    if (value.items.empty()) return pdFALSE;
    std::memcpy(item, value.items.front().data(), value.itemSize);
    value.items.pop_front();
    return pdTRUE;
}
BaseType_t xQueueOverwrite(QueueHandle_t queue, const void* item) {
    auto& value = queueAt(queue);
    fake::check(value.capacity == 1, "overwrite requires single-slot queue");
    value.items.clear();
    return xQueueSend(queue, item, 0);
}
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue) {
    return static_cast<UBaseType_t>(queueAt(queue).items.size());
}
SemaphoreHandle_t xSemaphoreCreateMutex() {
    if (allocationFails()) return nullptr;
    auto value = std::make_unique<FakeSemaphore>();
    auto* handle = value.get();
    semaphores.emplace(handle, std::move(value));
    return handle;
}
void vSemaphoreDelete(SemaphoreHandle_t semaphore) {
    fake::check(semaphoreAt(semaphore).owner == std::thread::id{}, "deleting locked semaphore");
    semaphores.erase(semaphore);
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t) {
    auto& value = semaphoreAt(semaphore);
    std::unique_lock<std::mutex> guard(value.mutex);
    fake::check(value.owner != std::this_thread::get_id(), "recursive/deadlocked semaphore take");
    if (value.owner != std::thread::id{} && fake::io.onSemaphoreWait) fake::io.onSemaphoreWait();
    value.released.wait(guard, [&] { return value.owner == std::thread::id{}; });
    value.owner = std::this_thread::get_id();
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
    auto& value = semaphoreAt(semaphore);
    std::lock_guard<std::mutex> guard(value.mutex);
    fake::check(value.owner == std::this_thread::get_id(), "giving unowned semaphore");
    value.owner = std::thread::id{};
    value.released.notify_one();
    return pdTRUE;
}
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t entry, const char*, uint32_t stack,
    void* context, UBaseType_t, TaskHandle_t* handle, BaseType_t core) {
    ++fake::io.taskAttempts;
    if (fake::io.failTask) return pdFALSE;
    fake::io.tasks.push_back({entry, context, stack, core});
    if (handle) *handle = context;
    return pdPASS;
}
void vTaskDelay(TickType_t ticks) {
    fake::io.now += ticks;
    ++fake::io.delays;
    fake::check(fake::io.delays <= 1000, "worker exceeded bounded test schedule");
    if (fake::io.onDelay) fake::io.onDelay(fake::io.delays);
}

Preferences::~Preferences() { end(); }
bool Preferences::begin(const char* name, bool readOnly) {
    fake::check(!opened_, "Preferences double open");
    fake::io.preferenceOpens.emplace_back(name, readOnly);
    if (fake::io.failPreferencesOpen || (readOnly && fake::io.failPreferencesReadOpen)) return false;
    opened_ = true;
    readOnly_ = readOnly;
    name_ = name;
    ++fake::io.openPreferences;
    return true;
}
size_t Preferences::getBytesLength(const char* key) {
    fake::check(opened_, "read closed Preferences");
    if (fake::io.failPreferencesLength) return 0;
    const auto found = fake::io.preferences.find(name_ + "/" + key);
    return found == fake::io.preferences.end() ? 0 : found->second.size();
}
size_t Preferences::getBytes(const char* key, void* output, size_t capacity) {
    fake::check(opened_, "read closed Preferences");
    ++fake::io.preferenceReads;
    if (fake::io.failPreferencesRead) return 0;
    const auto found = fake::io.preferences.find(name_ + "/" + key);
    if (found == fake::io.preferences.end() || capacity < found->second.size()) return 0;
    std::memcpy(output, found->second.data(), found->second.size());
    if (fake::io.corruptPreferencesReadOffset >= 0 &&
        static_cast<size_t>(fake::io.corruptPreferencesReadOffset) < found->second.size())
        static_cast<uint8_t*>(output)[fake::io.corruptPreferencesReadOffset] ^= 1;
    return found->second.size();
}
size_t Preferences::putBytes(const char* key, const void* data, size_t size) {
    ++fake::io.preferenceWriteCalls;
    fake::check(opened_ && !readOnly_, "write closed/read-only Preferences");
    if (fake::io.failPreferencesWrite) return 0;
    const auto* bytes = static_cast<const uint8_t*>(data);
    fake::io.preferences[name_ + "/" + key] = std::vector<uint8_t>(bytes, bytes + size);
    if (fake::io.onPreferencesWrite) fake::io.onPreferencesWrite();
    return size;
}
void Preferences::end() {
    if (opened_) --fake::io.openPreferences;
    opened_ = false;
}
void esp_fill_random(void* output, size_t length) {
    ++fake::io.randomCalls;
    auto* bytes = static_cast<uint8_t*>(output);
    for (size_t i = 0; i < length; ++i)
        bytes[i] = fake::io.zeroRandom ? 0 : static_cast<uint8_t>(i + fake::io.randomCalls);
}

PubSubClient::PubSubClient(WiFiClient&) { fake::io.clients.push_back(this); }
PubSubClient::~PubSubClient() {
    auto& clients = fake::io.clients;
    clients.erase(std::remove(clients.begin(), clients.end(), this), clients.end());
}
void PubSubClient::setCallback(Callback callback) {
    callback_ = callback;
    ++callbackChanges_;
}
bool PubSubClient::setBufferSize(uint16_t size) {
    if (fake::io.failBuffer) return false;
    buffer_.resize(size);
    return true;
}
void PubSubClient::setSocketTimeout(uint16_t timeout) { fake::io.timeout = timeout; }
void PubSubClient::setKeepAlive(uint16_t seconds) { fake::io.keepAlive = seconds; }
void PubSubClient::setServer(const char* host, uint16_t port) {
    fake::io.server = host;
    fake::io.port = port;
}
bool PubSubClient::connect(const char* id, const char* user, const char* password) {
    ++fake::io.connectCalls;
    fake::io.connectedId = id;
    // Keep only a boolean, never credentials in diagnostics/connection traces.
    fake::io.credentialsMatched = std::strcmp(user, "test-user") == 0 &&
        std::strcmp(password, "host-only-test-secret") == 0;
    connected_ = fake::io.connectOk;
    if (fake::io.onConnect) fake::io.onConnect();
    return connected_;
}
void PubSubClient::disconnect() {
    ++fake::io.disconnectCalls;
    connected_ = false;
    fake::io.incoming.clear();
}
bool PubSubClient::subscribe(const char* topic) {
    fake::io.subscriptions.emplace_back(topic);
    if (fake::io.subscriptionResults.empty()) return true;
    const bool result = fake::io.subscriptionResults.front();
    fake::io.subscriptionResults.pop_front();
    return result;
}
bool PubSubClient::loop() {
    ++fake::io.loopCalls;
    if (fake::io.onLoop) fake::io.onLoop();
    if (!connected_ || !fake::io.loopOk) {
        connected_ = false;
        return false;
    }
    while (!fake::io.incoming.empty()) {
        const fake::Packet packet = fake::io.incoming.front();
        fake::io.incoming.pop_front();
        deliver(packet.topic, packet.payload);
    }
    return true;
}
bool PubSubClient::publish(const char* topic, const char* payload, bool retained) {
    fake::io.published.push_back({topic, payload, retained});
    return connected_ && fake::io.publishOk && fake::io.rejectedPublishTopic != topic;
}
void PubSubClient::deliver(const std::string& topic, const std::string& payload) {
    fake::check(callback_ != nullptr, "MQTT callback not registered");
    fake::check(connected_, "cannot deliver MQTT while socket disconnected");
    std::vector<char> topicBytes(topic.begin(), topic.end());
    topicBytes.push_back(0);
    // Include a poison byte past the declared length, not an implicit C string.
    std::vector<uint8_t> payloadBytes(payload.begin(), payload.end());
    payloadBytes.push_back(0xA5);
    callback_(topicBytes.data(), payloadBytes.data(), static_cast<unsigned>(payload.size()));
}
