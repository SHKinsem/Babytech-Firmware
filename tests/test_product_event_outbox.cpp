#include "ProductEventOutbox.h"
#include <nvs.h>
#include <cassert>
#include <cstring>
#include <deque>
#include <iostream>
#include <optional>
#include <vector>

// Only platform I/O is replaced. Outbox, NVS adapter, journal, product session
// and motion supervision below are the production translation units.
namespace host {
uint32_t now = 100;
std::optional<std::string> disk, staged;
bool opened = false, dirty = false, ambiguousCommit = false;
esp_err_t openError = ESP_OK, sizeError = ESP_OK, readError = ESP_OK;
esp_err_t setError = ESP_OK, eraseError = ESP_OK, commitError = ESP_OK;
int erases = 0, commits = 0;
int openMode = NVS_READWRITE;
std::vector<std::string> published;
struct QueuedPublish { std::string payload, tag; };
std::deque<QueuedPublish> queued;
std::deque<CloudLink::PublishResult> results;
bool publishAccepted = true;
void reset() {
    assert(!opened);
    disk.reset(); staged.reset(); dirty = false; ambiguousCommit = false;
    openError = sizeError = readError = setError = eraseError = commitError = ESP_OK;
    erases = commits = 0; now = 100;
    published.clear(); queued.clear(); results.clear(); publishAccepted = true;
}
void check(nvs_handle_t handle, const char* key = "payload") {
    assert(opened && handle == 1 && std::string(key) == "payload");
}
void deliverQueued(bool success) {
    while (!queued.empty()) {
        const auto message = queued.front(); queued.pop_front();
        // CloudLink only emits results for accepted, tagged queue entries.
        if (!message.tag.empty()) {
            CloudLink::PublishResult result;
            assert(message.tag.size() < sizeof(result.tag));
            std::strcpy(result.tag, message.tag.c_str());
            result.accepted = success;
            results.push_back(result);
        }
    }
}
}
uint32_t millis() { return host::now; }
uint32_t esp_random() { static uint32_t value = 0; return ++value; }
esp_err_t nvs_open(const char* name, int mode, nvs_handle_t* handle) {
    assert(std::string(name) == "formulaevt" &&
           (mode == NVS_READWRITE || mode == NVS_READONLY) && !host::opened);
    host::openMode = mode;
    if (host::openError != ESP_OK) return host::openError;
    host::opened = true; host::dirty = false; host::staged = host::disk; *handle = 1;
    return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t h, const char* key, char* value, size_t* size) {
    host::check(h, key);
    const auto error = value ? host::readError : host::sizeError;
    if (error != ESP_OK) return error;
    if (!host::disk) return ESP_ERR_NVS_NOT_FOUND;
    const size_t required = host::disk->size() + 1;
    if (value && *size < required) return ESP_ERR_INVALID_SIZE;
    if (value) std::memcpy(value, host::disk->c_str(), required);
    *size = required;
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char* key, const char* value) {
    assert(host::openMode == NVS_READWRITE);
    host::check(h, key);
    if (host::setError != ESP_OK) return host::setError;
    host::staged = value; host::dirty = true;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char* key) {
    assert(host::openMode == NVS_READWRITE);
    host::check(h, key); ++host::erases;
    if (host::eraseError != ESP_OK) return host::eraseError;
    if (!host::staged) return ESP_ERR_NVS_NOT_FOUND;
    host::staged.reset(); host::dirty = true;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    assert(host::openMode == NVS_READWRITE);
    host::check(h); ++host::commits;
    if (host::dirty && (host::commitError == ESP_OK || host::ambiguousCommit))
        host::disk = host::staged;
    return host::commitError;
}
void nvs_close(nvs_handle_t h) { host::check(h); host::opened = false; }

// Socket/queue outcomes, not a simulation of the FreeRTOS MQTT worker.
void CloudLink::begin(const String&) { connected_ = true; reconnectRequested_ = false; }
void CloudLink::requestReconnect() { reconnectRequested_ = true; }
bool CloudLink::publish(const char* suffix, const String& payload, const char* tag) {
    assert(std::string(suffix) == "event");
    host::published.push_back(payload);
    if (host::publishAccepted) host::queued.push_back({payload, tag ? tag : ""});
    return host::publishAccepted;
}
bool CloudLink::takePublishResult(PublishResult& result) {
    if (host::results.empty()) return false;
    result = host::results.front(); host::results.pop_front(); return true;
}

using namespace motion;
struct Executor : DemoExecutor {
    bool healthyValue = true, stationaryValue = true, freshValue = true;
    int starts = 0, stops = 0;
    DemoExecution state = DemoExecution::Done;
    bool healthy() const override { return healthyValue; }
    bool available() const override { return true; }
    DemoEvidence evidence(uint8_t) const override {
        return {freshValue, stationaryValue, !healthyValue, 100};
    }
    bool start(const DemoScript&, bool, const std::array<int32_t, 256>&, uint32_t) override {
        ++starts; state = DemoExecution::Running; return true;
    }
    DemoExecution execution() const override { return state; }
    bool stop() override { ++stops; return healthyValue; }
    bool reset() override { return healthyValue; }
};
struct Rig {
    Executor executor;
    DemoFlowController flow{executor};
    ProductSession product{flow};
    ProductEventOutbox outbox;
    CloudLink cloud;
    Rig() {
        DemoConfig config;
        config.configured = true;
        config.axes.push_back({1, 10, 0, true});
        assert(flow.apply(config));
        product.setExecutionAuthorized(true);
        product.resources(true, false, true, 350, host::now);
        FeedingContext context;
        context.babyId = "baby-1"; context.profileVersion = 1;
        context.recipe = {180, 45, 25.0f};
        assert(product.applyContext(context));
        outbox.begin("bt-host-test", product);
    }
    void initialize() {
        assert(product.initialize(host::now));
        flow.tick(host::now);
        executor.state = DemoExecution::Done;
        flow.tick(++host::now);
        assert(product.canStart());
    }
    void start() {
        initialize();
        const char* reason = nullptr;
        assert(product.startLocal("local-1", ++host::now, reason));
        assert(host::disk && host::disk->find("_run_pending") != std::string::npos);
    }
    bool finish() {
        for (int i = 0; i < 5; ++i) {
            flow.tick(++host::now);
            executor.state = DemoExecution::Done;
            flow.tick(++host::now);
            product.tick(host::now);
        }
        ProductTerminal terminal;
        assert(product.takeTerminal(terminal) && terminal.completed);
        return outbox.queue(terminal, host::now);
    }
    void settle() { flow.tick(host::now += 3001); product.tick(host::now); }
};
DynamicJsonDocument receipt() {
    DynamicJsonDocument event(3072), result(512);
    assert(host::disk && !deserializeJson(event, *host::disk));
    result["type"] = "feeding_event_receipt";
    result["status"] = "stored";
    result["device_id"] = "bt-host-test";
    result["event_id"] = event["event_id"];
    return result;
}

void storageFailures() {
    for (int failure = 0; failure < 6; ++failure) {
        host::reset();
        if (failure == 0) host::openError = ESP_FAIL;
        if (failure == 1) host::sizeError = ESP_FAIL;
        if (failure == 2) { host::disk = "{}"; host::readError = ESP_FAIL; }
        if (failure == 3) host::disk = "";
        if (failure == 4) host::disk = std::string(2048, 'x');
        if (failure == 5) host::disk = "corrupt";
        const auto before = host::disk;
        Rig rig;
        assert(rig.product.eventPending() && !rig.product.canStart());
        assert(rig.executor.stops == 1 && rig.executor.starts == 0);
        assert(host::disk == before && !host::opened && host::erases == 0);
    }
    for (int failure = 0; failure < 3; ++failure) {
        host::reset(); Rig rig; rig.initialize();
        if (failure == 0) host::setError = ESP_FAIL;
        else { host::commitError = ESP_FAIL; host::ambiguousCommit = failure == 2; }
        const int starts = rig.executor.starts;
        const char* reason = nullptr;
        assert(!rig.product.startLocal("write-fails", ++host::now, reason));
        assert(std::string(reason) == "event_storage_fault");
        assert(rig.executor.starts == starts && !rig.product.canStart());
        assert(!rig.product.active() && !rig.flow.busy());
        host::setError = host::commitError = ESP_OK;
        assert(!rig.product.startLocal("do-not-overwrite", ++host::now, reason));
        assert(rig.executor.starts == starts && !host::opened);
    }
}

void receiptAndSecondRestart() {
    host::reset();
    {
        Rig first; first.start(); first.flow.tick(++host::now);
        assert(first.executor.starts == 2);
    }
    std::string replay;
    {
        Rig reboot;
        assert(reboot.executor.stops == 1 && reboot.executor.starts == 0);
        assert(host::disk->find("_run_pending") == std::string::npos);
        assert(host::disk->find("reboot_during_feed") != std::string::npos);
        replay = *host::disk;
        reboot.cloud.begin("");
        reboot.outbox.poll(reboot.cloud, reboot.product, host::now);
        auto ack = receipt();
        assert(!reboot.outbox.receiveReceipt(ack.as<JsonVariantConst>(), reboot.product));
        assert(host::disk == replay && host::erases == 0);
        reboot.executor.stationaryValue = false;
        reboot.settle(); // Stop times out; Cloud acknowledgement must not erase evidence.
        assert(!reboot.outbox.receiveReceipt(ack.as<JsonVariantConst>(), reboot.product));
        assert(host::disk == replay);
    }
    Rig second;
    assert(second.executor.stops == 1 && second.executor.starts == 0);
    assert(host::disk == replay && second.product.eventPending());
    auto ack = receipt();
    second.executor.freshValue = false; second.settle();
    assert(!second.outbox.receiveReceipt(ack.as<JsonVariantConst>(), second.product));
    second.executor.freshValue = true; second.settle();
    assert(second.product.pendingEventSettled());
    assert(second.outbox.receiveReceipt(ack.as<JsonVariantConst>(), second.product));
    assert(!host::disk && !second.product.eventPending());
    assert(!second.product.canStart()); // Receipt cannot restore mechanical reference.
    assert(!second.outbox.receiveReceipt(ack.as<JsonVariantConst>(), second.product));
}

void retryAndClearFailures() {
    host::reset(); Rig rig; rig.start();
    const auto journal = host::disk;
    host::commitError = ESP_FAIL;
    assert(!rig.finish());
    assert(host::disk == journal && rig.product.eventPending());
    rig.outbox.poll(rig.cloud, rig.product, host::now);
    assert(host::published.empty() && host::disk == journal);
    host::commitError = ESP_OK;
    rig.outbox.poll(rig.cloud, rig.product, host::now += 5000);
    assert(host::published.empty() && host::disk != journal);
    assert(host::disk->find("feeding_completed") != std::string::npos);
    const auto terminal = host::disk;
    rig.cloud.begin(""); host::publishAccepted = false;
    rig.outbox.poll(rig.cloud, rig.product, host::now += 5000);
    assert(host::published.size() == 1 && host::disk == terminal);
    assert(host::queued.empty());
    host::publishAccepted = true;
    rig.outbox.poll(rig.cloud, rig.product, host::now += 5000);
    assert(host::published.size() == 2 && host::published[0] == host::published[1]);
    assert(host::queued.size() == 1 && host::disk == terminal && rig.product.eventPending());
    host::deliverQueued(true);
    rig.outbox.poll(rig.cloud, rig.product, ++host::now);
    assert(host::results.empty() && host::disk == terminal && rig.product.eventPending());
    // Another tagged publish can leave a result in the shared result queue.
    assert(rig.cloud.publish("event", *terminal, "unrelated-publication"));
    host::deliverQueued(true);
    assert(host::results.size() == 1);
    rig.outbox.poll(rig.cloud, rig.product, ++host::now);
    assert(host::results.empty() && host::disk == terminal && rig.product.eventPending());
    rig.settle();
    auto ack = receipt();
    ack["event_id"] = "wrong";
    assert(!rig.outbox.receiveReceipt(ack.as<JsonVariantConst>(), rig.product));
    assert(host::erases == 0);
    ack = receipt();
    host::eraseError = ESP_FAIL;
    assert(!rig.outbox.receiveReceipt(ack.as<JsonVariantConst>(), rig.product));
    assert(host::disk == terminal && rig.product.eventPending());
    host::eraseError = ESP_OK; host::commitError = ESP_FAIL;
    assert(!rig.outbox.receiveReceipt(ack.as<JsonVariantConst>(), rig.product));
    assert(host::disk == terminal && rig.product.eventPending());
    host::ambiguousCommit = true;
    assert(!rig.outbox.receiveReceipt(ack.as<JsonVariantConst>(), rig.product));
    assert(!host::disk && rig.product.eventPending());
    host::commitError = ESP_OK;
    assert(rig.outbox.receiveReceipt(ack.as<JsonVariantConst>(), rig.product));
    assert(!rig.product.eventPending() && !host::opened);
}

void recoveryPersistenceRetry() {
    host::reset();
    { Rig first; first.start(); }
    const auto journal = host::disk;
    host::commitError = ESP_FAIL;
    Rig reboot;
    auto ack = receipt();
    reboot.settle();
    assert(reboot.product.pendingEventSettled());
    assert(!reboot.outbox.receiveReceipt(ack.as<JsonVariantConst>(), reboot.product));
    reboot.cloud.begin("");
    reboot.outbox.poll(reboot.cloud, reboot.product, host::now);
    assert(host::disk == journal && host::published.empty());
    host::commitError = ESP_OK;
    reboot.outbox.poll(reboot.cloud, reboot.product, host::now += 5000);
    assert(host::published.size() == 1 && host::disk != journal);
    assert(host::published[0] == *host::disk);
    assert(reboot.outbox.receiveReceipt(ack.as<JsonVariantConst>(), reboot.product));
    assert(!host::disk && !reboot.product.eventPending());

    host::reset();
    { Rig first; first.start(); assert(first.finish()); }
    const auto terminal = host::disk;
    Rig completed;
    completed.cloud.begin("");
    completed.outbox.poll(completed.cloud, completed.product, host::now);
    assert(host::disk == terminal && host::published.size() == 1);
    assert(host::published[0] == *terminal);
    assert(terminal->find("feeding_completed") != std::string::npos);
    assert(terminal->find("reboot_during_feed") == std::string::npos);
    assert(completed.executor.starts == 0 && completed.executor.stops == 1);
}

void readonlyMigrationInspection() {
    using State = ProductEventOutbox::LegacyState;
    for (int scenario = 0; scenario < 8; ++scenario) {
        host::reset();
        State expected = State::ReadError;
        if (scenario == 0) expected = State::Empty;
        if (scenario == 1) { host::openError = ESP_ERR_NVS_NOT_FOUND; expected = State::Empty; }
        if (scenario == 2) host::openError = ESP_FAIL;
        if (scenario == 3) host::sizeError = ESP_FAIL;
        if (scenario == 4) host::disk = "";
        if (scenario == 5) host::disk = std::string(2048, 'x');
        if (scenario == 6) { host::disk = "unparseable evidence"; expected = State::Present; }
        if (scenario == 7) {
            { Rig old; old.start(); }
            expected = State::Present;
        }
        const auto before = host::disk;
        const int commits = host::commits;
        assert(ProductEventOutbox::inspectLegacyState() == expected);
        assert(host::openMode == NVS_READONLY && !host::opened);
        assert(host::disk == before && host::commits == commits && host::erases == 0);
    }
}

int main() {
    readonlyMigrationInspection();
    storageFailures();
    receiptAndSecondRestart();
    retryAndClearFailures();
    recoveryPersistenceRetry();
    std::cout << "PASS production outbox/NVS adapter: fault, receipt, stop and restart integration\n";
}
