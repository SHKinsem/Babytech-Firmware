#include "ProductSession.h"
#include "DisplayLinkCore.h"
#include <cassert>
#include <iostream>

using namespace motion;
using namespace babytech::display;

ProductSession* screenProduct = nullptr;
bool screenStart(uint32_t now) {
    const char* rejection = nullptr;
    return screenProduct->startLocal("local-touch-1", now, rejection);
}
bool screenInitialize(uint32_t now) { return screenProduct->initialize(now); }
DisplaySnapshot screenSnapshot() { return screenProduct->displaySnapshot(); }

bool sendIntent(DisplayLinkCore& link, DisplayIntent intent, uint32_t sequence, uint32_t now) {
    uint8_t payload[96], frame[108], reply[108];
    const size_t nPayload = encodeDisplayIntentPayload(intent, payload, sizeof(payload));
    const size_t nFrame = encodeDisplayFrame(DisplayMessageType::Intent, sequence, payload,
                                             nPayload, frame, sizeof(frame));
    size_t nReply = 0;
    for (size_t i = 0; i < nFrame; ++i)
        nReply = link.receive(frame[i], now, reply, sizeof(reply));
    assert(nReply > 0);
    DisplayFrame decoded;
    DisplayFrameParser parser;
    for (size_t i = 0; i < nReply; ++i) parser.push(reply[i], decoded);
    DisplayAck ack;
    assert(decodeDisplayAckPayload(decoded, ack));
    return ack.accepted;
}

struct FakeExecutor : DemoExecutor {
    bool healthyValue = true;
    bool stationaryValue = true;
    int starts = 0;
    int stops = 0;
    DemoExecution state = DemoExecution::Done;
    bool healthy() const override { return healthyValue; }
    bool available() const override { return true; }
    DemoEvidence evidence(uint8_t) const override {
        return {true, stationaryValue, !healthyValue, 100};
    }
    bool start(const DemoScript&, bool, const std::array<int32_t, 256>&, uint32_t) override {
        ++starts;
        state = DemoExecution::Running;
        return true;
    }
    DemoExecution execution() const override { return state; }
    bool stop() override { ++stops; return healthyValue; }
    bool reset() override { healthyValue = true; return true; }
};

void initialize(DemoFlowController& flow, FakeExecutor& executor) {
    DemoConfig config;
    config.configured = true;
    config.axes.push_back({1, 10, 0, true});
    assert(flow.apply(config));
    assert(flow.initialize(10));
    flow.tick(10);
    executor.state = DemoExecution::Done;
    flow.tick(11);
    assert(flow.stage() == DisplayStage::Ready);
}

ProductRun run(const char* id = "cmd-1") {
    ProductRun result;
    result.commandId = id;
    result.recipe = {180, 45, 25.0f};
    result.babyId = "baby-1";
    return result;
}

struct Guard : ProductStartGuard {
    bool ok = false;
    int attempts = 0;
    bool ready() const override { return ok; }
    bool prepare(ProductRun& run, uint32_t) override {
        ++attempts;
        if (ok) run.eventId = "durable-id";
        return ok;
    }
};

int main() {
    {
        FakeExecutor executor;
        DemoFlowController flow(executor);
        initialize(flow, executor);
        ProductSession product(flow);
        product.setEventPending(true);
        const int started = executor.starts;
        executor.stationaryValue = false;
        product.recoverAfterRestart(100);
        assert(executor.stops == 1 && flow.busy() && !flow.referenceValid());
        assert(!product.pendingEventSettled());
        flow.tick(101);
        assert(flow.busy() && executor.starts == started);
        executor.stationaryValue = true;
        flow.tick(102);
        assert(!flow.busy() && executor.starts == started && product.eventPending());
        assert(product.pendingEventSettled());
        executor.stationaryValue = false;
        assert(!product.pendingEventSettled());
        executor.stationaryValue = true;
        executor.healthyValue = false;
        assert(!product.pendingEventSettled());
        ProductTerminal terminal;
        assert(!product.takeTerminal(terminal)); // Outbox owns the recovered result.
    }
    {
        FakeExecutor executor;
        DemoFlowController flow(executor);
        initialize(flow, executor);
        ProductSession product(flow);
        product.setExecutionAuthorized(true);
        product.resources(true, false, true, 350, 20);
        const char* reason = nullptr;
        assert(product.startCloud(run(), 21, reason));
        product.networkState(false, 22);
        product.networkState(false, 23);
        assert(executor.stops == 1);
        ProductTerminal terminal;
        assert(!product.takeTerminal(terminal));
        flow.tick(24);
        product.tick(24);
        assert(product.takeTerminal(terminal));
        assert(terminal.reason == "network_lost" && terminal.errorCode == "E_NETWORK_LOST");
        assert(!product.takeTerminal(terminal));
    }
    {
        FakeExecutor executor;
        DemoFlowController flow(executor);
        initialize(flow, executor);
        ProductSession product(flow);
        product.setExecutionAuthorized(true);
        const char* reason = nullptr;
        assert(!product.canStart());
        assert(!product.startCloud(run(), 20, reason));
        assert(std::string(reason) == "water_sensor_invalid");
        product.resources(true, false, true, 350, 21);
        assert(product.canStart());
        auto missingBaby = run();
        missingBaby.babyId.clear();
        const int beforeMissingBaby = executor.starts;
        assert(!product.startCloud(missingBaby, 21, reason));
        assert(std::string(reason) == "baby_context_missing" && executor.starts == beforeMissingBaby);
        assert(product.startCloud(run(), 22, reason));
        assert(product.active() && std::string(product.progress()) == "unscrewing_cap");
        assert(product.activeRun().targetPowderG == 45.0f);
        assert(!product.startCloud(run("cmd-2"), 22, reason));
        assert(std::string(reason) == "busy" && executor.starts == 1);
        for (uint32_t stage = 0; stage < 5; ++stage) {
            const uint32_t now = 23 + stage * 10;
            flow.tick(now);
            executor.state = DemoExecution::Done;
            flow.tick(now + 1);
            product.tick(now + 1);
        }
        assert(!product.active() && product.eventPending());
        assert(std::string(product.progress()) == "complete");
        ProductTerminal terminal;
        assert(product.takeTerminal(terminal));
        assert(!product.takeTerminal(terminal));
        assert(terminal.completed && terminal.run.commandId == "cmd-1");
        assert(terminal.run.babyId == "baby-1" && terminal.run.targetPowderG == 45.0f);
        product.setEventPending(false);
        flow.tick(3100);
        assert(!product.startCloud(run("cmd-3"), 3101, reason));
        assert(std::string(reason) == "bottle_full");
    }
    {
        FakeExecutor executor;
        DemoFlowController flow(executor);
        initialize(flow, executor);
        ProductSession product(flow);
        product.setExecutionAuthorized(true);
        product.resources(true, false, true, 350, 20);
        FeedingContext context;
        context.babyId = "baby-1";
        context.babyName = "Mia";
        context.formulaBrand = "Friso";
        context.recipe = {150, 45, 25.0f};
        context.profileVersion = 2;
        assert(product.applyContext(context));
        const char* reason = nullptr;
        product.setContextStorageReady(false);
        assert(!product.startLocal("local-storage-fault", 21, reason));
        assert(std::string(reason) == "context_storage_fault");
        product.setContextStorageReady(true);
        assert(product.startLocal("local-1", 21, reason));
        assert(product.activeRun().babyId == "baby-1");
        assert(product.activeRun().targetPowderG == 37.5f);
        context.babyId = "baby-2";
        context.profileVersion = 3;
        assert(product.applyContext(context));
        assert(product.activeRun().babyId == "baby-1");
        context.profileVersion = 2;
        assert(!product.applyContext(context));
        context.profileVersion = 3;
        assert(!product.applyContext(context));
        bool wasActive = false;
        assert(product.stop(22, wasActive) && wasActive);
        ProductTerminal terminal;
        assert(!product.takeTerminal(terminal));
        flow.tick(23);
        product.tick(23);
        assert(product.takeTerminal(terminal));
        assert(!terminal.completed && terminal.reason == "stopped");
        assert(terminal.run.babyId == "baby-1");
        assert(!product.startLocal("local-2", 23, reason));
        assert(std::string(reason) == "busy");
        product.setEventPending(false);
        assert(!product.clearContext(3));
        assert(product.clearContext(4));
        assert(!product.hasContext() && product.context().profileVersion == 4);
        assert(!product.startLocal("local-3", 24, reason));
        assert(std::string(reason) == "baby_context_missing");
        context.profileVersion = 1;
        assert(!product.applyContext(context));
        context.profileVersion = 5;
        assert(product.applyContext(context));
        assert(product.hasContext() && product.context().babyId == "baby-2");
    }
    {
        FakeExecutor executor;
        DemoFlowController flow(executor);
        initialize(flow, executor);
        ProductSession product(flow);
        product.setExecutionAuthorized(true);
        product.resources(true, false, true, 350, 20);
        const char* reason = nullptr;
        assert(product.startCloud(run(), 21, reason));
        executor.healthyValue = false;
        product.resources(true, true, true, 350, 22);
        ProductTerminal terminal;
        assert(product.takeTerminal(terminal));
        assert(!terminal.completed && terminal.errorCode == "E_CAN_FAULT");
        assert(std::string(product.progress()) == "error");
    }
    {
        FakeExecutor executor;
        DemoFlowController flow(executor);
        initialize(flow, executor);
        ProductSession product(flow);
        product.setExecutionAuthorized(true);
        product.resources(true, false, true, 350, 20);
        FeedingContext context;
        context.babyId = "screen-baby";
        context.babyName = "Mia";
        context.formulaBrand = "Friso";
        context.recipe = {180, 45, 25.0f};
        context.profileVersion = 2;
        assert(product.applyContext(context));
        screenProduct = &product;
        Guard guard;
        product.setStartGuard(&guard);
        const int beforeStart = executor.starts;
        assert(!product.canStart());
        const char* storageReason = nullptr;
        assert(!product.startLocal("failed-journal", 20, storageReason));
        assert(std::string(storageReason) == "event_storage_fault");
        assert(executor.starts == beforeStart);
        guard.ok = true;
        assert(product.displaySnapshot().startEnabled);
        assert(!product.displaySnapshot().cloudConnected);
        DisplayLinkCore link(flow, screenStart, screenInitialize, screenSnapshot);
        assert(sendIntent(link, DisplayIntent::StartFeeding, 7, 21));
        assert(sendIntent(link, DisplayIntent::StartFeeding, 7, 22));
        assert(!sendIntent(link, DisplayIntent::StartFeeding, 8, 23));
        assert(product.activeRun().source == "local_touch");
        assert(product.activeRun().babyId == "screen-baby");
        assert(product.activeRun().eventId == "durable-id");
        product.networkState(false, 23);
        assert(product.active());
        ProductTerminal offlineTerminal;
        assert(!product.takeTerminal(offlineTerminal));
        assert(std::string(product.displaySnapshot().babyName.data()) == "Mia");
        flow.tick(24);
        assert(executor.starts == 2);
        screenProduct = nullptr;
    }
    {
        FakeExecutor executor;
        DemoFlowController flow(executor);
        initialize(flow, executor);
        ProductSession product(flow);
        FeedingContext context;
        context.babyId = "screen-baby";
        context.recipe = {180, 45, 25.0f};
        context.profileVersion = 2;
        for (int i = 0; i < 11; ++i) context.babyName += "\xE5\xAE\x9D";
        for (int i = 0; i < 8; ++i) context.formulaBrand += "\xF0\x9F\xA7\xB4";
        assert(product.applyContext(context));
        const auto snapshot = product.displaySnapshot();
        assert(std::string(snapshot.babyName.data()) == context.babyName.substr(0, 30));
        assert(std::string(snapshot.formulaBrand.data()) == context.formulaBrand.substr(0, 28));
    }
    {
        FakeExecutor executor;
        DemoFlowController flow(executor);
        initialize(flow, executor);
        ProductSession product(flow);
        const char* reason = nullptr;
        assert(product.clean(20, reason));
        assert(!product.cleaning() && product.ownsMotion());
        assert(std::string(product.progress()) == "noready");
        assert(!product.waterValid());
        flow.tick(21);
        product.tick(21);
        assert(product.cleaning() && std::string(product.progress()) == "cleaning");
        bool wasActive = false;
        assert(product.stop(22, wasActive) && !wasActive);
        assert(!product.cleaning());
    }
    {
        FakeExecutor executor;
        DemoFlowController flow(executor);
        initialize(flow, executor);
        ProductSession product(flow);
        executor.stationaryValue = false;
        const char* reason = nullptr;
        assert(product.clean(20, reason));
        flow.tick(3021);
        product.tick(3021);
        assert(!product.cleaning() && std::string(product.progress()) == "error");
    }
    {
        FakeExecutor executor;
        DemoFlowController flow(executor);
        initialize(flow, executor);
        ProductSession product(flow);
        executor.healthyValue = false;
        const char* reason = nullptr;
        assert(!product.clean(20, reason));
        assert(std::string(reason) == "stop_unconfirmed");
        assert(!product.cleaning() && std::string(product.progress()) == "error");
    }
    {
        FakeExecutor executor;
        DemoFlowController flow(executor);
        initialize(flow, executor);
        ProductSession product(flow);
        product.setExecutionAuthorized(true);
        product.resources(true, false, true, 350, 20);
        const char* reason = nullptr;
        assert(product.startCloud(run(), 21, reason));
        executor.healthyValue = false;
        bool wasActive = false;
        assert(!product.stop(22, wasActive) && wasActive);
        ProductTerminal terminal;
        assert(product.takeTerminal(terminal));
        assert(!terminal.completed && terminal.reason == "stop_unconfirmed");
        assert(terminal.errorCode == "E_CAN_FAULT");
    }
    std::cout << "PASS product session gates, terminal result and context\n";
}
