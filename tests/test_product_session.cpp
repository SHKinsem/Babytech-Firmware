#include "ProductSession.h"
#include "DisplayLinkCore.h"
#include <cassert>
#include <iostream>
#include <limits>

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
    bool availableValue = true;
    bool configurationValidValue = true;
    bool freshValue = true;
    bool faultValue = false;
    bool stationaryValue = true;
    bool resetResult = true;
    int32_t position = 100;
    int starts = 0;
    int stops = 0;
    int resets = 0;
    DemoExecution state = DemoExecution::Done;
    bool healthy() const override { return healthyValue; }
    bool available() const override { return availableValue; }
    bool configurationValid() const override { return configurationValidValue; }
    DemoEvidence evidence(uint8_t) const override {
        return {freshValue, stationaryValue, faultValue || !healthyValue, position};
    }
    bool start(const DemoScript&, bool, const std::array<int32_t, 256>&, uint32_t) override {
        ++starts;
        state = DemoExecution::Running;
        return true;
    }
    DemoExecution execution() const override { return state; }
    bool stop() override { ++stops; return healthyValue; }
    bool reset() override {
        ++resets;
        if (resetResult) healthyValue = true;
        return resetResult;
    }
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
    bool prepareResult = true;
    int attempts = 0;
    bool ready() const override { return ok; }
    bool prepare(ProductRun& run, uint32_t) override {
        ++attempts;
        if (ok && prepareResult) run.eventId = "durable-id";
        return ok && prepareResult;
    }
};

void assertRunEqual(const ProductRun& actual, const ProductRun& expected) {
    assert(actual.commandId == expected.commandId && actual.eventId == expected.eventId);
    assert(actual.source == expected.source && actual.babyId == expected.babyId);
    assert(actual.profileVersion == expected.profileVersion);
    assert(actual.recipe.waterMl == expected.recipe.waterMl);
    assert(actual.recipe.temperatureC == expected.recipe.temperatureC);
    assert(actual.recipe.powderGPer100Ml == expected.recipe.powderGPer100Ml);
    assert(actual.targetPowderG == expected.targetPowderG);
}

template <typename Check>
void assertReadOnly(const ProductSession& product, const DemoFlowController& flow,
                    const FakeExecutor& executor, const Guard& guard,
                    Check check, const char* expected) {
    const auto snapshot = product.displaySnapshot();
    const auto flowSnapshot = flow.snapshot();
    const std::string flowReason = flow.reason(), progress = product.progress();
    const auto context = product.context();
    const auto activeRun = product.activeRun();
    const bool active = product.active(), ownsMotion = product.ownsMotion();
    const bool cleaning = product.cleaning(), eventPending = product.eventPending();
    const bool busy = flow.busy(), reference = flow.referenceValid();
    const bool initializing = flow.initializing();
    const int starts = executor.starts, stops = executor.stops, resets = executor.resets;
    const int attempts = guard.attempts;
    for (int i = 0; i < 3; ++i) {
        const char* actual = check();
        assert(expected ? actual && std::string(actual) == expected : actual == nullptr);
    }
    assert(executor.starts == starts && executor.stops == stops && executor.resets == resets);
    assert(guard.attempts == attempts);
    assert(displaySnapshotsEqual(snapshot, product.displaySnapshot()));
    assert(displaySnapshotsEqual(flowSnapshot, flow.snapshot()));
    assert(flowReason == flow.reason() && progress == product.progress());
    assert(active == product.active() && ownsMotion == product.ownsMotion());
    assert(cleaning == product.cleaning() && eventPending == product.eventPending());
    assert(busy == flow.busy() && reference == flow.referenceValid());
    assert(initializing == flow.initializing());
    assertRunEqual(product.activeRun(), activeRun);
    assert(product.context().babyId == context.babyId);
    assert(product.context().profileVersion == context.profileVersion);
    assert(product.context().recipe.waterMl == context.recipe.waterMl);
    assert(product.context().recipe.temperatureC == context.recipe.temperatureC);
    assert(product.context().recipe.powderGPer100Ml == context.recipe.powderGPer100Ml);
}

void testPreparePreflight() {
    FakeExecutor executor;
    DemoFlowController flow(executor);
    initialize(flow, executor);
    ProductSession product(flow);
    Guard guard;
    product.setStartGuard(&guard);
    auto request = run();
    const ProductSession& session = product;
    auto check = [&](const char* reason) {
        assertReadOnly(session, flow, executor, guard,
                       [&] { return session.prepareRejection(request); }, reason);
    };
    check("non_consumable_demo_disabled");
    product.setExecutionAuthorized(true);
    product.setContextStorageReady(false);
    check("context_storage_fault");
    product.setContextStorageReady(true);
    product.setEventPending(true);
    check("busy");
    product.setEventPending(false);
    check("water_sensor_invalid");
    product.resources(true, true, true, 350, 20);
    check("low_water");
    product.resources(true, false, false, 350, 20);
    check("powder_sensor_invalid");
    product.resources(true, false, true, std::numeric_limits<float>::quiet_NaN(), 20);
    check("powder_sensor_invalid");
    product.resources(true, false, true, 50, 20);
    check("low_powder");
    product.resources(true, false, true, 350, 20);
    for (int value : {29, 501}) {
        request.recipe.waterMl = value;
        check("invalid_water_ml");
    }
    request = run();
    for (int value : {34, 61}) {
        request.recipe.temperatureC = value;
        check("invalid_temp");
    }
    request = run();
    for (float value : {0.9f, 50.1f, std::numeric_limits<float>::quiet_NaN(),
                        std::numeric_limits<float>::infinity()}) {
        request.recipe.powderGPer100Ml = value;
        check("invalid_powder_g_per_100ml");
    }
    request = run();
    request.babyId.clear();
    check("baby_context_missing");
    request = run();
    request.targetPowderG = 1.0f; // Admission derives the target without mutating this snapshot.
    product.resources(true, false, true, 94.9f, 20);
    check("low_powder");
    product.resources(true, false, true, 95, 20);
    check("event_storage_fault");
    assert(request.targetPowderG == 1.0f);
    guard.ok = true;
    executor.healthyValue = executor.availableValue = executor.freshValue = false;
    executor.configurationValidValue = executor.stationaryValue = false;
    check(nullptr); // Ready admission does not add a new live-feedback gate or tick.
    executor.healthyValue = executor.availableValue = executor.freshValue = true;
    executor.configurationValidValue = executor.stationaryValue = true;
    guard.ok = false;
    flow.invalidate();
    check("not_ready"); // Flow rejection keeps priority over the storage guard.
    assert(flow.initialize(21));
    check("not_ready");
    flow.tick(21);
    executor.state = DemoExecution::Done;
    flow.tick(22);
    guard.ok = true;
    check(nullptr);
    for (auto recipe : {ProductRecipe{30, 35, 1.0f}, ProductRecipe{500, 60, 50.0f}}) {
        request.recipe = recipe;
        product.resources(true, false, true, 350, 23);
        check(nullptr);
    }
    request = run();
    guard.prepareResult = false;
    const char* reason = nullptr;
    const int starts = executor.starts;
    assert(!product.startCloud(request, 23, reason));
    assert(std::string(reason) == "event_storage_fault" && guard.attempts == 1);
    assert(!product.active() && executor.starts == starts && executor.stops == 0);
    guard.prepareResult = true;
    assert(product.startCloud(request, 24, reason));
    assert(guard.attempts == 2 && product.activeRun().eventId == "durable-id");
    assert(product.activeRun().targetPowderG == 45.0f);
    check("busy");
    assertReadOnly(session, flow, executor, guard,
                   [&] { return session.cleanRejection(); }, "busy");
    assertReadOnly(session, flow, executor, guard,
                   [&] { return session.initializeRejection(); }, "busy");
}

void testInitializePreflight() {
    FakeExecutor executor;
    DemoFlowController flow(executor);
    ProductSession product(flow);
    Guard guard;
    product.setStartGuard(&guard);
    product.setContextStorageReady(false);
    auto check = [&](const char* reason) {
        assertReadOnly(product, flow, executor, guard,
                       [&] { return product.initializeRejection(); }, reason);
        assert(flow.canInitialize() == (reason == nullptr));
    };
    check("not_ready");
    DemoConfig config;
    config.configured = true;
    config.axes.push_back({1, 10, 0, true});
    assert(flow.apply(config));
    check(nullptr); // No prepare authorization, context, resources or guard gate here.
    product.setEventPending(true);
    assertReadOnly(product, flow, executor, guard,
                   [&] { return product.initializeRejection(); }, "busy");
    assert(!product.initialize(20) && executor.resets == 0 && executor.starts == 0);
    product.setEventPending(false);
    executor.availableValue = false;
    check("not_ready");
    assert(!product.initialize(20));
    executor.availableValue = true;
    executor.configurationValidValue = false;
    check(nullptr); // No reference yet: preserve the original initialization path.
    executor.configurationValidValue = true;
    executor.healthyValue = false;
    check(nullptr);
    executor.resetResult = false;
    check(nullptr); // A reset transport failure cannot be predicted by a read-only check.
    assert(!product.initialize(20) && executor.resets == 1);
    executor.resetResult = true;
    assert(product.initialize(21) && executor.resets == 2);
    check("busy");
    flow.tick(22);
    check("busy");
    flow.tick(23);
    executor.state = DemoExecution::Done;
    flow.tick(24);
    check(nullptr);
    executor.position = 111;
    check("not_ready");
    const std::string reasonBefore = flow.reason();
    assert(!product.initialize(25) && reasonBefore == flow.reason());
    executor.position = 110;
    check(nullptr);
    executor.position = 89;
    check("not_ready");
    executor.position = 90;
    check(nullptr);
    executor.position = 100;
    executor.stationaryValue = false;
    check("not_ready");
    executor.stationaryValue = true;
    executor.freshValue = false;
    check("not_ready");
    executor.freshValue = true;
    executor.faultValue = true;
    check("not_ready");
    executor.faultValue = false;
    executor.configurationValidValue = false;
    check("not_ready");
    executor.configurationValidValue = true;
    executor.healthyValue = false;
    check(nullptr); // Unhealthy Ready also enters reset, without a fresh-feedback gate.
    executor.healthyValue = true;
    assert(flow.start(30));
    flow.tick(30);
    flow.tick(60030);
    check("busy"); // A latched Error must finish its Stop before reset admission.
    flow.tick(60031);
    assert(flow.stage() == DisplayStage::Error && flow.referenceValid());
    executor.position = 200;
    check(nullptr); // Error is resettable even when its existing zero is not settled.
    assertReadOnly(product, flow, executor, guard,
                   [&] { return product.cleanRejection(); }, "error_state");
    assert(product.initialize(60032) && executor.resets == 3);
    flow.tick(60033);
    assert(flow.stage() == DisplayStage::NotReady && flow.referenceValid());
    check("not_ready");
    executor.position = 100;
    check(nullptr);
    const int starts = executor.starts;
    assert(product.initialize(60034));
    assert(flow.stage() == DisplayStage::Ready && executor.starts == starts);
}

void testCleanPreflight() {
    FakeExecutor executor;
    DemoFlowController flow(executor);
    initialize(flow, executor);
    ProductSession product(flow);
    Guard guard;
    product.setStartGuard(&guard);
    product.setContextStorageReady(false);
    auto check = [&](const char* reason) {
        assertReadOnly(product, flow, executor, guard,
                       [&] { return product.cleanRejection(); }, reason);
    };
    product.setEventPending(true);
    check("busy");
    const char* reason = nullptr;
    assert(!product.clean(20, reason) && std::string(reason) == "busy");
    product.setEventPending(false);
    executor.stationaryValue = false;
    executor.availableValue = false;
    executor.healthyValue = false;
    check(nullptr); // No new network/sensor/feedback gate before the original Stop.
    assert(!product.clean(20, reason) && std::string(reason) == "stop_unconfirmed");
    assert(executor.stops == 1 && !product.ownsMotion());
    check("busy");
    executor.stationaryValue = executor.availableValue = executor.healthyValue = true;
    flow.tick(21);
    check("error_state");
    assert(!product.clean(22, reason) && executor.stops == 1);
    assert(product.initialize(23));
    flow.tick(24);
    flow.tick(25);
    executor.state = DemoExecution::Done;
    flow.tick(26);
    check(nullptr);
    assert(product.clean(27, reason) && executor.stops == 2);
    check("busy");
    flow.tick(28);
    product.tick(28);
    assert(product.cleaning());
    check("busy");
    assertReadOnly(product, flow, executor, guard,
                   [&] { return product.initializeRejection(); }, "busy");
}

void testPairedSnapshot() {
    for (const char* source : {"local_touch", "cloud_command"}) {
        FakeExecutor executor;
        DemoFlowController flow(executor);
        initialize(flow, executor);
        ProductSession product(flow);
        product.setExecutionAuthorized(true);
        product.resources(true, false, true, 350, 20);
        FeedingContext context;
        context.babyId = "new-baby";
        context.recipe = {300, 60, 40.0f};
        context.profileVersion = 99;
        assert(product.applyContext(context));
        auto frozen = run("paired-1");
        frozen.eventId = "paired-event-1";
        frozen.source = source;
        frozen.profileVersion = 2;
        frozen.recipe = {123, 37, 23.4f};
        auto expected = frozen;
        expected.targetPowderG = 28.8f;
        const char* reason = nullptr;
        auto invalid = frozen;
        invalid.commandId.clear();
        assert(!product.startPaired(invalid, 21, reason));
        assert(std::string(reason) == "invalid_command_id");
        for (const char* badSource : {"", "cloud", "local", "LOCAL_TOUCH"}) {
            invalid = frozen;
            invalid.source = badSource;
            assert(!product.startPaired(invalid, 21, reason));
            assert(std::string(reason) == "invalid_source");
        }
        invalid = frozen;
        invalid.babyId.clear();
        assert(!product.startPaired(invalid, 21, reason));
        assert(std::string(reason) == "baby_context_missing"); // Never fill from new context.
        assert(!product.active() && executor.starts == 1 && executor.stops == 0);
        assert(product.clearContext(100));
        assert(product.startPaired(frozen, 22, reason) && reason == nullptr);
        assertRunEqual(product.activeRun(), expected); // No saved context is required for a full run.
        assert(frozen.targetPowderG == 0.0f && frozen.eventId == "paired-event-1");
        context.profileVersion = 101;
        assert(product.applyContext(context));
        assertRunEqual(product.activeRun(), expected);
        ProductTerminal terminal;
        if (std::string(source) == "local_touch") {
            product.networkState(false, 23);
            assert(product.active() && executor.stops == 0);
            for (uint32_t stage = 0; stage < 5; ++stage) {
                const uint32_t now = 24 + stage * 10;
                flow.tick(now);
                executor.state = DemoExecution::Done;
                flow.tick(now + 1);
                product.tick(now + 1);
            }
            assert(product.takeTerminal(terminal) && terminal.completed);
        } else {
            product.networkState(false, 23);
            assert(executor.stops == 1);
            flow.tick(24);
            product.tick(24);
            assert(product.takeTerminal(terminal) && !terminal.completed);
            assert(terminal.reason == "network_lost");
        }
        assertRunEqual(terminal.run, expected);
        assert(!product.takeTerminal(terminal));
    }
    FakeExecutor executor;
    DemoFlowController flow(executor);
    initialize(flow, executor);
    ProductSession product(flow);
    product.setExecutionAuthorized(true);
    product.resources(true, false, true, 350, 20);
    FeedingContext context;
    context.babyId = "legacy-baby";
    context.recipe = {150, 40, 20.0f};
    context.profileVersion = 3;
    assert(product.applyContext(context));
    auto cloud = run();
    cloud.babyId.clear();
    cloud.source = "ignored-legacy-source";
    const char* reason = nullptr;
    assert(!product.startCloud(run(""), 21, reason));
    assert(std::string(reason) == "invalid_command_id");
    assert(!product.startLocal("", 21, reason));
    assert(std::string(reason) == "invalid_command_id");
    assert(product.startCloud(cloud, 21, reason));
    assert(product.activeRun().babyId == "legacy-baby");
    assert(product.activeRun().profileVersion == 3);
    assert(product.activeRun().source == "cloud_command");
    assert(product.activeRun().recipe.waterMl == 180 && product.activeRun().targetPowderG == 45.0f);
}

int main() {
    testPreparePreflight();
    testInitializePreflight();
    testCleanPreflight();
    testPairedSnapshot();
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
        Guard guard;
        assertReadOnly(product, flow, executor, guard,
                       [&] { return product.prepareRejection(run()); }, "busy");
        assertReadOnly(product, flow, executor, guard,
                       [&] { return product.initializeRejection(); }, "busy");
        assertReadOnly(product, flow, executor, guard,
                       [&] { return product.cleanRejection(); }, "busy");
        ProductTerminal terminal;
        assert(product.takeTerminal(terminal));
        assert(!product.takeTerminal(terminal));
        assert(terminal.completed && terminal.run.commandId == "cmd-1");
        assert(terminal.run.babyId == "baby-1" && terminal.run.targetPowderG == 45.0f);
        product.setEventPending(false);
        flow.tick(3100);
        assert(!product.startCloud(run("cmd-3"), 3101, reason));
        assert(std::string(reason) == "bottle_full");
        assertReadOnly(product, flow, executor, guard,
                       [&] { return product.prepareRejection(run()); }, "bottle_full");
        assertReadOnly(product, flow, executor, guard,
                       [&] { return product.initializeRejection(); }, nullptr);
        assertReadOnly(product, flow, executor, guard,
                       [&] { return product.cleanRejection(); }, nullptr);
        assert(product.initialize(3102));
        assert(product.canStart() && std::string(product.progress()) == "ready");
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
