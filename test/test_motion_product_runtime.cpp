#include "MotionProductRuntime.h"
#include "MotionStateRecovery.h"
#include "ReadOnlyBoardLink.h"
#include "FakeBrainNvs.h"
#include "FakeProductCrypto.h"
#include "brain_stop_target.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace babytech::boardlink;
namespace v4 = babytech::v4;
namespace fake = fake_brain;
using fake::Bytes;
using fake::Op;
using fake::io;
using babytech::display::DisplayStage;
using babytech::display::DisplayError;

namespace {
unsigned scenarios = 0, failures = 0, deliveries = 0, lookups = 0;
unsigned uartExchanges = 0, uartShortWrites = 0, uartZeroWrites = 0, uartFrames = 0;
unsigned brainSenderScenarios = 0;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + \
    std::to_string(__LINE__) + ": " #x); } while (false)

v4::Pairing pairing() {
    v4::Pairing p;
    p.role = v4::Role::Motion;
    std::strcpy(p.deviceId, "Babytech_01-runtime");
    std::strcpy(p.epoch, "0123456789abcdef0123456789abcdef");
    std::strcpy(p.localPhysicalId, "012345abcdef");
    std::strcpy(p.peerPhysicalId, "fedcba987654");
    return p;
}
ProductContext context() {
    ProductContext c;
    std::strcpy(c.deviceId, pairing().deviceId);
    c.profileVersion = 10;
    std::strcpy(c.babyId, "baby-original");
    std::strcpy(c.babyName, "Full baby name");
    std::strcpy(c.formulaBrand, "Full formula brand");
    c.waterMl = 120;
    c.temperatureC = 40;
    c.powderGPer100Ml = 13.5f;
    CHECK(validProductContext(c));
    return c;
}
ProductRequest request(uint64_t sequence = 20, ProductCommand command = ProductCommand::Prepare,
                       v4::Source source = v4::Source::LocalTouch) {
    ProductRequest r;
    r.source = source;
    r.sequence = sequence;
    r.command = command;
    std::strcpy(r.deviceId, pairing().deviceId);
    if (source == v4::Source::LocalTouch) {
        auto brain = pairing();
        brain.role = v4::Role::Brain;
        std::swap(brain.localPhysicalId, brain.peerPhysicalId);
        CHECK(makeLocalCommandId(brain, sequence, r.commandId));
    } else std::snprintf(r.commandId, sizeof(r.commandId), "cloud-%llu",
                         static_cast<unsigned long long>(sequence));
    if (command == ProductCommand::Prepare) {
        std::strcpy(r.babyId, context().babyId);
        r.profileVersion = 10;
        r.waterMl = 180;
        r.temperatureC = 45;
        r.powderGPer100Ml = 13.5f;
    } else if (command == ProductCommand::SetTargetTemp) r.temperatureC = 50;
    CHECK(validProductRequest(r));
    return r;
}
std::string executionId(uint64_t value) {
    char text[33];
    std::snprintf(text, sizeof(text), "%032llx", static_cast<unsigned long long>(value));
    return text;
}
Bytes encode(const MotionState& state) {
    std::array<uint8_t, kMotionStateMaxSize> bytes{};
    const size_t length = encodeMotionState(state, bytes.data(), bytes.size());
    CHECK(length);
    return Bytes(bytes.begin(), bytes.begin() + length);
}
MotionState decodedDisk() {
    const auto& bytes = io.disk.at("productstate").at("record").bytes;
    MotionState state;
    CHECK(decodeMotionState(bytes.data(), bytes.size(), state));
    return state;
}
fake::Database protectedData() {
    auto disk = io.disk;
    const auto found = disk.find("productstate");
    if (found != disk.end()) {
        found->second.erase("record");
        if (found->second.empty()) disk.erase(found);
    }
    return disk;
}
void audit() {
    fake::verifyFaults();
    CHECK(io.handles.empty() && !fake::count(Op::Erase) && !fake::count(Op::Init));
    for (const auto& call : io.calls) {
        CHECK(call.name == "productstate");
        CHECK(call.key.empty() || call.key == "record");
    }
}
void scenario(const std::string& name, const std::function<void()>& run) {
    fake::reset();
    fake_product_crypto::reset();
    for (const char* ns : {"productpair", "brainstate", "productctx", "outbox", "wifi-cfg",
                           "actuatorcfg", "sensorcfg"})
        io.disk[ns]["record"] = {{0, 0xff, 1, 2}, fake::Type::Blob};
    io.disk["productstate"]["unrelated"] = {{1, 2, 3}, fake::Type::U32};
    const auto protectedBefore = protectedData();
    ++scenarios;
    try {
        run();
        audit();
        CHECK(protectedData() == protectedBefore);
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "FAIL %s: %s\n", name.c_str(), error.what());
        io.before = {};
        io.handles.clear();
    }
}

// Only the executor/feedback, monotonic clock and board ownership are faked.
// Session, flow, runtime, decisions, result lookup and SHA remain production.
struct Executor : motion::DemoExecutor {
    bool healthyValue = true, availableValue = true, startValue = true, stopValue = true;
    motion::DemoEvidence sample{true, true, false, 0};
    motion::DemoExecution state = motion::DemoExecution::Running;
    DisplayError failure = DisplayError::CanFault;
    unsigned starts = 0, stops = 0, resets = 0;
    std::function<void()> beforeStart, beforeStop;
    bool healthy() const override { return healthyValue; }
    bool available() const override { return availableValue; }
    motion::DemoEvidence evidence(uint8_t id) const override {
        CHECK(id == 1);
        return sample;
    }
    bool start(const motion::DemoScript&, bool, const std::array<int32_t, 256>&, uint32_t) override {
        if (beforeStart) beforeStart();
        ++starts;
        state = motion::DemoExecution::Running;
        sample.fresh = true;
        sample.stationary = false;
        return startValue;
    }
    motion::DemoExecution execution() const override { return state; }
    DisplayError failureError() const override { return failure; }
    bool stop() override {
        if (beforeStop) beforeStop();
        ++stops;
        sample.fresh = sample.stationary = false;
        return stopValue;
    }
    bool reset() override { ++resets; return true; }
    void confirm() { sample = {true, true, false, 0}; }
    bool stopConfirmed() const { return sample.fresh && sample.stationary && !sample.fault; }
};
struct Hardware : motion::MotionProductHardware {
    explicit Hardware(Executor& executor) : executor(executor) {}
    Executor& executor;
    uint32_t clock = 1000;
    const char* owner = nullptr;
    bool executionValue = true;
    unsigned generated = 0;
    bool workbenchWriter = false, workbenchFresh = true, workbenchSettled = true;
    unsigned workbenchStops = 0, recoveryStops = 0;
    char recoveryId[33]{};
    std::function<void()> beforeId;
    uint32_t nowMs() const override { return clock; }
    const char* unavailable() const override { return owner; }
    bool newExecution(char (&id)[33]) override {
        if (beforeId) beforeId();
        ++generated;
        if (!executionValue) return false;
        std::strcpy(id, executionId(1000 + generated).c_str());
        return true;
    }
    bool stationary() const override {
        return executor.sample.fresh && executor.sample.stationary && !executor.sample.fault;
    }
    bool workbenchBusy() const override { return workbenchWriter; }
    bool workbenchStationary() const override { return workbenchFresh && workbenchSettled; }
    void stopWorkbench(uint32_t) override {
        ++workbenchStops;
        workbenchWriter = false;
        workbenchFresh = false;
    }
    const char* recoveringExecutionId() const override { return recoveryId; }
    void stopRecovery(uint32_t) override { ++recoveryStops; }
};
struct Fixture {
    MotionStateStore store;
    Executor executor;
    motion::DemoFlowController flow{executor};
    motion::ProductSession product{flow};
    Hardware hardware{executor};
    motion::MotionProductRuntime runtime{store, product, flow, hardware};
    explicit Fixture(bool ready = true, unsigned queued = 0, bool restored = false, bool projectContext = true) {
        const auto c = context();
        if (restored) CHECK(store.load(pairing()) == MotionLoad::Ready);
        else {
            CHECK(store.installInitial(pairing(), &c) == MotionWrite::Stored);
            for (unsigned i = 0; i < queued; ++i) {
                const auto r = request(1 + i, ProductCommand::Prepare, v4::Source::CloudCommand);
                const auto id = executionId(100 + i);
                CHECK(store.recordDecision(r, true, "accepted", id.c_str()) == MotionWrite::Stored);
                CHECK(store.finishFeeding(id.c_str(), i % 2 == 0, i % 2 == 0 ? "" : "stopped",
                                         i % 2 == 0 ? "" : "E_STOPPED", 500 + i) == MotionWrite::Stored);
                CHECK(store.archiveFeeding(true) == MotionWrite::Stored);
            }
        }
        motion::DemoConfig config;
        config.configured = true;
        config.axes.push_back({1, 10, 0, true});
        config.initialization.commands = {"initialize"};
        for (auto& stage : config.stages) stage.commands = {"stage"};
        CHECK(flow.apply(config));
        motion::FeedingContext feeding;
        feeding.babyId = c.babyId;
        feeding.babyName = c.babyName;
        feeding.formulaBrand = c.formulaBrand;
        feeding.profileVersion = c.profileVersion;
        feeding.recipe = {c.waterMl, c.temperatureC, c.powderGPer100Ml};
        CHECK(product.applyContext(feeding));
        if (projectContext) {
            ContextResult contextResult;
            CHECK(runtime.context(c, hardware.clock, contextResult));
            CHECK(contextResult.status == ContextStatus::Unchanged);
        }
        product.setExecutionAuthorized(true);
        product.resources(true, false, true, 300, hardware.clock);
        if (ready) {
            CHECK(product.initialize(900));
            flow.tick(901);
            executor.state = motion::DemoExecution::Done;
            executor.confirm();
            flow.tick(902);
            product.tick(902);
            CHECK(flow.startEnabled() && product.canStart());
        }
        executor.starts = executor.stops = executor.resets = 0;
        CHECK(sameMotionState(store.state(), decodedDisk()));
        // All setup is complete. Reopen/commit occurrence numbers now describe
        // only the actual runtime under test, while durable bytes are retained.
        fake::reboot();
    }
    ~Fixture() { io.before = {}; }
    CommandResult deliver(const ProductRequest& r, uint16_t ttl = 5000, uint32_t received = 1000,
                          bool expectReady = true) {
        CommandMessage message;
        message.request = r;
        message.remainingTtlMs = ttl;
        if (ttl && ttl <= 5000) {
            v4::Message wire;
            CommandMessage decoded;
            CHECK(encodeCommand(message, wire) && decodeCommand(wire, decoded));
            CHECK(sameProductRequest(decoded.request, r) && decoded.remainingTtlMs == ttl);
            message = decoded;
        }
        CommandResult result;
        CHECK(runtime.command(message, received, result));
        CHECK(result.source == r.source && result.sequence == r.sequence && !std::strcmp(result.commandId, r.commandId));
        CHECK(runtime.resultReady(result) == expectReady);
        if (expectReady) {
            v4::Message wire;
            CommandResult decoded;
            CHECK(encodeCommandResult(result, wire) && decodeCommandResult(wire, decoded));
            CHECK(result.accepted == decoded.accepted && !std::strcmp(result.reason, decoded.reason));
        }
        ++deliveries;
        return result;
    }
    void tick(uint32_t now) {
        hardware.clock = now;
        flow.tick(now);
        product.tick(now);
        runtime.poll(now);
    }
    void poll(uint32_t now) { hardware.clock = now; runtime.poll(now); }
    uint32_t finishFeed(uint32_t began = 1010, bool stationary = false) {
        for (unsigned i = 0; i < 5; ++i) {
            tick(began + 2 * i);
            executor.state = motion::DemoExecution::Done;
            if (stationary && i == 4) executor.confirm();
            tick(began + 2 * i + 1);
        }
        CHECK(!product.active() && executor.starts == 5);
        return began + 9;
    }
};
void decision(const CommandResult& result, bool accepted, const char* reason) {
    CHECK(result.accepted == accepted && !std::strcmp(result.reason, reason));
}
std::string digestHex(const ProductRequest& request) {
    uint8_t digest[kProductDigestSize];
    CHECK(requestDigest(request, digest));
    std::string hex;
    for (uint8_t byte : digest) {
        hex += "0123456789abcdef"[byte >> 4];
        hex += "0123456789abcdef"[byte & 15];
    }
    return hex;
}
QueriedResult lookup(const MotionStateStore& store, const ProductRequest& request,
                    bool accepted, const char* reason, MotionOutcome outcome = MotionOutcome::None) {
    ResultQuery query;
    query.source = request.source;
    query.sequence = request.sequence;
    std::strcpy(query.deviceId, request.deviceId);
    std::strcpy(query.commandId, request.commandId);
    const auto disk = io.disk;
    const auto ram = encode(store.state());
    const auto calls = io.calls.size();
    QueriedResult result;
    CHECK(queryMotionResult(store, query, result));
    CHECK(result.status == ResultQueryStatus::Known && result.accepted == accepted);
    CHECK(!std::strcmp(result.reason, reason) && result.outcome == outcome);
    CHECK(result.requestDigestHex == digestHex(request));
    CHECK(io.calls.size() == calls && io.disk == disk && encode(store.state()) == ram);
    v4::Message wire;
    QueriedResult decoded;
    CHECK(encodeQueriedResult(result, wire) && decodeQueriedResult(wire, decoded));
    CHECK(decoded.status == result.status && decoded.outcome == result.outcome);
    ++lookups;
    return result;
}
v4::StopRequest stopRequest(const Fixture& fixture, v4::Source source = v4::Source::LocalTouch,
                            uint64_t sequence = 0, bool idle = false) {
    v4::StopRequest stop;
    stop.source = source;
    stop.sequence = sequence;
    stop.scope = idle ? v4::StopScope::Idle : v4::StopScope::Product;
    std::strcpy(stop.commandId, "stop-original");
    stop.commandIdLength = uint8_t(std::strlen(stop.commandId));
    if (!idle) {
        const char* id = fixture.store.state().slot.executionId;
        CHECK(std::strlen(id) == 32);
        for (unsigned i = 0; i < 16; ++i) {
            unsigned byte = 0;
            CHECK(std::sscanf(id + 2 * i, "%2x", &byte) == 1);
            stop.executionId[i] = uint8_t(byte);
        }
    }
    return stop;
}
bool validStop(const v4::StopRequest& request) {
    uint8_t bytes[155];
    return v4::encodeStop(request, bytes, sizeof(bytes)) != 0;
}

// ReadOnlyLink's callbacks have no context argument; this single-owner route
// delegates directly to the real runtime/store, never a fake decision handler.
Fixture* uartRuntime = nullptr;
unsigned uartCommands = 0, uartStops = 0, uartQueries = 0, uartContexts = 0;
uint16_t uartCommandTtl = 0;
bool routeCommand(const CommandMessage& command, uint32_t now, CommandResult& result) {
    CHECK(uartRuntime);
    ++uartCommands;
    ++deliveries;
    uartCommandTtl = command.remainingTtlMs;
    return uartRuntime->runtime.command(command, now, result);
}
bool routeStop(const v4::StopRequest& request, uint32_t now) {
    CHECK(uartRuntime);
    ++uartStops;
    return uartRuntime->runtime.stop(request, now);
}
bool routeReady(CommandResult& result) {
    CHECK(uartRuntime);
    return uartRuntime->runtime.resultReady(result);
}
bool routeQuery(const ResultQuery& query, QueriedResult& result) {
    CHECK(uartRuntime);
    ++uartQueries;
    ++lookups;
    const auto calls = io.calls.size();
    const auto disk = io.disk;
    const bool answered = queryMotionResult(uartRuntime->store, query, result);
    CHECK(io.calls.size() == calls && io.disk == disk);
    return answered;
}
bool routeContext(const ProductContext& context, uint32_t now, ContextResult& result) {
    CHECK(uartRuntime);
    ++uartContexts;
    return uartRuntime->runtime.context(context, now, result);
}
struct PeerWire : v4::ByteSink {
    static constexpr size_t capacity = 23;
    std::array<uint8_t, capacity> bytes{};
    size_t used = 0, highWater = 0;
    unsigned calls = 0, shortWrites = 0, zeroWrites = 0;
    bool drop = false;
    bool forcedZero = false;
    size_t writeLimit = 7;
    v4::Parser observer;
    v4::Assembler assembled;
    std::vector<v4::Frame> frames;
    std::vector<v4::Message> messages;
    bool idle() const override { return used == 0; }
    size_t available() const override { return capacity - used; }
    size_t write(const uint8_t* input, size_t length) override {
        CHECK(length && length <= available());
        ++calls;
        if (forcedZero || calls % 13 == 0) { ++zeroWrites; return 0; }
        const size_t written = length > writeLimit ? writeLimit : length;
        if (written < length) ++shortWrites;
        std::memcpy(bytes.data() + used, input, written);
        used += written;
        if (used > highWater) highWater = used;
        CHECK(used <= capacity);
        return written;
    }
    void deliver(ReadOnlyLink& peer, uint32_t now) {
        for (size_t i = 0; i < used; ++i) {
            v4::Frame frame;
            if (observer.push(bytes[i], now, frame)) {
                frames.push_back(frame);
                v4::Message message;
                if (assembled.accept(frame, now, message) == v4::AssemblyResult::Complete)
                    messages.push_back(message);
            }
            if (!drop) peer.receive(bytes[i], now);
        }
        used = 0;
    }
    unsigned count(v4::Kind kind) const {
        unsigned total = 0;
        for (const auto& frame : frames) if (frame.kind == kind) ++total;
        return total;
    }
};
struct LinkFixture {
    Fixture device;
    ReadOnlyLink brain, motionLink;
    PeerWire toMotion, toBrain;
    uint32_t now = 1000;
    bool publishStatus = false;
    LinkFixture() {
        CHECK(!uartRuntime);
        uartRuntime = &device;
        uartCommands = uartStops = uartQueries = 0;
        uartContexts = 0;
        uartCommandTtl = 0;
        auto brainPair = pairing();
        brainPair.role = v4::Role::Brain;
        std::swap(brainPair.localPhysicalId, brainPair.peerPhysicalId);
        CHECK(brain.begin(brainPair, 11) && motionLink.begin(pairing(), 22));
        CHECK(motionLink.setCommandHandler(routeCommand) && motionLink.setStopHandler(routeStop));
        CHECK(motionLink.setCommandReadyHandler(routeReady));
        CHECK(motionLink.setResultQueryHandler(routeQuery));
        CHECK(motionLink.setContextHandler(routeContext));
        CHECK(!brain.setContextHandler(routeContext));
        CHECK(!brain.setCommandHandler(routeCommand) && !brain.setStopHandler(routeStop));
    }
    ~LinkFixture() { uartRuntime = nullptr; }
    void step() {
        device.hardware.clock = now;
        brain.poll(now, toMotion);
        Status status;
        status.snapshot = device.product.displaySnapshot();
        status.sampleUptimeMs = now;
        status.motionBusy = device.runtime.active() || device.flow.busy() || device.product.active() ||
            device.hardware.workbenchBusy();
        status.stationary = device.hardware.stationary();
        device.runtime.project(status, true);
        if (status.executionOwner == ExecutionOwner::Workbench)
            status.stationary = device.hardware.workbenchStationary();
        motionLink.poll(now, toBrain, publishStatus ? &status : nullptr);
        toMotion.deliver(motionLink, now);
        toBrain.deliver(brain, now);
        device.tick(now);
        ++now;
    }
    void run(unsigned steps) { while (steps--) step(); }
    void handshake() {
        run(1000);
        CHECK(brain.connected(now) && motionLink.connected(now));
        CHECK(toMotion.count(v4::Kind::Hello) && toBrain.count(v4::Kind::HelloAck));
        CHECK(toMotion.count(v4::Kind::Heartbeat) && toBrain.count(v4::Kind::Heartbeat));
        CHECK(brain.takeFailure() == v4::LinkFailure::None && motionLink.takeFailure() == v4::LinkFailure::None);
        CHECK(!uartCommands && !uartStops && !uartQueries && io.calls.empty());
    }
    v4::Message command(const ProductRequest& request, uint32_t id) {
        CommandMessage command;
        command.request = request;
        command.remainingTtlMs = 5000;
        v4::Message message;
        CHECK(encodeCommand(command, message));
        message.senderBoot = 11;
        message.receiverBoot = 22;
        message.messageId = id;
        return message;
    }
    void send(const v4::Frame& frame) {
        // Keep manual injection for malformed/replayed/fragment ordering cases;
        // normal sender scenarios below use Brain's actual public API.
        CHECK(toMotion.idle());
        std::array<uint8_t, v4::kMaxFrame> encoded{};
        const size_t length = v4::encode(frame, encoded.data(), encoded.size());
        CHECK(length);
        size_t offset = 0;
        unsigned pumps = 0;
        while (offset < length && pumps++ < 256) {
            device.hardware.clock = now;
            const size_t remaining = length - offset;
            const size_t count = remaining < toMotion.available() ? remaining : toMotion.available();
            offset += toMotion.write(encoded.data() + offset, count);
            toMotion.deliver(motionLink, now);
            ++now;
        }
        CHECK(offset == length && toMotion.idle());
    }
    void send(const v4::Message& message, size_t offset = 0) {
        while (offset < message.length) {
            v4::Frame frame;
            CHECK(v4::fragment(message, offset, frame));
            send(frame);
            offset += frame.length;
        }
    }
    void accepted(const ProductRequest& request) {
        run(200);
        unsigned responses = 0;
        for (const auto& message : toBrain.messages) {
            if (message.kind != v4::Kind::CommandResult) continue;
            CommandResult result;
            CHECK(decodeCommandResult(message, result));
            if (result.sequence != request.sequence) continue;
            CHECK(message.senderBoot == 22 && message.receiverBoot == 11);
            CHECK(result.source == request.source && !std::strcmp(result.commandId, request.commandId));
            decision(result, true, "accepted");
            ++responses;
        }
        CHECK(responses == 1 && uartCommands == 1 && uartCommandTtl > 0 && uartCommandTtl < 5000);
    }
    void sendBrain(const ProductRequest& request, uint16_t ttl = 5000) {
        CommandMessage command;
        command.request = request;
        command.remainingTtlMs = ttl;
        CHECK(brain.requestCommand(command, now));
        CHECK(brain.commandSendState() == CommandSendState::Pending);
    }
    void contextBrain(const ProductContext& context, ContextStatus expected) {
        CHECK(brain.requestContext(context, now));
        CHECK(brain.contextSendState() == ContextSendState::Pending);
        for (unsigned i = 0; i < 1000 && brain.contextSendState() == ContextSendState::Pending; ++i) step();
        CHECK(brain.contextSendState() == ContextSendState::Complete);
        CHECK(brain.contextResponse().status == expected && matchesContextResult(brain.contextResponse(), context));
    }
    void awaitBrain(const ProductRequest& request, bool accepted, const char* reason) {
        for (unsigned i = 0; i < 800 && brain.commandSendState() == CommandSendState::Pending; ++i) step();
        CHECK(brain.commandSendState() == CommandSendState::Complete);
        const auto& result = brain.commandResponse();
        CHECK(result.source == request.source && result.sequence == request.sequence);
        CHECK(!std::strcmp(result.commandId, request.commandId));
        decision(result, accepted, reason);
    }
    v4::StopRequest observedTarget(v4::Source source, uint64_t sequence = 0) {
        publishStatus = true;
        const char* expected = device.runtime.workbenchExecutionId()[0] ?
            device.runtime.workbenchExecutionId() : device.store.state().slot.executionId;
        for (unsigned i = 0; i < 700; ++i) {
            if (brain.freshStatus(now) && !std::strcmp(brain.peerStatus().activeExecutionId,
                                                     expected)) break;
            step();
        }
        CHECK(brain.freshStatus(now));
        const auto& status = brain.peerStatus();
        CHECK(std::strlen(status.activeExecutionId) == 32 && status.motionBusy && !status.stationary);
        v4::StopRequest stop;
        struct ObservedLink {
            const ReadOnlyLink& core;
            bool connected(uint32_t nowMs) const { return core.connected(nowMs); }
            const Status* lastTelemetry() const { return &core.peerStatus(); }
            uint32_t lastTelemetryReceivedAtMs() const { return core.peerStatusReceivedAtMs(); }
        } observed{brain};
        CHECK(babytech::brain::bindStopTarget(observed, now, stop));
        stop.source = source;
        stop.sequence = sequence;
        if (source == v4::Source::CloudCommand) {
            std::strcpy(stop.commandId, "cloud-stop-from-brain");
            stop.commandIdLength = uint8_t(std::strlen(stop.commandId));
        }
        return stop;
    }
    void queryOriginal(const ProductRequest& request, MotionOutcome outcome,
                       bool accepted = true, const char* reason = "accepted") {
        ResultQuery query;
        query.source = request.source;
        query.sequence = request.sequence;
        std::strcpy(query.commandId, request.commandId);
        std::strcpy(query.deviceId, request.deviceId);
        const auto calls = io.calls.size();
        const auto disk = io.disk;
        const auto starts = device.executor.starts;
        CHECK(brain.requestResult(query, now));
        for (unsigned i = 0; i < 800 && brain.resultLookupState() == ResultLookupState::Pending; ++i) step();
        CHECK(brain.resultLookupState() == ResultLookupState::Complete && uartQueries == 1);
        const auto& original = brain.resultQueryResponse();
        CHECK(sameResultQuery(original.query, query) && original.status == ResultQueryStatus::Known);
        CHECK(original.accepted == accepted && !std::strcmp(original.reason, reason) && original.outcome == outcome);
        CHECK(original.requestDigestHex == digestHex(request));
        CHECK(io.calls.size() == calls && io.disk == disk && device.executor.starts == starts);
        CHECK(toMotion.count(v4::Kind::ResultQuery) && toBrain.count(v4::Kind::Result));
        CHECK(toMotion.highWater <= PeerWire::capacity && toBrain.highWater <= PeerWire::capacity);
        CHECK(toMotion.shortWrites && toBrain.shortWrites && toMotion.zeroWrites && toBrain.zeroWrites);
        CHECK(brain.healthy() && motionLink.healthy());
        ++uartExchanges;
        uartShortWrites += toMotion.shortWrites + toBrain.shortWrites;
        uartZeroWrites += toMotion.zeroWrites + toBrain.zeroWrites;
        uartFrames += unsigned(toMotion.frames.size() + toBrain.frames.size());
    }
};

void acceptance() {
    for (auto command : {ProductCommand::Prepare, ProductCommand::Initialize, ProductCommand::Clean})
        for (auto source : {v4::Source::LocalTouch, v4::Source::CloudCommand}) {
            if (command == ProductCommand::Initialize && source == v4::Source::CloudCommand) continue;
            scenario("durable acceptance before action " + std::to_string(int(command)) + "/" + std::to_string(int(source)), [=] {
                Fixture f(command != ProductCommand::Initialize);
                const auto r = request(20, command, source);
                bool durableBeforeAction = false;
                const auto action = [&] {
                    const auto disk = decodedDisk();
                    CHECK(disk.slot.kind == MotionSlotKind::Intent && sameProductRequest(disk.slot.request, r));
                    CHECK(sameMotionState(f.store.state(), disk) && fake::count(Op::Commit) == 1);
                    CHECK(fake_product_crypto::calls > 0);
                    durableBeforeAction = true;
                };
                f.executor.beforeStart = action;
                f.executor.beforeStop = action;
                io.before = [&](const fake::Call&) {
                    CHECK(!f.product.active() && !f.product.cleaning() && !f.executor.starts && !f.executor.stops);
                };
                decision(f.deliver(r), true, "accepted");
                io.before = {};
                CHECK(f.runtime.active() && f.store.state().slot.kind == MotionSlotKind::Intent);
                if (command != ProductCommand::Clean) f.tick(1001);
                CHECK(durableBeforeAction);
                lookup(f.store, r, true, "accepted");
                if (command == ProductCommand::Prepare) {
                    const auto& frozen = f.store.state().slot;
                    CHECK(f.product.activeRun().eventId == frozen.eventId);
                    CHECK(f.product.activeRun().commandId == r.commandId && f.product.activeRun().babyId == r.babyId);
                    CHECK(f.product.activeRun().recipe.waterMl == r.waterMl);
                    CHECK(f.product.activeRun().targetPowderG == frozen.targetPowderG);
                    CHECK(f.product.targetTemp() == r.temperatureC);
                }
            });
        }
}

ContextResult deliverContext(Fixture& fixture, const ProductContext& context) {
    v4::Message wire;
    ProductContext decoded;
    CHECK(encodeContextMessage(context, wire));
    CHECK(decodeContextMessage(wire, pairing().deviceId, decoded));
    ContextResult response;
    CHECK(fixture.runtime.context(decoded, fixture.hardware.clock, response));
    // UART owns reply correlation; this group tests only the production handler.
    response.replyTo = 41;
    CHECK(matchesContextResult(response, context));
    ContextResult result;
    CHECK(encodeContextResult(response, wire) && decodeContextResult(wire, result));
    CHECK(result.status == response.status && matchesContextResult(result, context));
    return result;
}

void temperatureProjection() {
    for (auto command : {ProductCommand::Clean, ProductCommand::Initialize})
        scenario("non-feeding active operation keeps current target " + std::to_string(int(command)), [=] {
            Fixture f(command != ProductCommand::Initialize);
            auto settemp = request(20, ProductCommand::SetTargetTemp, v4::Source::CloudCommand);
            settemp.temperatureC = 46;
            decision(f.deliver(settemp), true, "accepted");
            decision(f.deliver(request(21, command)), true, "accepted");
            CHECK(f.runtime.active() && !f.product.active());
            CHECK(f.product.activeRun().recipe.temperatureC != 46);
            Status status;
            status.snapshot = f.product.displaySnapshot();
            f.runtime.project(status, true);
            CHECK(status.snapshot.temperatureC == 46);
        });
    scenario("v4 idle target does not rewrite context or default v3 snapshot", [] {
        Fixture f;
        const auto originalContext = f.store.state().context;
        auto settemp = request(20, ProductCommand::SetTargetTemp, v4::Source::CloudCommand);
        settemp.temperatureC = 46;
        decision(f.deliver(settemp), true, "accepted");
        CHECK(f.product.targetTemp() == 46 && !f.product.active());
        CHECK(f.product.context().recipe.temperatureC == context().temperatureC);
        CHECK(f.store.state().context.profileVersion == originalContext.profileVersion);
        CHECK(!std::memcmp(f.store.state().context.digest, originalContext.digest, kProductDigestSize));
        Status status;
        status.snapshot = f.product.displaySnapshot();
        CHECK(status.snapshot.temperatureC == context().temperatureC);
        const auto disk = io.disk;
        const auto calls = io.calls.size();
        f.runtime.project(status, true);
        CHECK(status.snapshot.temperatureC == 46);
        f.runtime.project(status, false);
        CHECK(status.snapshot.temperatureC == 46 && !status.snapshot.startEnabled);
        CHECK(io.disk == disk && io.calls.size() == calls && !f.executor.starts && !f.executor.stops);

        // A later local Prepare still uses the cached recipe, not the display target.
        auto prepare = request(21);
        prepare.waterMl = context().waterMl;
        prepare.temperatureC = context().temperatureC;
        decision(f.deliver(prepare), true, "accepted");
        CHECK(f.product.active() && f.product.activeRun().recipe.temperatureC == context().temperatureC);
        CHECK(f.product.targetTemp() == context().temperatureC);
        status.snapshot = f.product.displaySnapshot();
        f.runtime.project(status, true);
        CHECK(status.snapshot.temperatureC == context().temperatureC);
    });
    scenario("v4 active target follows frozen Prepare rather than cached recipe", [] {
        Fixture f;
        const auto prepare = request(20, ProductCommand::Prepare, v4::Source::CloudCommand);
        decision(f.deliver(prepare), true, "accepted");
        CHECK(prepare.temperatureC != context().temperatureC);
        Status status;
        status.snapshot = f.product.displaySnapshot();
        CHECK(status.snapshot.temperatureC == context().temperatureC);
        f.runtime.project(status, true);
        CHECK(status.snapshot.temperatureC == prepare.temperatureC);
        auto newer = context();
        ++newer.profileVersion;
        newer.temperatureC = 47;
        CHECK(deliverContext(f, newer).status == ContextStatus::Busy);
        decision(f.deliver(request(21, ProductCommand::SetTargetTemp, v4::Source::CloudCommand)), false, "busy");
        CHECK(f.product.activeRun().recipe.temperatureC == prepare.temperatureC);
        status.snapshot = f.product.displaySnapshot();
        f.runtime.project(status, true);
        CHECK(status.snapshot.temperatureC == prepare.temperatureC);
        f.finishFeed(1010, true);
        CHECK(deliverContext(f, newer).status == ContextStatus::Stored);
        CHECK(f.product.context().recipe.temperatureC == 47);
        CHECK(f.product.displaySnapshot().temperatureC == 47);
        status.snapshot = f.product.displaySnapshot();
        f.runtime.project(status, true);
        CHECK(!f.product.active() && status.snapshot.temperatureC == prepare.temperatureC);
    });
}

void contexts() {
    scenario("Initialize before context proof uses original mechanical admission", [] {
        Fixture f(false, 0, false, false);
        decision(f.deliver(request(20, ProductCommand::Initialize)), true, "accepted");
        CHECK(f.runtime.active());
    });
    scenario("boot context proof affects only Prepare not initialization", [] {
        Fixture f(true, 0, false, false);
        Status status;
        f.runtime.project(status, true);
        CHECK(!status.snapshot.startEnabled);
        decision(f.deliver(request()), false, "context_required");
        CHECK(deliverContext(f, context()).status == ContextStatus::Unchanged);
        f.runtime.project(status, true);
        CHECK(status.snapshot.startEnabled);
        decision(f.deliver(request(21, ProductCommand::Initialize)), true, "accepted");
    });
    scenario("same persisted context confirms without Flash or motion", [] {
        Fixture f;
        const auto sets = fake::count(Op::Set), commits = fake::count(Op::Commit);
        const auto disk = io.disk;
        CHECK(deliverContext(f, context()).status == ContextStatus::Unchanged);
        CHECK(fake::count(Op::Set) == sets && fake::count(Op::Commit) == commits && io.disk == disk);
        CHECK(!f.executor.starts && !f.executor.stops);
        Status status;
        f.runtime.project(status, true);
        CHECK(status.snapshot.startEnabled);
    });
    scenario("new context projects full names and float32 without movement", [] {
        Fixture f;
        auto c = context();
        ++c.profileVersion;
        std::strcpy(c.babyId, "baby-new");
        std::strcpy(c.babyName, "A complete new name longer than the display label allows");
        std::strcpy(c.formulaBrand, "A complete brand longer than the display label allows");
        c.waterMl = 210; c.temperatureC = 46; c.powderGPer100Ml = 13.123456f;
        CHECK(deliverContext(f, c).status == ContextStatus::Stored);
        CHECK(fake::count(Op::Commit) == 1);
        CHECK(f.product.context().babyName == c.babyName && f.product.context().formulaBrand == c.formulaBrand);
        CHECK(f.product.context().recipe.powderGPer100Ml == c.powderGPer100Ml);
        CHECK(!f.executor.starts && !f.executor.stops);
        auto r = request();
        r.profileVersion = c.profileVersion;
        std::strcpy(r.babyId, c.babyId);
        r.powderGPer100Ml = c.powderGPer100Ml;
        decision(f.deliver(r), true, "accepted");
    });
    for (bool clear : {false, true})
        scenario("new configuration while running preserves frozen run " + std::to_string(clear), [=] {
            Fixture f;
            const auto r = request();
            decision(f.deliver(r), true, "accepted");
            f.tick(1001);
            auto c = context();
            ++c.profileVersion;
            if (clear) {
                c.cleared = true;
                c.babyId[0] = c.babyName[0] = c.formulaBrand[0] = 0;
                c.waterMl = c.temperatureC = 0; c.powderGPer100Ml = 0;
            } else std::strcpy(c.babyId, "baby-new");
            const auto disk = io.disk;
            const auto calls = io.calls.size();
            CHECK(deliverContext(f, c).status == ContextStatus::Busy);
            CHECK(io.disk == disk && io.calls.size() == calls);
            CHECK(f.product.activeRun().babyId == r.babyId && f.product.activeRun().profileVersion == r.profileVersion);
            CHECK(f.product.activeRun().recipe.waterMl == r.waterMl);
            CHECK(f.product.context().profileVersion == context().profileVersion);
            CHECK(!f.executor.stops);
            const auto target = stopRequest(f);
            CHECK(f.runtime.stop(target, 1002));
            f.executor.confirm();
            f.tick(1003);
            CHECK(!f.runtime.active() && f.store.state().pendingResultCount == 1);
            CHECK(deliverContext(f, c).status == ContextStatus::Stored);
            CHECK(f.product.context().profileVersion == c.profileVersion);
            CHECK(f.product.hasContext() != clear);
            const auto& terminal = f.store.state().pendingResults[0];
            CHECK(terminal.request.profileVersion == r.profileVersion);
            CHECK(!std::strcmp(terminal.request.babyId, r.babyId));
        });
    for (bool clear : {false, true})
        scenario("observed newer context blocks old Prepare but not Initialize " + std::to_string(clear), [=] {
            Fixture f;
            auto c = context(); ++c.profileVersion;
            if (clear) {
                c.cleared = true;
                c.babyId[0] = c.babyName[0] = c.formulaBrand[0] = 0;
                c.waterMl = c.temperatureC = 0; c.powderGPer100Ml = 0;
            }
            f.hardware.owner = "debug_busy";
            const auto calls = io.calls.size();
            CHECK(deliverContext(f, c).status == ContextStatus::Busy);
            CHECK(io.calls.size() == calls);
            f.hardware.owner = nullptr;
            decision(f.deliver(request()), false, "context_required");
            CHECK(!f.executor.starts);
            decision(f.deliver(request(21, ProductCommand::Initialize)), true, "accepted");
        });
    scenario("pending newer configuration does not block Clean", [] {
        Fixture f;
        auto c = context(); ++c.profileVersion;
        f.hardware.owner = "debug_busy";
        CHECK(deliverContext(f, c).status == ContextStatus::Busy);
        f.hardware.owner = nullptr;
        decision(f.deliver(request(20, ProductCommand::Clean)), true, "accepted");
        CHECK(f.executor.stops == 1);
        f.executor.confirm();
        f.tick(1001);
        CHECK(f.product.cleaning() && !f.runtime.active());
        CHECK(deliverContext(f, c).status == ContextStatus::Stored);
        CHECK(f.product.cleaning() && f.executor.stops == 1);
    });
    scenario("Complete display hold does not block stopped context update", [] {
        Fixture f;
        decision(f.deliver(request()), true, "accepted");
        f.finishFeed(1010, true);
        CHECK(f.flow.stage() == DisplayStage::Complete && f.flow.busy());
        CHECK(!f.runtime.active() && f.hardware.stationary());
        auto c = context(); ++c.profileVersion;
        CHECK(deliverContext(f, c).status == ContextStatus::Stored);
        CHECK(f.store.state().pendingResultCount == 1);
    });
    scenario("stable cleaning allows clear without another motor Stop", [] {
        Fixture f;
        decision(f.deliver(request(20, ProductCommand::Clean)), true, "accepted");
        f.executor.confirm(); f.tick(1001);
        auto c = context(); ++c.profileVersion; c.cleared = true;
        c.babyId[0] = c.babyName[0] = c.formulaBrand[0] = 0;
        c.waterMl = c.temperatureC = 0; c.powderGPer100Ml = 0;
        CHECK(deliverContext(f, c).status == ContextStatus::Stored);
        CHECK(!f.product.hasContext() && f.product.cleaning() && f.executor.stops == 1);
    });
    scenario("stopped fault does not prevent storing next context", [] {
        Fixture f;
        decision(f.deliver(request(20, ProductCommand::Clean)), true, "accepted");
        f.tick(4001); f.executor.confirm(); f.tick(4002);
        CHECK(f.flow.stage() == DisplayStage::Error && !f.runtime.active());
        auto c = context(); ++c.profileVersion;
        CHECK(deliverContext(f, c).status == ContextStatus::Stored);
        CHECK(f.flow.stage() == DisplayStage::Error);
    });
    scenario("same persisted context can be confirmed under unrelated maintenance", [] {
        Fixture f;
        f.hardware.owner = "maintenance_active";
        const auto commits = fake::count(Op::Commit);
        CHECK(deliverContext(f, context()).status == ContextStatus::Unchanged);
        CHECK(fake::count(Op::Commit) == commits && !f.executor.starts && !f.executor.stops);
    });
    for (unsigned mutation = 0; mutation < 4; ++mutation)
        scenario("same-version wrong RAM cache never receives stored proof " + std::to_string(mutation), [=] {
            Fixture f;
            motion::ProductSession product(f.flow);
            motion::FeedingContext cache = f.product.context();
            if (mutation == 0) cache.babyName = "Wrong name";
            else if (mutation == 1) cache.formulaBrand = "Wrong brand";
            else if (mutation == 2) ++cache.recipe.waterMl;
            if (mutation == 3) CHECK(product.clearContext(cache.profileVersion));
            else CHECK(product.applyContext(cache));
            motion::MotionProductRuntime runtime(f.store, product, f.flow, f.hardware);
            ContextResult response;
            const auto commits = fake::count(Op::Commit);
            CHECK(runtime.context(context(), 1000, response) && response.status == ContextStatus::Conflict);
            CHECK(fake::count(Op::Commit) == commits);
            Status status;
            runtime.project(status, true);
            CHECK(!status.snapshot.startEnabled);
        });
    for (unsigned mutation = 0; mutation < 3; ++mutation)
        scenario("old or same-version conflicting configuration leaves cache intact " + std::to_string(mutation), [=] {
            Fixture f;
            auto c = context();
            if (mutation == 0) --c.profileVersion;
            else if (mutation == 1) std::strcpy(c.babyName, "Different same-version name");
            else c.powderGPer100Ml = 13.500001f;
            const auto calls = io.calls.size();
            const auto disk = io.disk;
            CHECK(deliverContext(f, c).status == ContextStatus::Conflict);
            CHECK(io.calls.size() == calls && io.disk == disk);
            Status status;
            f.runtime.project(status, true);
            CHECK(status.snapshot.startEnabled);
        });
    scenario("higher observed version cannot be displaced by lower retry", [] {
        Fixture f;
        auto c = context(); c.profileVersion += 2;
        f.hardware.owner = "debug_busy";
        CHECK(deliverContext(f, c).status == ContextStatus::Busy);
        auto older = c; --older.profileVersion;
        const auto calls = io.calls.size();
        CHECK(deliverContext(f, older).status == ContextStatus::Conflict);
        auto different = c; ++different.waterMl;
        CHECK(deliverContext(f, different).status == ContextStatus::Conflict);
        CHECK(io.calls.size() == calls);
        f.hardware.owner = nullptr;
        CHECK(deliverContext(f, c).status == ContextStatus::Stored);
        CHECK(f.product.context().profileVersion == c.profileVersion);
    });
    for (bool clear : {false, true})
        scenario("reboot rehydrates persisted barrier without rewriting NVS " + std::to_string(clear), [=] {
            auto c = context();
            if (clear) {
                c.cleared = true;
                c.babyId[0] = c.babyName[0] = c.formulaBrand[0] = 0;
                c.waterMl = c.temperatureC = 0; c.powderGPer100Ml = 0;
            }
            MotionStateStore installed;
            CHECK(installed.installInitial(pairing(), &c) == MotionWrite::Stored);
            fake::reboot();
            MotionStateStore loaded;
            CHECK(loaded.load(pairing()) == MotionLoad::Ready);
            Executor executor;
            motion::DemoFlowController flow(executor);
            motion::ProductSession product(flow);
            Hardware hardware(executor);
            motion::MotionProductRuntime runtime(loaded, product, flow, hardware);
            const auto sets = fake::count(Op::Set), commits = fake::count(Op::Commit);
            ContextResult reply;
            CHECK(runtime.context(c, 1000, reply) && reply.status == ContextStatus::Unchanged);
            CHECK(product.context().profileVersion == c.profileVersion && product.hasContext() != clear);
            CHECK(fake::count(Op::Set) == sets && fake::count(Op::Commit) == commits);
            CHECK(!executor.starts && !executor.stops);
        });
    scenario("wrong paired device does not mutate output or observe version", [] {
        Fixture f;
        auto c = context(); ++c.profileVersion;
        std::strcpy(c.deviceId, "foreign-device");
        ContextResult response; response.replyTo = 123;
        const auto calls = io.calls.size();
        CHECK(!f.runtime.context(c, 1000, response));
        CHECK(response.replyTo == 123 && io.calls.size() == calls);
        Status status;
        f.runtime.project(status, true);
        CHECK(status.snapshot.startEnabled);
    });
    std::vector<fake::Call> trace;
    scenario("capture context persistence operations", [&] {
        Fixture f;
        auto c = context(); ++c.profileVersion;
        CHECK(deliverContext(f, c).status == ContextStatus::Stored);
        trace = io.calls;
    });
    for (const auto& call : trace) {
        if (call.op == Op::Close) continue;
        for (bool apply : {false, true}) {
            if (apply && call.op != Op::Set && call.op != Op::Commit) continue;
            scenario("configuration write fault preserves last verified cache " + std::to_string(int(call.op)) + "/" +
                     std::to_string(call.occurrence) + "/" + std::to_string(apply), [=] {
                Fixture f;
                auto c = context(); ++c.profileVersion;
                const auto before = encode(f.store.state());
                fake::fail(call.op, call.occurrence, ESP_FAIL, apply);
                CHECK(deliverContext(f, c).status == ContextStatus::StorageFault);
                CHECK(f.store.faulted() && encode(f.store.state()) == before);
                CHECK(f.product.context().profileVersion == context().profileVersion);
                CHECK(!f.executor.starts && !f.executor.stops);
                Status status;
                f.runtime.project(status, true);
                CHECK(!status.snapshot.startEnabled);
            });
        }
    }
    for (bool clear : {false, true})
        for (bool durable : {false, true})
            scenario("reboot after uncertain configuration commit uses actual disk " + std::to_string(clear) + "/" +
                     std::to_string(durable), [=] {
                Fixture f;
                auto c = context(); ++c.profileVersion;
                if (clear) {
                    c.cleared = true; c.babyId[0] = c.babyName[0] = c.formulaBrand[0] = 0;
                    c.waterMl = c.temperatureC = 0; c.powderGPer100Ml = 0;
                }
                fake::fail(Op::Commit, 1, ESP_FAIL, durable);
                CHECK(deliverContext(f, c).status == ContextStatus::StorageFault);
                fake::verifyFaults(); fake::reboot();
                MotionStateStore loaded;
                CHECK(loaded.load(pairing()) == MotionLoad::Ready);
                CHECK(loaded.state().context.profileVersion == (durable ? c.profileVersion : context().profileVersion));
                motion::ProductSession product(f.flow);
                motion::MotionProductRuntime runtime(loaded, product, f.flow, f.hardware);
                const auto commits = fake::count(Op::Commit);
                ContextResult response;
                const auto recovered = durable ? c : context();
                CHECK(runtime.context(recovered, 1000, response) && response.status == ContextStatus::Unchanged);
                CHECK(product.context().profileVersion == recovered.profileVersion && product.hasContext() != recovered.cleared);
                CHECK(fake::count(Op::Commit) == commits);
                if (durable) {
                    CHECK(runtime.context(context(), 1000, response) && response.status == ContextStatus::Conflict);
                }
            });
}

void contextUart() {
    scenario("actual Brain context sender confirms Motion durable version", [] {
        LinkFixture link;
        link.handshake();
        auto c = context(); ++c.profileVersion;
        std::strcpy(c.babyName, "A full label preserved through bounded fragmented UART");
        link.contextBrain(c, ContextStatus::Stored);
        CHECK(uartContexts == 1 && link.device.product.context().babyName == c.babyName);
        CHECK(fake::count(Op::Commit) == 1 && !link.device.executor.starts && !link.device.executor.stops);
        CHECK(link.toMotion.count(v4::Kind::Context) && link.toBrain.count(v4::Kind::ContextResult));
        const auto sets = fake::count(Op::Set), commits = fake::count(Op::Commit);
        link.contextBrain(c, ContextStatus::Unchanged);
        CHECK(uartContexts == 2 && fake::count(Op::Set) == sets && fake::count(Op::Commit) == commits);
        auto r = request(); r.profileVersion = c.profileVersion;
        link.sendBrain(r);
        link.awaitBrain(r, true, "accepted");
    });
    scenario("busy configuration reply is not persistence proof and retry is harmless", [] {
        LinkFixture link;
        link.handshake();
        auto c = context(); ++c.profileVersion;
        link.device.hardware.owner = "debug_busy";
        const auto before = io.disk;
        link.contextBrain(c, ContextStatus::Busy);
        CHECK(io.disk == before && io.calls.empty());
        link.device.hardware.owner = nullptr;
        link.contextBrain(c, ContextStatus::Stored);
        CHECK(uartContexts == 2 && fake::count(Op::Commit) == 1);
    });
    scenario("fragmented clear reaches actual Motion handler", [] {
        LinkFixture link;
        link.handshake();
        auto c = context(); ++c.profileVersion;
        c.cleared = true; c.babyId[0] = c.babyName[0] = c.formulaBrand[0] = 0;
        c.waterMl = c.temperatureC = 0; c.powderGPer100Ml = 0;
        link.contextBrain(c, ContextStatus::Stored);
        CHECK(!link.device.product.hasContext() && link.device.store.state().context.cleared);
        link.device.hardware.clock = link.now;
        decision(link.device.deliver(request(20), 5000, link.now), false, "context_required");
    });
    scenario("LinkAck is never durable configuration proof", [] {
        LinkFixture link;
        link.handshake();
        CHECK(link.brain.requestContext(context(), link.now));
        v4::Frame ack;
        ack.kind = v4::Kind::LinkAck; ack.senderBoot = 22; ack.receiverBoot = 11; ack.messageId = 800;
        const auto id = link.brain.contextResponse().replyTo;
        const int length = std::snprintf(reinterpret_cast<char*>(ack.payload), sizeof(ack.payload),
                                         "{\"message_id\":%u}", id);
        CHECK(length > 0); ack.total = ack.length = uint16_t(length);
        link.brain.receiveFrame(ack, link.now);
        CHECK(link.brain.contextSendState() == ContextSendState::Pending);
        link.run(400);
        CHECK(link.brain.contextSendState() == ContextSendState::Complete && uartContexts == 1);
    });
    for (unsigned mutation = 0; mutation < 7; ++mutation)
        scenario("unmatched configuration response cannot complete request " + std::to_string(mutation), [=] {
            LinkFixture link;
            link.handshake();
            CHECK(link.brain.requestContext(context(), link.now));
            auto reply = link.brain.contextResponse();
            reply.status = ContextStatus::Stored;
            if (mutation == 0) ++reply.replyTo;
            else if (mutation == 1) ++reply.profileVersion;
            else if (mutation == 2) reply.cleared = true;
            else if (mutation == 3) std::strcpy(reply.deviceId, "wrong-device");
            else if (mutation == 4) reply.digest[0] ^= 1;
            v4::Message wire;
            CHECK(encodeContextResult(reply, wire));
            wire.senderBoot = mutation == 5 ? 23 : 22;
            wire.receiverBoot = mutation == 6 ? 12 : 11; wire.messageId = 900;
            size_t offset = 0;
            while (offset < wire.length) {
                v4::Frame frame;
                CHECK(v4::fragment(wire, offset, frame));
                link.brain.receiveFrame(frame, link.now);
                offset += frame.length;
            }
            CHECK(link.brain.contextSendState() == ContextSendState::Pending);
            link.run(400);
            CHECK(link.brain.contextSendState() == ContextSendState::Complete && uartContexts == 1);
        });
    scenario("lost context application reply remains unknown despite live link", [] {
        LinkFixture link;
        link.handshake();
        auto c = context(); ++c.profileVersion;
        CHECK(link.brain.requestContext(c, link.now));
        for (unsigned i = 0; i < 500 && !uartContexts; ++i) link.step();
        CHECK(uartContexts == 1 && link.device.store.state().context.profileVersion == c.profileVersion);
        link.toBrain.drop = true;
        link.run(1100);
        CHECK(link.brain.contextSendState() == ContextSendState::TimedOut);
        link.toBrain.drop = false;
        link.run(20);
        const auto commits = fake::count(Op::Commit);
        link.contextBrain(c, ContextStatus::Unchanged);
        CHECK(fake::count(Op::Commit) == commits && !link.device.executor.starts);
    });
    scenario("context proof expires after link loss and cannot authorize stale cache", [] {
        LinkFixture link;
        link.handshake();
        link.contextBrain(context(), ContextStatus::Unchanged);
        link.toBrain.drop = true;
        link.run(2500);
        CHECK(!link.brain.connected(link.now) && link.brain.contextSendState() == ContextSendState::Unavailable);
        CHECK(!link.brain.requestContext(context(), link.now));
    });
    scenario("new peer boot invalidates completed context proof", [] {
        LinkFixture link;
        link.handshake();
        link.contextBrain(context(), ContextStatus::Unchanged);
        CHECK(link.motionLink.begin(pairing(), 23));
        CHECK(link.motionLink.setContextHandler(routeContext));
        link.run(1000);
        CHECK(link.brain.contextSendState() != ContextSendState::Complete);
        CHECK(link.brain.connected(link.now) && link.motionLink.connected(link.now));
        link.contextBrain(context(), ContextStatus::Unchanged);
    });
    scenario("Stop bypasses pending configuration and cancels only its transport", [] {
        LinkFixture link;
        link.handshake();
        auto c = context(); ++c.profileVersion;
        CHECK(link.brain.requestContext(c, link.now));
        v4::StopRequest stop;
        stop.source = v4::Source::LocalTouch; stop.scope = v4::StopScope::Idle;
        CHECK(link.brain.requestStop(stop, link.now));
        CHECK(link.brain.contextSendState() == ContextSendState::Cancelled);
        link.run(300);
        CHECK(link.brain.stopSendState() == StopSendState::Received);
        CHECK(!uartContexts && !link.device.executor.starts && !fake::count(Op::Commit));
        link.contextBrain(c, ContextStatus::Stored);
    });
    for (unsigned phase = 0; phase < 3; ++phase)
        scenario("Stop cancels context partial/first-fragment/applied without stale proof " + std::to_string(phase), [=] {
            LinkFixture link;
            link.handshake();
            auto c = context(); ++c.profileVersion;
            std::memset(c.babyName, 'b', sizeof(c.babyName) - 1);
            std::memset(c.formulaBrand, 'f', sizeof(c.formulaBrand) - 1);
            CHECK(link.brain.requestContext(c, link.now));
            if (phase == 0) {
                link.toMotion.writeLimit = 1;
                link.run(10);
                CHECK(!link.toMotion.count(v4::Kind::Context) && !uartContexts);
            } else if (phase == 1) {
                for (unsigned i = 0; i < 800 && !link.toMotion.count(v4::Kind::Context); ++i) link.step();
                CHECK(link.toMotion.count(v4::Kind::Context) && !uartContexts);
            } else {
                link.toBrain.drop = true;
                for (unsigned i = 0; i < 800 && !uartContexts; ++i) link.step();
                CHECK(uartContexts == 1 && link.device.store.state().context.profileVersion == c.profileVersion);
            }
            CHECK(link.brain.contextSendState() == ContextSendState::Pending);
            v4::StopRequest stop;
            stop.source = v4::Source::LocalTouch; stop.scope = v4::StopScope::Idle;
            CHECK(link.brain.requestStop(stop, link.now));
            CHECK(link.brain.contextSendState() == ContextSendState::Cancelled);
            link.toMotion.writeLimit = 7; link.toBrain.drop = false;
            const auto stoppedAt = link.now;
            for (unsigned i = 0; i < 700 && link.brain.stopSendState() == StopSendState::Pending; ++i) link.step();
            CHECK(link.brain.stopSendState() == StopSendState::Received);
            CHECK(link.brain.contextSendState() == ContextSendState::Cancelled);
            CHECK(!link.device.executor.starts && !link.device.executor.stops);
            CHECK(link.device.store.state().context.profileVersion == (phase == 2 ? c.profileVersion : context().profileVersion));
            link.contextBrain(c, phase == 2 ? ContextStatus::Unchanged : ContextStatus::Stored);
            CHECK(uint32_t(link.now - stoppedAt) < v4::kMessageTimeoutMs);
            CHECK(fake::count(Op::Commit) == 1);
        });
}

void writes() {
    for (auto command : {ProductCommand::Prepare, ProductCommand::Initialize, ProductCommand::Clean}) {
        std::vector<fake::Call> trace;
        scenario("capture actual admission trace " + std::to_string(int(command)), [&] {
            Fixture f(command != ProductCommand::Initialize, 1);
            decision(f.deliver(request(20, command)), true, "accepted");
            trace = io.calls;
        });
        for (const auto& call : trace) {
            if (call.op == Op::Close) continue;
            for (bool early : {false, true}) for (bool apply : {false, true}) {
                if (apply && call.op != Op::Set && call.op != Op::Commit) continue;
                scenario("write failure no action command/op/n/early/apply " + std::to_string(int(command)) + "/" +
                         std::to_string(int(call.op)) + "/" + std::to_string(call.occurrence) + "/" +
                         std::to_string(early) + "/" + std::to_string(apply), [=] {
                    Fixture f(command != ProductCommand::Initialize, 1);
                    const auto before = encode(f.store.state());
                    io.durableOnSet = early;
                    fake::fail(call.op, call.occurrence, ESP_FAIL, apply);
                    decision(f.deliver(request(20, command)), false, "storage_fault");
                    CHECK(f.store.faulted() && encode(f.store.state()) == before);
                    CHECK(!f.runtime.active() && !f.product.ownsMotion() && !f.flow.busy());
                    CHECK(!f.executor.starts && !f.executor.stops && !f.executor.resets);
                    const auto disk = io.disk;
                    const auto calls = io.calls.size();
                    for (unsigned i = 0; i < 5; ++i) f.poll(1100 + i);
                    decision(f.deliver(request(21, command)), false, "storage_fault");
                    CHECK(io.calls.size() == calls && io.disk == disk && encode(f.store.state()) == before);
                });
            }
        }
    }
}

void duplicates() {
    for (auto source : {v4::Source::LocalTouch, v4::Source::CloudCommand})
        scenario("duplicates and digest conflicts never replay " + std::to_string(int(source)), [=] {
            Fixture f;
            const auto r = request(20, ProductCommand::Prepare, source);
            decision(f.deliver(r), true, "accepted");
            f.tick(1001);
            const auto disk = io.disk;
            const auto calls = io.calls.size();
            const auto ids = f.hardware.generated;
            for (unsigned i = 0; i < 3; ++i) decision(f.deliver(r, 1, 0), true, "accepted");
            for (auto mutate : std::vector<std::function<void(ProductRequest&)>>{
                [](ProductRequest& r) { ++r.waterMl; },
                [](ProductRequest& r) { ++r.temperatureC; },
                [](ProductRequest& r) { r.powderGPer100Ml += 0.25f; },
                [](ProductRequest& r) { ++r.profileVersion; },
                [](ProductRequest& r) { std::strcpy(r.babyId, "other-baby"); }}) {
                auto changed = r;
                mutate(changed);
                decision(f.deliver(changed), false, "request_conflict");
            }
            CHECK(io.calls.size() == calls && io.disk == disk && f.hardware.generated == ids);
            CHECK(f.executor.starts == 1 && !f.executor.stops);
            const auto terminalAt = f.finishFeed(1010, true);
            CHECK(!f.runtime.active() && f.store.state().pendingResultCount == 1);
            const auto archived = io.disk;
            const auto archivedCalls = io.calls.size();
            decision(f.deliver(r, 1, 0), true, "accepted");
            CHECK(io.disk == archived && io.calls.size() == archivedCalls && f.executor.starts == 5);
            CHECK(f.store.state().pendingResults[0].uptimeMs == terminalAt);
            lookup(f.store, r, true, "accepted", MotionOutcome::Succeeded);
        });
    scenario("accepted intent duplicate after serialized reboot does not execute", [] {
        const auto r = request();
        {
            Fixture f;
            decision(f.deliver(r), true, "accepted");
        }
        fake::reboot();
        Fixture f(false, 0, true);
        const auto disk = io.disk;
        const auto calls = io.calls.size();
        decision(f.deliver(r), true, "accepted");
        CHECK(!f.runtime.active() && !f.executor.starts && !f.executor.stops && !f.hardware.generated);
        CHECK(f.store.state().slot.kind == MotionSlotKind::Intent && io.disk == disk && io.calls.size() == calls);
    });
}

void rejections() {
    for (unsigned condition = 0; condition < 5; ++condition)
        scenario("final rejection consumes sequence " + std::to_string(condition), [=] {
            Fixture f;
            auto r = request();
            const char* reason = nullptr;
            if (condition == 0) { f.product.resources(true, true, true, 300, 1000); reason = "low_water"; }
            if (condition == 1) { f.hardware.owner = "local_maintenance"; reason = "local_maintenance"; }
            if (condition == 2) { f.hardware.executionValue = false; reason = "execution_id_unavailable"; }
            if (condition == 3) { ++r.profileVersion; reason = "context_required"; }
            if (condition == 4) { r = request(20, ProductCommand::CheckFirmwareUpdate); reason = "cloud_ota_not_supported"; }
            decision(f.deliver(r), false, reason);
            CHECK(f.store.state().localSequence == r.sequence && f.store.state().slot.kind == MotionSlotKind::Empty);
            CHECK(!decodedDisk().localResult.accepted && !f.executor.starts && !f.executor.stops);
            f.product.resources(true, false, true, 300, 1001);
            f.hardware.owner = nullptr;
            f.hardware.executionValue = true;
            const auto disk = io.disk;
            const auto calls = io.calls.size();
            decision(f.deliver(r), false, reason);
            CHECK(io.calls.size() == calls && io.disk == disk && !f.runtime.active());
            lookup(f.store, r, false, reason);
            // A lower sequence cannot gain permission after resources recover.
            decision(f.deliver(request(19)), false, "result_expired");
            CHECK(io.calls.size() == calls && io.disk == disk);
        });
    scenario("new busy rejection preserves older execution and original ACK", [] {
        Fixture f;
        const auto original = request(20);
        decision(f.deliver(original), true, "accepted");
        const auto slot = f.store.state().slot;
        const auto blocked = request(21);
        decision(f.deliver(blocked), false, "busy");
        CHECK(f.store.state().localSequence == 21 && sameProductRequest(f.store.state().slot.request, slot.request));
        decision(f.deliver(original), true, "accepted");
        lookup(f.store, original, true, "accepted");
        lookup(f.store, blocked, false, "busy");
        CHECK(f.hardware.generated == 1 && !f.executor.starts);
    });
}

void deferredRejections() {
    scenario("completed deferred reply never replaces later conflict or storage_fault", [] {
        Fixture f;
        decision(f.deliver(request()), true, "accepted");
        f.tick(1001);
        const auto rejected = request(21);
        auto held = f.deliver(rejected, 5000, 1000, false);
        f.executor.confirm();
        f.poll(1200);
        CHECK(f.store.state().localSequence == 21 && !f.store.state().localResult.accepted);
        auto changed = rejected;
        ++changed.waterMl;
        decision(f.deliver(changed), false, "request_conflict");
        CHECK(f.runtime.resultReady(held));
        decision(held, false, "busy");
        decision(f.deliver(changed), false, "request_conflict");
        fake::fail(Op::OpenRO, fake::count(Op::OpenRO) + 1);
        CHECK(f.store.load(pairing()) == MotionLoad::IoError);
        decision(f.deliver(rejected), false, "storage_fault");
        CHECK(f.executor.starts == 1 && !f.executor.stops);
    });
    scenario("failed deferred reply replacement is one-shot and cannot shadow a fresh fault", [] {
        Fixture f;
        decision(f.deliver(request()), true, "accepted");
        f.tick(1001);
        const auto rejected = request(21);
        auto held = f.deliver(rejected, 5000, 1000, false);
        auto staleCopy = held;
        fake::fail(Op::OpenRO, fake::count(Op::OpenRO) + 1);
        CHECK(f.store.load(pairing()) == MotionLoad::IoError);
        f.poll(1002);
        decision(f.deliver(rejected), false, "storage_fault");
        CHECK(f.runtime.resultReady(held));
        decision(held, false, "storage_fault");
        CHECK(f.runtime.resultReady(staleCopy));
        decision(staleCopy, false, "busy"); // Consumed held-reply metadata cannot be applied twice.
        CHECK(f.store.state().localSequence == 20 && f.store.state().slot.kind == MotionSlotKind::Intent);
    });
    scenario("moving busy rejection is frozen, bounded and unknown until persisted", [] {
        Fixture f;
        const auto original = request();
        decision(f.deliver(original), true, "accepted");
        f.tick(1001);
        auto rejected = request(21);
        const auto frozen = rejected;
        const auto calls = io.calls.size();
        const auto disk = io.disk;
        auto pending = f.deliver(rejected, 5000, 1000, false);
        decision(pending, false, "busy");
        CHECK(io.calls.size() == calls && io.disk == disk && f.store.state().localSequence == 20);
        // The caller's buffer changes after command(); the durable rejection
        // must keep the full original request/digest, not just its identity.
        ++rejected.waterMl;
        std::strcpy(rejected.babyId, "changed-after-delivery");
        CommandMessage extra;
        extra.request = request(22);
        extra.remainingTtlMs = 5000;
        CommandResult ignored;
        CHECK(!f.runtime.command(extra, 1000, ignored));
        for (unsigned i = 0; i < 5; ++i) {
            f.poll(1100 + i);
            CHECK(!f.runtime.resultReady(pending));
        }
        CHECK(io.calls.size() == calls && io.disk == disk && f.executor.starts == 1 && !f.executor.stops);
        decision(f.deliver(original, 1, 0), true, "accepted");
        CHECK(io.calls.size() == calls && io.disk == disk);
        f.hardware.clock = 7000; // Expired TTL cannot turn a frozen rejection into acceptance.
        CHECK(f.runtime.stopOwned(7000));
        f.executor.confirm();
        f.tick(7001);
        CHECK(f.runtime.resultReady(pending));
        decision(pending, false, "busy");
        CHECK(!f.runtime.active() && f.store.state().localSequence == 21);
        CHECK(sameProductRequest(f.store.state().localResult.request, frozen));
        CHECK(f.executor.starts == 1 && f.executor.stops == 1);
        lookup(f.store, frozen, false, "busy");
        lookup(f.store, original, true, "accepted", MotionOutcome::Failed);
        decision(f.deliver(frozen, 1, 0), false, "busy");
    });
    scenario("power loss before deferred persistence leaves unknown, not a final busy ACK", [] {
        const auto rejected = request(21);
        {
            Fixture f;
            decision(f.deliver(request()), true, "accepted");
            f.tick(1001);
            auto pending = f.deliver(rejected, 5000, 1000, false);
            CHECK(!f.runtime.resultReady(pending) && f.store.state().localSequence == 20);
        }
        fake::reboot();
        MotionStateStore restored;
        CHECK(restored.load(pairing()) == MotionLoad::Ready);
        ResultQuery query;
        query.source = rejected.source;
        query.sequence = rejected.sequence;
        std::strcpy(query.commandId, rejected.commandId);
        std::strcpy(query.deviceId, rejected.deviceId);
        QueriedResult unknown;
        const auto disk = io.disk;
        const auto calls = io.calls.size();
        CHECK(queryMotionResult(restored, query, unknown));
        CHECK(unknown.status == ResultQueryStatus::Unknown && !unknown.accepted && !std::strcmp(unknown.reason, "unknown"));
        CHECK(restored.state().slot.kind == MotionSlotKind::Intent && restored.state().localSequence == 20);
        CHECK(io.calls.size() == calls && io.disk == disk && !fake::count(Op::Set) && !fake::count(Op::Commit));
    });
    for (Op point : {Op::Set, Op::Commit, Op::OpenRO})
        scenario("deferred persistence fault releases same-identity storage_fault reply " + std::to_string(int(point)), [=] {
            Fixture f;
            decision(f.deliver(request()), true, "accepted");
            f.tick(1001);
            const auto rejected = request(21);
            auto pending = f.deliver(rejected, 5000, 1000, false);
            const auto before = encode(f.store.state());
            fake::fail(point, fake::count(point) + 1);
            f.executor.confirm();
            f.poll(1200);
            CHECK(f.store.faulted() && f.runtime.resultReady(pending));
            decision(pending, false, "storage_fault");
            CHECK(pending.sequence == rejected.sequence && !std::strcmp(pending.commandId, rejected.commandId));
            CHECK(encode(f.store.state()) == before && f.store.state().localSequence == 20);
            CHECK(f.executor.starts == 1 && !f.executor.stops);
            const auto calls = io.calls.size();
            for (unsigned i = 0; i < 5; ++i) f.poll(1300 + i);
            CHECK(io.calls.size() == calls && encode(f.store.state()) == before);
        });
    scenario("external Store read fault resolves pending reply without discarding accepted intent", [] {
        Fixture f;
        decision(f.deliver(request()), true, "accepted");
        f.tick(1001);
        auto pending = f.deliver(request(21), 5000, 1000, false);
        const auto before = encode(f.store.state());
        fake::fail(Op::OpenRO, fake::count(Op::OpenRO) + 1);
        CHECK(f.store.load(pairing()) == MotionLoad::IoError);
        CHECK(!f.runtime.resultReady(pending));
        const auto calls = io.calls.size();
        f.poll(1002);
        CHECK(f.runtime.resultReady(pending));
        decision(pending, false, "storage_fault");
        CHECK(encode(f.store.state()) == before && io.calls.size() == calls);
    });
}

void ttl() {
    for (auto command : {ProductCommand::SetTargetTemp, ProductCommand::ResetError})
        for (Op point : {Op::Set, Op::Commit, Op::OpenRO})
            scenario("metadata post-Flash TTL preserves ACK but applies no expired side effect " +
                     std::to_string(int(command)) + "/" + std::to_string(int(point)), [=] {
                Fixture f;
                const int target = f.product.targetTemp();
                bool expired = false;
                io.before = [&](const fake::Call& call) {
                    if (!expired && call.op == point && (point != Op::OpenRO || fake::count(Op::Commit))) {
                        expired = true;
                        f.hardware.clock = 1100;
                    }
                };
                const auto r = request(20, command);
                const char* reason = command == ProductCommand::ResetError ? "already_clear" : "accepted";
                decision(f.deliver(r, 100), true, reason);
                io.before = {};
                CHECK(expired && f.store.ready() && f.store.state().localSequence == 20);
                CHECK(f.product.targetTemp() == target && !f.runtime.active() && !f.runtime.ownsMotion());
                CHECK(!f.hardware.generated && !f.executor.starts && !f.executor.stops && !f.executor.resets);
                lookup(f.store, r, true, reason);
                const auto calls = io.calls.size();
                decision(f.deliver(r, 1, 0), true, reason);
                CHECK(f.product.targetTemp() == target && io.calls.size() == calls);
            });
    for (auto command : {ProductCommand::Prepare, ProductCommand::Initialize, ProductCommand::Clean})
        for (unsigned phase = 0; phase < 3; ++phase)
            scenario("TTL before persistence command/phase " + std::to_string(int(command)) + "/" + std::to_string(phase), [=] {
                Fixture f(command != ProductCommand::Initialize);
                const auto before = encode(f.store.state());
                if (phase == 1) f.hardware.clock = 1100;
                if (phase == 2) f.hardware.beforeId = [&] { f.hardware.clock = 1100; };
                decision(f.deliver(request(20, command), phase == 0 ? 0 : 100), false, "request_expired");
                CHECK(io.calls.empty() && encode(f.store.state()) == before && !f.runtime.active());
                CHECK(!f.executor.starts && !f.executor.stops);
            });
    for (auto command : {ProductCommand::Prepare, ProductCommand::Initialize, ProductCommand::Clean})
        for (Op point : {Op::Set, Op::Commit, Op::OpenRO})
            scenario("TTL expires during Flash command/op " + std::to_string(int(command)) + "/" + std::to_string(int(point)), [=] {
                Fixture f(command != ProductCommand::Initialize);
                bool expired = false;
                io.before = [&](const fake::Call& call) {
                    if (!expired && call.op == point && (point != Op::OpenRO || fake::count(Op::Commit))) {
                        expired = true;
                        f.hardware.clock = 1100;
                    }
                };
                const auto r = request(20, command);
                decision(f.deliver(r, 100), true, "accepted");
                io.before = {};
                CHECK(expired && f.store.ready() && !f.executor.starts && !f.executor.stops && !f.flow.busy());
                CHECK(f.runtime.active() && f.store.state().slot.kind == MotionSlotKind::Intent);
                f.poll(1200);
                CHECK(!f.runtime.active() && f.store.state().slot.kind == MotionSlotKind::Empty);
                CHECK(!f.runtime.ownsMotion());
                if (command == ProductCommand::Prepare) {
                    CHECK(f.store.state().pendingResultCount == 1);
                    const auto& terminal = f.store.state().pendingResults[0];
                    CHECK(!terminal.completed && !std::strcmp(terminal.reason, "request_expired"));
                    CHECK(terminal.uptimeMs == 1100);
                    lookup(f.store, r, true, "accepted", MotionOutcome::Failed);
                } else {
                    CHECK(!f.store.state().pendingResultCount && f.store.state().localResult.outcome == MotionOutcome::Failed);
                    lookup(f.store, r, true, "accepted", MotionOutcome::Failed);
                }
                decision(f.deliver(r, 1, 0), true, "accepted");
                CHECK(!f.executor.starts && !f.executor.stops);
            });
    scenario("TTL monotonic subtraction handles uint32 wrap", [] {
        Fixture f;
        const uint32_t received = UINT32_MAX - 40;
        f.hardware.clock = uint32_t(received + 60);
        decision(f.deliver(request(), 60, received), false, "request_expired");
        CHECK(io.calls.empty());
        f.hardware.clock = uint32_t(received + 59);
        decision(f.deliver(request(), 60, received), true, "accepted");
        CHECK(f.runtime.active());
    });
}

void terminals() {
    for (bool success : {true, false})
        scenario("prepare freezes terminal across moving/stale polls " + std::to_string(success), [=] {
            Fixture f;
            const auto r = request();
            decision(f.deliver(r), true, "accepted");
            uint32_t terminalAt;
            if (success) terminalAt = f.finishFeed();
            else {
                f.tick(1010);
                f.executor.state = motion::DemoExecution::Failed;
                f.tick(1011);
                terminalAt = 1011;
                CHECK(f.executor.stops == 1);
            }
            CHECK(f.store.state().slot.kind == MotionSlotKind::Intent && f.runtime.active());
            const auto terminal = f.store.state().slot;
            CHECK(!terminal.uptimeMs && !terminal.completed);
            const auto frozen = encode(f.store.state());
            const auto writes = fake::count(Op::Set);
            f.poll(terminalAt + 10);
            f.executor.sample.fresh = true;
            f.executor.sample.stationary = false;
            f.poll(terminalAt + 20);
            f.executor.sample.fresh = false;
            f.executor.sample.stationary = true;
            f.poll(terminalAt + 30);
            CHECK(encode(f.store.state()) == frozen && fake::count(Op::Set) == writes);
            f.executor.confirm();
            f.poll(terminalAt + 100);
            CHECK(!f.runtime.active() && f.store.state().slot.kind == MotionSlotKind::Empty);
            CHECK(f.store.state().pendingResultCount == 1 && fake::count(Op::Set) == writes + 2);
            const auto& archived = f.store.state().pendingResults[0];
            CHECK(archived.uptimeMs == terminalAt && archived.completed == success);
            CHECK(!std::strcmp(archived.eventId, terminal.eventId));
            CHECK(!std::strcmp(archived.reason, success ? "" : "execution_failed"));
            CHECK(!std::memcmp(archived.digest, terminal.digest, sizeof(terminal.digest)));
            lookup(f.store, r, true, "accepted", success ? MotionOutcome::Succeeded : MotionOutcome::Failed);
            const auto disk = io.disk;
            const auto calls = io.calls.size();
            for (unsigned i = 0; i < 5; ++i) f.poll(terminalAt + 200 + i);
            CHECK(io.disk == disk && io.calls.size() == calls);
            fake::reboot();
            MotionStateStore restored;
            CHECK(restored.load(pairing()) == MotionLoad::Ready && sameMotionState(restored.state(), f.store.state()));
            lookup(restored, r, true, "accepted", success ? MotionOutcome::Succeeded : MotionOutcome::Failed);
        });
    for (Op point : {Op::Set, Op::Commit, Op::OpenRO})
        scenario("stationary evidence lost inside terminal Flash " + std::to_string(int(point)), [=] {
            Fixture f;
            decision(f.deliver(request()), true, "accepted");
            bool hit = false;
            io.before = [&](const fake::Call& call) {
                if (!hit && call.op == point && (point == Op::OpenRO ? fake::count(Op::Commit) == 2 : call.occurrence == 2)) {
                    hit = true;
                    f.executor.sample.fresh = false;
                }
            };
            const auto terminalAt = f.finishFeed(1010, true);
            io.before = {};
            CHECK(hit && f.store.ready() && f.runtime.active());
            CHECK(f.store.state().slot.kind == MotionSlotKind::Terminal && !f.store.state().pendingResultCount);
            CHECK(f.store.state().slot.uptimeMs == terminalAt && fake::count(Op::Set) == 2);
            f.poll(terminalAt + 10);
            f.poll(terminalAt + 20);
            CHECK(fake::count(Op::Set) == 2 && f.runtime.active());
            f.executor.confirm();
            f.poll(terminalAt + 100);
            CHECK(!f.runtime.active() && f.store.state().pendingResultCount == 1);
            CHECK(f.store.state().pendingResults[0].uptimeMs == terminalAt);
            CHECK(fake::count(Op::Set) == 3);
        });
    scenario("durable start failure drains old Session terminal before next prepare", [] {
        Fixture f;
        bool invalidated = false;
        io.before = [&](const fake::Call& call) {
            if (!invalidated && call.op == Op::Close && fake::count(Op::Commit) == 1) {
                invalidated = true;
                f.executor.healthyValue = false;
            }
        };
        const auto original = request();
        decision(f.deliver(original), true, "accepted");
        io.before = {};
        CHECK(invalidated && !f.product.active() && f.runtime.active() && f.executor.stops == 1);
        CHECK(f.flow.stage() == DisplayStage::Error && !f.executor.starts);
        f.executor.healthyValue = true;
        f.executor.confirm();
        f.tick(1002);
        CHECK(!f.runtime.active() && f.store.state().pendingResultCount == 1);
        CHECK(!std::strcmp(f.store.state().pendingResults[0].reason, "not_ready"));
        CHECK(f.store.state().pendingResults[0].uptimeMs == 1000);
        motion::ProductTerminal stale;
        CHECK(!f.product.takeTerminal(stale));
        lookup(f.store, original, true, "accepted", MotionOutcome::Failed);
        decision(f.deliver(request(21, ProductCommand::Initialize)), true, "accepted");
        f.tick(1003); // reset feedback enters initialization
        f.tick(1004); // initialization executor starts
        f.executor.state = motion::DemoExecution::Done;
        f.executor.confirm();
        f.tick(1005);
        CHECK(!f.runtime.active() && f.product.canStart());
        decision(f.deliver(request(22)), true, "accepted");
        f.poll(1006);
        CHECK(f.runtime.active() && f.product.active() && f.store.state().slot.kind == MotionSlotKind::Intent);
        CHECK(f.store.state().pendingResultCount == 1);
    });
}

void queues() {
    scenario("four historical results block prepare only, not initialize/clean/temp/reset", [] {
        Fixture f(true, 4);
        const auto original = f.store.state();
        decision(f.deliver(request(20)), false, "result_queue_full");
        CHECK(!f.runtime.active() && !f.hardware.generated);
        decision(f.deliver(request(21, ProductCommand::SetTargetTemp)), true, "accepted");
        CHECK(f.product.targetTemp() == 50 && !f.runtime.active());
        decision(f.deliver(request(22, ProductCommand::ResetError)), true, "already_clear");
        const auto initialize = request(23, ProductCommand::Initialize);
        decision(f.deliver(initialize), true, "accepted");
        f.poll(1001);
        CHECK(!f.runtime.active() && f.store.state().localResult.outcome == MotionOutcome::Succeeded);
        lookup(f.store, initialize, true, "accepted", MotionOutcome::Succeeded);
        const auto clean = request(24, ProductCommand::Clean);
        decision(f.deliver(clean), true, "accepted");
        CHECK(f.runtime.active());
        f.executor.confirm();
        f.tick(1002);
        CHECK(f.product.cleaning() && !f.runtime.active() && f.store.state().localResult.outcome == MotionOutcome::Succeeded);
        lookup(f.store, clean, true, "accepted", MotionOutcome::Succeeded);
        auto expected = original;
        expected.localSequence = f.store.state().localSequence;
        expected.localResult = f.store.state().localResult;
        CHECK(sameMotionState(expected, f.store.state()) && sameMotionState(f.store.state(), decodedDisk()));
    });
    for (auto command : {ProductCommand::Initialize, ProductCommand::Clean})
        for (unsigned outcome = 0; outcome < 3; ++outcome)
            scenario("initialize/clean terminal success/failure/interrupted " + std::to_string(int(command)) + "/" +
                     std::to_string(outcome), [=] {
                Fixture f(command != ProductCommand::Initialize, 4);
                const auto r = request(20, command);
                decision(f.deliver(r), true, "accepted");
                if (command == ProductCommand::Initialize) f.tick(1001);
                if (outcome == 2) {
                    const auto stop = stopRequest(f);
                    CHECK(f.runtime.stop(stop, 1002));
                } else if (outcome == 1) {
                    if (command == ProductCommand::Initialize) {
                        f.executor.state = motion::DemoExecution::Failed;
                        f.tick(1002);
                    } else f.tick(4001); // genuine stop-feedback timeout in production flow
                    CHECK(f.flow.stage() == DisplayStage::Error);
                } else if (command == ProductCommand::Initialize) f.executor.state = motion::DemoExecution::Done;
                const auto calls = io.calls.size();
                f.poll(4100);
                CHECK(f.runtime.active() && io.calls.size() == calls);
                f.executor.confirm();
                f.tick(4101);
                CHECK(!f.runtime.active() && f.store.state().slot.kind == MotionSlotKind::Empty);
                CHECK(f.store.state().pendingResultCount == 4);
                lookup(f.store, r, true, "accepted", outcome == 0 ? MotionOutcome::Succeeded :
                       outcome == 1 ? MotionOutcome::Failed : MotionOutcome::Interrupted);
            });
}

// Compile the real HTTP admission helper, not a copied ownership decision.
// This fixture has no boot recovery in progress; HTTP parsing, MotorControl
// Results and the success-only handler handoffs are checked statically.
struct ManualAdmission {
    motion::MotionProductRuntime& productRuntime;
    motion::DemoFlowController& demo;
    motion::ProductSession& product;
    Executor& demoExecutor;
    unsigned error = 0;
    explicit ManualAdmission(Fixture& f)
        : productRuntime(f.runtime), demo(f.flow), product(f.product), demoExecutor(f.executor) {}
    bool recoveryMotionPending() const { return false; }
    static const char* F(const char* text) { return text; }
    void sendError(unsigned code, const char* reason) {
        CHECK(!std::strcmp(reason, "demo_busy"));
        error = code;
    }
#include "MotionManualAdmissionHost.inc"
};

void manualOwnership() {
    for (auto source : {v4::Source::LocalTouch, v4::Source::CloudCommand}) {
        const auto suffix = " source=" + std::to_string(int(source));
        scenario("manual admission rejects active product without dropping its owner" + suffix, [=] {
            Fixture f;
            decision(f.deliver(request(20, ProductCommand::Prepare, source)), true, "accepted");
            f.tick(1001);
            ManualAdmission manual(f);
            const auto before = encode(f.store.state());
            const auto disk = io.disk;
            const auto calls = io.calls.size();
            CHECK(!manual.demoManualMutation() && manual.error == 409);
            CHECK(f.runtime.active() && f.runtime.ownsMotion() && f.product.active());
            CHECK(f.runtime.stopOwned(1002) && f.executor.stops == 1 && f.executor.starts == 1);
            CHECK(encode(f.store.state()) == before && io.disk == disk && io.calls.size() == calls);
        });
        for (unsigned stopPath = 0; stopPath < 4; ++stopPath)
            scenario("terminal manual admission retains Stop/D1 until physically settled path=" +
                     std::to_string(stopPath) + suffix, [=] {
                Fixture f;
                const auto original = request(20, ProductCommand::Prepare, source);
                decision(f.deliver(original), true, "accepted");
                const auto terminalAt = f.finishFeed();
                ManualAdmission manual(f);
                CHECK(!manual.demoManualMutation() && manual.error == 409); // Complete display hold.
                f.tick(terminalAt + 3000);
                CHECK(!f.product.active() && !f.flow.busy() && !f.hardware.stationary());
                CHECK(f.runtime.active() && f.runtime.ownsMotion());
                CHECK(f.store.state().slot.kind == MotionSlotKind::Intent);
                const auto before = encode(f.store.state());
                const auto disk = io.disk;
                const auto calls = io.calls.size();
                const auto target = stopRequest(f, source, source == v4::Source::CloudCommand ? 21 : 0);
                CHECK(validStop(target));
                // HTTP may still reject validation/busy/CAN dispatch, or only
                // configure the bench. Admission itself cannot transfer motion.
                for (unsigned i = 0; i < 3; ++i) {
                    CHECK(manual.demoManualMutation());
                    CHECK(f.runtime.ownsMotion() && f.runtime.active());
                    CHECK(f.flow.referenceValid() && !f.executor.stops && f.executor.starts == 5);
                    CHECK(encode(f.store.state()) == before && io.disk == disk && io.calls.size() == calls);
                }
                const auto now = terminalAt + 3010;
                if (stopPath == 0) CHECK(f.runtime.stopOwned(now)); // HTTP Stop's production target.
                else if (stopPath == 1) CHECK(f.runtime.stop(target, now));
                else if (stopPath == 2) f.runtime.linkLost(now); // D1 supervision.
                else {
                    f.executor.stopValue = false;
                    CHECK(!f.runtime.stopOwned(now));
                    CHECK(f.runtime.ownsMotion() && f.executor.stops == 1);
                    f.executor.stopValue = true;
                    CHECK(f.runtime.stopOwned(now + 1));
                }
                const auto stops = stopPath == 3 ? 2u : 1u;
                CHECK(f.executor.stops == stops && f.runtime.ownsMotion());
                CHECK(encode(f.store.state()) == before && io.disk == disk && io.calls.size() == calls);
                CHECK(f.runtime.stopOwned(now + 2));
                f.runtime.linkLost(now + 3);
                f.poll(now + 4);
                CHECK(f.executor.stops == stops && f.runtime.active() && f.runtime.ownsMotion());
                CHECK(io.calls.size() == calls && io.disk == disk);
                f.executor.confirm();
                f.tick(now + 5);
                CHECK(!f.runtime.active() && !f.runtime.ownsMotion() && f.executor.starts == 5);
                CHECK(f.store.state().pendingResultCount == 1 && f.store.state().pendingResults[0].uptimeMs == terminalAt);
                lookup(f.store, original, true, "accepted", MotionOutcome::Succeeded);
            });
        scenario("manual admission leaves native stationary release and dedup intact" + suffix, [=] {
            Fixture f;
            const auto original = request(20, ProductCommand::Prepare, source);
            decision(f.deliver(original), true, "accepted");
            const auto terminalAt = f.finishFeed();
            f.tick(terminalAt + 3000);
            ManualAdmission manual(f);
            CHECK(manual.demoManualMutation() && f.runtime.ownsMotion());
            f.executor.confirm();
            f.tick(terminalAt + 3001);
            CHECK(!f.runtime.active() && !f.runtime.ownsMotion() && !f.executor.stops);
            lookup(f.store, original, true, "accepted", MotionOutcome::Succeeded);
            decision(f.deliver(original), true, "accepted");
            CHECK(!f.runtime.active() && f.executor.starts == 5 && f.hardware.generated == 1);
        });
        for (bool accepted : {false, true})
            scenario("independent Demo acceptance controls explicit retained-terminal handoff accepted=" +
                     std::to_string(accepted) + suffix, [=] {
                Fixture f;
                decision(f.deliver(request(20, ProductCommand::Prepare, source)), true, "accepted");
                const auto terminalAt = f.finishFeed();
                f.tick(terminalAt + 3000);
                const auto target = stopRequest(f, source, source == v4::Source::CloudCommand ? 21 : 0);
                CHECK(validStop(target));
                const auto before = encode(f.store.state());
                const auto disk = io.disk;
                const auto calls = io.calls.size();
                f.executor.availableValue = accepted;
                const bool launched = f.flow.single(0, terminalAt + 3001);
                CHECK(launched == accepted);
                // Mirrors only the external ownership action after a real
                // production Demo acceptance; main's ordering is checked statically.
                if (launched) f.runtime.releaseMotionOwnership();
                if (accepted) {
                    f.tick(terminalAt + 3002);
                    CHECK(!f.runtime.ownsMotion() && f.runtime.active() && f.executor.starts == 6);
                    CHECK(!f.runtime.stopOwned(terminalAt + 3003) && !f.runtime.stop(target, terminalAt + 3003));
                    f.runtime.linkLost(terminalAt + 3004);
                    CHECK(!f.executor.stops && f.flow.busy());
                } else {
                    CHECK(f.runtime.ownsMotion() && f.runtime.stopOwned(terminalAt + 3003));
                    CHECK(f.executor.stops == 1 && f.executor.starts == 5);
                }
                CHECK(encode(f.store.state()) == before && io.disk == disk && io.calls.size() == calls);
            });
    }
}

void offlineAndLink() {
    scenario("independent workbench stage handoff cannot apply metadata or write moving Flash", [] {
        Fixture f;
        CHECK(f.flow.single(0, 1000));
        f.executor.availableValue = false; // Production flow waits to launch the next stage.
        f.executor.sample.stationary = false;
        f.flow.tick(1001);
        CHECK(f.flow.busy() && !f.product.ownsMotion() && !f.runtime.active() && !f.executor.starts);
        const auto calls = io.calls.size();
        const auto disk = io.disk;
        auto held = f.deliver(request(20, ProductCommand::SetTargetTemp), 5000, 1000, false);
        decision(held, false, "busy");
        f.poll(1002);
        CHECK(!f.runtime.resultReady(held) && io.calls.size() == calls && io.disk == disk);
        CHECK(f.product.targetTemp() == 45 && !f.runtime.ownsMotion() && !f.runtime.stopOwned(1003));
        f.flow.stop(1004); // Independent workbench owner handles its own Stop.
        f.executor.availableValue = true;
        f.executor.confirm();
        f.tick(1005);
        CHECK(f.runtime.resultReady(held));
        decision(held, false, "busy");
        CHECK(f.product.targetTemp() == 45 && !f.executor.starts && f.executor.stops == 1);
        lookup(f.store, request(20, ProductCommand::SetTargetTemp), false, "busy");
    });
    scenario("cleaning Session ownership blocks metadata even after execution slot is archived", [] {
        Fixture f;
        decision(f.deliver(request(20, ProductCommand::Clean)), true, "accepted");
        f.executor.confirm();
        f.tick(1001);
        CHECK(f.product.ownsMotion() && !f.flow.busy() && !f.runtime.active());
        decision(f.deliver(request(21, ProductCommand::SetTargetTemp)), false, "busy");
        CHECK(f.product.targetTemp() == 45 && f.executor.stops == 1 && !f.executor.starts);
    });
    scenario("maintenance projection is local and does not persist a global disable", [] {
        Fixture f;
        Status status;
        const auto disk = io.disk;
        f.hardware.owner = "local_maintenance";
        f.runtime.project(status, true);
        CHECK(!status.executionAuthorized && !status.snapshot.startEnabled && io.calls.empty());
        f.hardware.owner = nullptr;
        f.runtime.project(status, true);
        CHECK(status.executionAuthorized && status.snapshot.startEnabled && io.disk == disk && io.calls.empty());
    });
    scenario("D1 after RAM terminal but before stationary still sends supervised Stop", [] {
        Fixture f;
        const auto original = request();
        decision(f.deliver(original), true, "accepted");
        const auto terminalAt = f.finishFeed();
        CHECK(!f.product.active() && f.runtime.active() && f.runtime.ownsMotion());
        CHECK(f.store.state().slot.kind == MotionSlotKind::Intent);
        const auto calls = io.calls.size();
        const auto disk = io.disk;
        f.runtime.linkLost(terminalAt + 10);
        f.runtime.linkLost(terminalAt + 11);
        CHECK(f.executor.stops == 1 && io.calls.size() == calls && io.disk == disk);
        f.poll(terminalAt + 12);
        CHECK(f.runtime.active() && io.calls.size() == calls);
        f.executor.confirm();
        f.tick(terminalAt + 13);
        CHECK(!f.runtime.active() && !f.runtime.ownsMotion());
        CHECK(f.store.state().pendingResults[0].completed && f.store.state().pendingResults[0].uptimeMs == terminalAt);
        lookup(f.store, original, true, "accepted", MotionOutcome::Succeeded);
    });
    scenario("local action needs no network and projection is not admission", [] {
        Fixture f;
        f.product.networkState(false, 999);
        Status status;
        f.runtime.project(status, false);
        CHECK(!status.executionAuthorized && !status.snapshot.startEnabled);
        decision(f.deliver(request()), true, "accepted");
        f.tick(1001);
        f.product.networkState(false, 1002);
        CHECK(f.product.active() && f.executor.starts == 1 && !f.executor.stops);
        const auto slot = f.store.state().slot;
        f.runtime.project(status, false);
        CHECK(!std::strcmp(status.activeExecutionId, slot.executionId) && !status.executionAuthorized);
    });
    for (auto command : {ProductCommand::Prepare, ProductCommand::Initialize, ProductCommand::Clean})
        scenario("D1 link lost supervised Stop once " + std::to_string(int(command)), [=] {
            Fixture f(command != ProductCommand::Initialize);
            const auto r = request(20, command);
            decision(f.deliver(r), true, "accepted");
            if (command != ProductCommand::Clean) f.tick(1001);
            const auto stops = f.executor.stops;
            const auto disk = io.disk;
            const auto calls = io.calls.size();
            f.runtime.linkLost(1002);
            f.runtime.linkLost(1003);
            CHECK(f.executor.stops == stops + 1 && io.calls.size() == calls && io.disk == disk);
            f.poll(1004);
            CHECK(f.runtime.active() && io.calls.size() == calls);
            f.executor.confirm();
            f.tick(1005);
            CHECK(!f.runtime.active());
            CHECK(f.executor.stops == stops + 1);
            if (command == ProductCommand::Prepare) {
                CHECK(f.store.state().pendingResultCount == 1);
                CHECK(!std::strcmp(f.store.state().pendingResults[0].reason, "link_lost"));
                CHECK(!std::strcmp(f.store.state().pendingResults[0].errorCode, "E_LINK_LOST"));
            } else lookup(f.store, r, true, "accepted", MotionOutcome::Interrupted);
            f.runtime.linkLost(1010);
            CHECK(f.executor.stops == stops + 1);
        });
}

void stops() {
    scenario("first CAN Stop enqueue failure permits explicit retry without Flash", [] {
        Fixture f;
        decision(f.deliver(request()), true, "accepted");
        f.tick(1001);
        const auto stop = stopRequest(f);
        const auto calls = io.calls.size();
        const auto disk = io.disk;
        f.executor.stopValue = false;
        CHECK(f.runtime.stop(stop, 1002) && f.executor.stops == 1);
        CHECK(!std::strcmp(f.flow.reason(), "stop_unconfirmed"));
        f.executor.stopValue = true;
        CHECK(f.runtime.stop(stop, 1003) && f.executor.stops == 2);
        CHECK(!std::strcmp(f.flow.reason(), "stop_requested"));
        CHECK(f.runtime.stop(stop, 1004) && f.executor.stops == 2);
        CHECK(io.calls.size() == calls && io.disk == disk);
        f.poll(1005);
        CHECK(f.store.state().slot.kind == MotionSlotKind::Intent && io.calls.size() == calls);
        f.executor.confirm();
        f.tick(1006);
        CHECK(!f.runtime.active() && f.store.state().pendingResultCount == 1);
        CHECK(!std::strcmp(f.store.state().pendingResults[0].reason, "stop_unconfirmed"));
    });
    scenario("accepted Stop timeout does not auto-retry or refresh deadline, explicit retry does", [] {
        Fixture f;
        const auto original = request();
        decision(f.deliver(original), true, "accepted");
        f.tick(1001);
        const auto stop = stopRequest(f);
        CHECK(f.runtime.stop(stop, 1002) && f.executor.stops == 1);
        CHECK(f.runtime.stop(stop, 3002) && f.executor.stops == 1);
        f.tick(4001);
        CHECK(f.flow.busy() && f.executor.stops == 1);
        f.tick(4002);
        CHECK(f.flow.stage() == DisplayStage::Error && !std::strcmp(f.flow.reason(), "stop_unconfirmed"));
        CHECK(!f.product.active() && f.runtime.active() && f.executor.stops == 1);
        CHECK(f.store.state().slot.kind == MotionSlotKind::Intent && fake::count(Op::Set) == 1);
        for (unsigned i = 0; i < 5; ++i) f.poll(4100 + i);
        CHECK(f.executor.stops == 1); // No automatic high-frequency Stop retry.
        const auto calls = io.calls.size();
        CHECK(f.runtime.stop(stop, 4200) && f.executor.stops == 2);
        CHECK(f.runtime.stop(stop, 4201) && f.executor.stops == 2 && io.calls.size() == calls);
        f.executor.confirm();
        f.tick(4202);
        CHECK(!f.runtime.active() && f.store.state().pendingResultCount == 1);
        CHECK(f.store.state().pendingResults[0].uptimeMs == 4002);
        CHECK(!std::strcmp(f.store.state().pendingResults[0].reason, "stop_unconfirmed"));
        lookup(f.store, original, true, "accepted", MotionOutcome::Failed);
    });
    for (auto command : {ProductCommand::Initialize, ProductCommand::Clean})
        scenario("HTTP-owned Stop records Interrupted not Succeeded " + std::to_string(int(command)), [=] {
            Fixture f(command != ProductCommand::Initialize);
            const auto r = request(20, command);
            decision(f.deliver(r), true, "accepted");
            if (command == ProductCommand::Initialize) f.tick(1001);
            const auto stops = f.executor.stops;
            const auto calls = io.calls.size();
            CHECK(f.runtime.stopOwned(1002) && f.executor.stops == stops + 1);
            CHECK(f.runtime.stopOwned(1003) && f.executor.stops == stops + 1 && io.calls.size() == calls);
            f.executor.confirm();
            f.tick(1004);
            CHECK(!f.runtime.active() && !f.runtime.ownsMotion() && !f.store.state().pendingResultCount);
            lookup(f.store, r, true, "accepted", MotionOutcome::Interrupted);
        });
    for (auto source : {v4::Source::LocalTouch, v4::Source::CloudCommand})
        for (bool old : {false, true}) {
            if (source == v4::Source::LocalTouch && old) continue;
            scenario("Stop precedes all NVS and old Cloud sequence cannot deny it " +
                     std::to_string(int(source)) + "/" + std::to_string(old), [=] {
                Fixture f;
                const auto r = request(20, ProductCommand::Prepare, v4::Source::CloudCommand);
                decision(f.deliver(r), true, "accepted");
                f.tick(1001);
                const auto stop = stopRequest(f, source, source == v4::Source::CloudCommand ? (old ? 1 : 21) : 0);
                CHECK(validStop(stop));
                const auto calls = io.calls.size();
                const auto disk = io.disk;
                f.executor.beforeStop = [&] { CHECK(io.calls.size() == calls && io.disk == disk); };
                CHECK(f.runtime.stop(stop, 1002));
                CHECK(f.runtime.stop(stop, 1003));
                f.executor.beforeStop = {};
                CHECK(f.executor.stops == 1 && io.calls.size() == calls && io.disk == disk);
                f.poll(1004);
                CHECK(io.calls.size() == calls && f.runtime.active());
                f.executor.confirm();
                f.tick(1005);
                CHECK(!f.runtime.active() && f.store.state().pendingResultCount == 1);
                CHECK(!std::strcmp(f.store.state().pendingResults[0].reason, "stopped"));
                if (source == v4::Source::CloudCommand && !old) {
                    CHECK(f.store.state().cloudSequence == 21 && f.store.state().cloudResult.kind == MotionResultKind::CloudStop);
                    CHECK(!std::strcmp(f.store.state().cloudResult.stopCommandId, stop.commandId));
                } else CHECK(f.store.state().cloudSequence == 20 && f.store.state().cloudResult.kind == MotionResultKind::Ordinary);
            });
        }
    const std::vector<std::pair<const char*, std::function<void(v4::StopRequest&)>>> bad = {
        {"source", [](v4::StopRequest& r) { r.source = v4::Source(0); }},
        {"local sequence", [](v4::StopRequest& r) { r.sequence = 1; }},
        {"cloud zero", [](v4::StopRequest& r) { r.source = v4::Source::CloudCommand; }},
        {"cloud overflow", [](v4::StopRequest& r) { r.source = v4::Source::CloudCommand; r.sequence = UINT64_MAX; }},
        {"empty ID", [](v4::StopRequest& r) { r.commandIdLength = 0; }},
        {"long ID", [](v4::StopRequest& r) { r.commandIdLength = 129; }},
        {"ID NUL", [](v4::StopRequest& r) { r.commandId[0] = 0; }},
        {"ID UTF8", [](v4::StopRequest& r) { r.commandId[0] = char(0xff); }},
        {"scope", [](v4::StopRequest& r) { r.scope = v4::StopScope(0); }},
        {"zero target", [](v4::StopRequest& r) { std::memset(r.executionId, 0, sizeof(r.executionId)); }},
        {"idle with target", [](v4::StopRequest& r) { r.scope = v4::StopScope::Idle; }}};
    for (const auto& mutation : bad)
        scenario(std::string("invalid Stop must not act: ") + mutation.first, [&] {
            Fixture f;
            decision(f.deliver(request()), true, "accepted");
            f.tick(1001);
            auto stop = stopRequest(f);
            mutation.second(stop);
            CHECK(!validStop(stop));
            const auto disk = io.disk;
            const auto calls = io.calls.size();
            CHECK(!f.runtime.stop(stop, 1002));
            CHECK(!f.executor.stops && f.product.active() && io.calls.size() == calls && io.disk == disk);
        });
    scenario("Stop ID length must agree with its C-string terminator", [] {
        Fixture f;
        decision(f.deliver(request()), true, "accepted");
        f.tick(1001);
        auto stop = stopRequest(f);
        stop.commandIdLength = 4;
        CHECK(validStop(stop)); // binary codec is length-delimited, runtime uses a C-string
        const auto disk = io.disk;
        const auto calls = io.calls.size();
        CHECK(!f.runtime.stop(stop, 1002));
        CHECK(!f.executor.stops && io.calls.size() == calls && io.disk == disk);
    });
    scenario("pending Stop retains highest Cloud sequence and prevents new acceptance", [] {
        Fixture f;
        decision(f.deliver(request()), true, "accepted");
        f.tick(1001);
        const auto before = f.store.state();
        auto first = stopRequest(f, v4::Source::CloudCommand, 5);
        std::strcpy(first.commandId, "stop-5");
        first.commandIdLength = 6;
        auto highest = first;
        highest.sequence = 7;
        std::strcpy(highest.commandId, "stop-7");
        auto lower = first;
        lower.sequence = 6;
        std::strcpy(lower.commandId, "stop-6");
        const auto calls = io.calls.size();
        CHECK(f.runtime.stop(first, 1002) && f.runtime.stop(highest, 1003) && f.runtime.stop(lower, 1004));
        CHECK(f.executor.stops == 1 && io.calls.size() == calls && sameMotionState(before, f.store.state()));
        Status status;
        f.runtime.project(status, true);
        CHECK(!status.executionAuthorized && !status.snapshot.startEnabled);
        const auto temp = request(21, ProductCommand::SetTargetTemp);
        auto pending = f.deliver(temp, 5000, 1000, false);
        decision(pending, false, "busy");
        CHECK(!f.runtime.resultReady(pending));
        CHECK(f.product.targetTemp() == 45 && f.hardware.generated == 1);
        const auto rejected = f.store.state();
        f.poll(1005);
        CHECK(sameMotionState(rejected, f.store.state()));
        // Observe each committed replacement: terminal, archived result, then
        // Cloud Stop. Stop never overwrites the feeding journal before archive.
        std::vector<MotionState> replacements;
        unsigned commits = fake::count(Op::Commit);
        io.before = [&](const fake::Call& call) {
            if (call.op == Op::Close && fake::count(Op::Commit) > commits) {
                commits = fake::count(Op::Commit);
                replacements.push_back(decodedDisk());
            }
        };
        f.executor.confirm();
        f.tick(1006);
        io.before = {};
        CHECK(replacements.size() == 4);
        CHECK(replacements[0].slot.kind == MotionSlotKind::Terminal && replacements[0].cloudSequence == 0);
        CHECK(replacements[1].slot.kind == MotionSlotKind::Empty && replacements[1].pendingResultCount == 1);
        CHECK(replacements[1].cloudSequence == 0 && replacements[2].cloudSequence == 7);
        CHECK(!std::strcmp(replacements[2].cloudResult.stopCommandId, "stop-7"));
        CHECK(replacements[3].localSequence == 21 && !replacements[3].localResult.accepted);
        CHECK(!std::strcmp(replacements[3].localResult.reason, "busy"));
        CHECK(f.runtime.resultReady(pending));
        decision(pending, false, "busy");
        CHECK(!f.runtime.active() && f.executor.stops == 1);
        f.runtime.project(status, true);
        CHECK(status.executionAuthorized);
        decision(f.deliver(temp), false, "busy");
        decision(f.deliver(request(22, ProductCommand::SetTargetTemp)), true, "accepted");
        CHECK(f.product.targetTemp() == 50);
    });
    scenario("wrong execution or workbench target never stops product", [] {
        Fixture f;
        decision(f.deliver(request()), true, "accepted");
        f.tick(1001);
        auto stop = stopRequest(f);
        stop.executionId[0] ^= 1;
        CHECK(validStop(stop));
        const auto disk = io.disk;
        const auto calls = io.calls.size();
        CHECK(!f.runtime.stop(stop, 1002));
        stop = stopRequest(f);
        stop.scope = v4::StopScope::Workbench;
        CHECK(validStop(stop) && !f.runtime.stop(stop, 1003));
        CHECK(!f.executor.stops && f.product.active() && io.calls.size() == calls && io.disk == disk);
    });
    scenario("idle Stop validates zero target and remains read-only for local", [] {
        Fixture f;
        const auto disk = io.disk;
        const auto calls = io.calls.size();
        auto stop = stopRequest(f, v4::Source::LocalTouch, 0, true);
        CHECK(validStop(stop) && f.runtime.stop(stop, 1001));
        stop.executionId[0] = 1;
        CHECK(!validStop(stop) && !f.runtime.stop(stop, 1002));
        CHECK(!f.executor.stops && io.calls.size() == calls && io.disk == disk);
    });
}

v4::StopRequest workbenchStop(const Fixture& f, v4::Source source, uint64_t sequence = 0) {
    auto stop = stopRequest(f, source, sequence, true);
    stop.scope = v4::StopScope::Workbench;
    const char* id = f.runtime.workbenchExecutionId();
    CHECK(std::strlen(id) == 32);
    for (unsigned i = 0; i < 16; ++i) {
        unsigned byte = 0;
        CHECK(std::sscanf(id + 2 * i, "%2x", &byte) == 1);
        stop.executionId[i] = uint8_t(byte);
    }
    CHECK(validStop(stop));
    return stop;
}

struct RecoveryStationaryHost : motion::MotionRecoveryHardware {
    struct Busy {
        bool writer = false, settled = true;
        bool busy() const { return writer; }
        bool active() const { return writer; }
        bool operationBusy() const { return writer; }
        bool affectedAxesStationary() const { return settled; }
    } endpoint, motor, queue;
    bool canStarted = true;
    motion::ProductSession& product;
    motion::DemoFlowController& demo;
    Executor& demoExecutor;
    RecoveryStationaryHost(motion::ProductSession& p, motion::DemoFlowController& d, Executor& e)
        : product(p), demo(d), demoExecutor(e) {}
    void supervisedStop(uint32_t nowMs) override { product.recoverAfterRestart(nowMs); }
#include "MotionRecoveryStationaryHost.inc"
};

void workbenchOwnership() {
    scenario("real Recovery and production stationary adapter retain journal after raw writer Done", [] {
        Fixture f;
        decision(f.deliver(request()), true, "accepted");
        motion::DemoFlowController rebootFlow(f.executor);
        CHECK(rebootFlow.apply(f.flow.config()));
        motion::ProductSession rebootProduct(rebootFlow);
        RecoveryStationaryHost hardware(rebootProduct, rebootFlow, f.executor);
        motion::MotionStateRecovery recovery(f.store, hardware);
        CHECK(recovery.begin(pairing(), 1001) == MotionLoad::Ready);
        CHECK(recovery.motionPending() && recovery.executionPending());
        f.executor.confirm();
        rebootFlow.tick(1002);
        rebootProduct.tick(1002);
        CHECK(!rebootFlow.busy() && !rebootProduct.ownsMotion() && hardware.demoExecutor.stopConfirmed());
        // A config-outside workbench axis remains physically moving although
        // the old Demo axes and software writer have already finished.
        hardware.motor.writer = false;
        hardware.motor.settled = false;
        const auto before = encode(f.store.state());
        const auto calls = io.calls.size();
        const auto disk = io.disk;
        recovery.poll();
        CHECK(recovery.motionPending() && recovery.executionPending());
        CHECK(before == encode(f.store.state()) && calls == io.calls.size() && disk == io.disk);
        Status status;
        recovery.project(status);
        CHECK(status.executionOwner == ExecutionOwner::Product && status.activeExecutionId[0]);
        hardware.motor.settled = true;
        recovery.poll();
        CHECK(!recovery.motionPending() && !recovery.executionPending());
        CHECK(f.store.state().slot.kind == MotionSlotKind::Empty && f.store.state().pendingResultCount == 1);
        recovery.project(status);
        CHECK(status.executionOwner == ExecutionOwner::None && !status.activeExecutionId[0] && status.pendingEventId[0]);
    });
    for (auto source : {v4::Source::LocalTouch, v4::Source::CloudCommand})
        scenario("workbench Stop exact owner, old generation rejected, ACK before physical/NVS source=" +
                 std::to_string(int(source)), [=] {
            Fixture f;
            f.hardware.workbenchWriter = true;
            f.hardware.workbenchSettled = false;
            const auto disk = io.disk;
            const auto calls = io.calls.size();
            CHECK(f.runtime.workbenchAccepted());
            const std::string first = f.runtime.workbenchExecutionId();
            auto old = workbenchStop(f, source, source == v4::Source::CloudCommand ? 40 : 0);
            for (unsigned i = 0; i < 5; ++i) f.runtime.poll(1001 + i);
            CHECK(first == f.runtime.workbenchExecutionId() && f.hardware.generated == 1);
            CHECK(io.calls.size() == calls && io.disk == disk);
            CHECK(f.runtime.workbenchAccepted());
            CHECK(first != f.runtime.workbenchExecutionId());
            CHECK(!f.runtime.stop(old, 1010) && !f.hardware.workbenchStops);
            auto target = workbenchStop(f, source, source == v4::Source::CloudCommand ? 41 : 0);
            const std::string targetId = f.runtime.workbenchExecutionId();
            auto wrongOwner = target;
            wrongOwner.scope = v4::StopScope::Product;
            CHECK(!f.runtime.stop(wrongOwner, 1011));
            Status status;
            f.runtime.project(status, true);
            CHECK(status.executionOwner == ExecutionOwner::Workbench && !status.snapshot.startEnabled);
            CHECK(!std::strcmp(status.activeExecutionId, f.runtime.workbenchExecutionId()));
            CHECK(f.runtime.stop(target, 1012) && f.hardware.workbenchStops == 1);
            CHECK(!f.executor.stops && !f.executor.starts && io.calls.size() == calls && io.disk == disk);
            f.runtime.poll(1013);
            CHECK(f.runtime.workbenchExecutionId()[0] && io.disk == disk);
            f.hardware.workbenchFresh = f.hardware.workbenchSettled = true;
            f.runtime.poll(1014);
            CHECK(!f.runtime.workbenchExecutionId()[0]);
            f.runtime.project(status, true);
            CHECK(status.executionOwner == ExecutionOwner::None && !status.activeExecutionId[0]);
            CHECK(!f.runtime.stop(target, 1015));
            CHECK(f.store.state().slot.kind == MotionSlotKind::Empty && !f.store.state().pendingResultCount);
            if (source == v4::Source::CloudCommand) {
                CHECK(f.store.state().cloudSequence == 41);
                CHECK(f.store.state().cloudResult.kind == MotionResultKind::CloudStop);
                CHECK(targetId == f.store.state().cloudResult.stopExecutionId);
                CHECK(std::strlen(f.store.state().cloudResult.stopExecutionId) == 32);
                CHECK(f.store.state().cloudResult.accepted && decodedDisk().cloudSequence == 41);
            } else CHECK(io.disk == disk && io.calls.size() == calls);
        });
    scenario("workbench natural completion requires writer done and fresh evidence", [] {
        Fixture f;
        f.hardware.workbenchWriter = true;
        CHECK(f.runtime.workbenchAccepted());
        const std::string id = f.runtime.workbenchExecutionId();
        f.runtime.poll(1001);
        CHECK(id == f.runtime.workbenchExecutionId()); // read/config-only queue still has a writer.
        f.hardware.workbenchWriter = false;
        f.hardware.workbenchFresh = false;
        f.runtime.poll(1002);
        CHECK(id == f.runtime.workbenchExecutionId());
        f.hardware.workbenchFresh = true;
        f.hardware.workbenchSettled = false;
        f.runtime.poll(1003);
        CHECK(id == f.runtime.workbenchExecutionId());
        f.hardware.workbenchSettled = true;
        f.runtime.poll(1004);
        CHECK(!f.runtime.workbenchExecutionId()[0] && !f.hardware.workbenchStops);
    });
    scenario("failed workbench ID generation preserves previous target", [] {
        Fixture f;
        CHECK(f.runtime.workbenchAccepted());
        const std::string original = f.runtime.workbenchExecutionId();
        f.hardware.executionValue = false;
        CHECK(!f.runtime.workbenchAccepted() && original == f.runtime.workbenchExecutionId());
    });
    scenario("accepted product generation revokes old workbench Stop target", [] {
        Fixture f;
        CHECK(f.runtime.workbenchAccepted());
        const auto old = workbenchStop(f, v4::Source::LocalTouch);
        decision(f.deliver(request(20, ProductCommand::Initialize)), true, "accepted");
        CHECK(f.runtime.ownsMotion() && !f.runtime.workbenchExecutionId()[0]);
        CHECK(!f.runtime.stop(old, 1001) && !f.hardware.workbenchStops);
        Status status;
        f.runtime.project(status, true);
        CHECK(status.executionOwner == ExecutionOwner::Product && status.activeExecutionId[0]);
    });
    scenario("unfinished workbench writer cannot overlap a product acceptance", [] {
        Fixture f;
        CHECK(f.runtime.workbenchAccepted());
        f.hardware.workbenchWriter = true;
        decision(f.deliver(request(20, ProductCommand::Initialize)), false, "busy");
        CHECK(!f.runtime.ownsMotion() && !f.executor.starts && f.runtime.workbenchExecutionId()[0]);
    });
    scenario("workbench local Stop remains available with unavailable durable store", [] {
        Fixture f;
        CHECK(f.runtime.workbenchAccepted());
        auto target = workbenchStop(f, v4::Source::LocalTouch);
        fake::fail(Op::OpenRO, fake::count(Op::OpenRO) + 1);
        CHECK(f.store.load(pairing()) == MotionLoad::IoError);
        CHECK(!f.store.ready());
        const auto calls = io.calls.size();
        CHECK(f.runtime.stop(target, 1001) && f.hardware.workbenchStops == 1);
        CHECK(io.calls.size() == calls);
        f.runtime.linkLost(1002);
        CHECK(f.hardware.workbenchStops == 1); // no new workbench network gate.
    });
    scenario("workbench Cloud Stop archives without product Demo stationary eligibility", [] {
        Fixture f(false);
        CHECK(f.runtime.workbenchAccepted());
        f.executor.sample.fresh = false;
        f.hardware.workbenchFresh = f.hardware.workbenchSettled = false;
        auto stop = workbenchStop(f, v4::Source::CloudCommand, 44);
        CHECK(f.runtime.stop(stop, 1001));
        f.hardware.workbenchFresh = f.hardware.workbenchSettled = true;
        CHECK(!f.hardware.stationary());
        f.runtime.poll(1002);
        CHECK(f.store.state().cloudSequence == 44 && decodedDisk().cloudSequence == 44);
        CHECK(f.store.state().slot.kind == MotionSlotKind::Empty && !f.store.state().pendingResultCount);
    });
    scenario("retained unarchived product terminal cannot starve independent workbench Cloud Stop", [] {
        Fixture f;
        decision(f.deliver(request()), true, "accepted");
        const auto terminalAt = f.finishFeed();
        f.tick(terminalAt + 3000);
        CHECK(f.runtime.active() && f.runtime.ownsMotion() && !f.hardware.stationary());
        CHECK(f.store.state().slot.kind == MotionSlotKind::Intent);
        CHECK(f.runtime.workbenchAccepted());
        auto expected = f.store.state();
        auto stop = workbenchStop(f, v4::Source::CloudCommand, 44);
        CHECK(f.runtime.stop(stop, terminalAt + 3001));
        f.hardware.workbenchFresh = f.hardware.workbenchSettled = true;
        CHECK(!f.hardware.stationary());
        f.runtime.poll(terminalAt + 3002);
        CHECK(f.store.state().cloudSequence == 44 && decodedDisk().cloudSequence == 44);
        expected.cloudSequence = 44;
        expected.cloudResult = f.store.state().cloudResult;
        CHECK(sameMotionState(expected, f.store.state()));
        CHECK(f.runtime.active() && !f.runtime.workbenchExecutionId()[0]);
        CHECK(f.store.state().slot.kind == MotionSlotKind::Intent);
    });
    scenario("recovery Product target stays explicit and history alone has no owner", [] {
        Fixture f;
        std::strcpy(f.hardware.recoveryId, executionId(9876).c_str());
        auto stop = stopRequest(f, v4::Source::LocalTouch, 0, true);
        stop.scope = v4::StopScope::Product;
        for (unsigned i = 0; i < 16; ++i) {
            unsigned byte = 0;
            CHECK(std::sscanf(f.hardware.recoveryId + 2 * i, "%2x", &byte) == 1);
            stop.executionId[i] = uint8_t(byte);
        }
        CHECK(f.runtime.stop(stop, 1001) && f.hardware.recoveryStops == 1);
        f.hardware.recoveryId[0] = 0;
        CHECK(!f.runtime.stop(stop, 1002) && f.hardware.recoveryStops == 1);
        Status status;
        f.runtime.project(status, true);
        CHECK(status.executionOwner == ExecutionOwner::None && !status.activeExecutionId[0]);
    });
}

void faults() {
    for (bool failedWrite : {false, true})
        scenario("Clean Error releases mechanical ownership without clearing fault/Ready " +
                 std::to_string(failedWrite), [=] {
            Fixture f;
            const auto r = request(20, ProductCommand::Clean);
            decision(f.deliver(r), true, "accepted");
            const auto oldStop = stopRequest(f);
            f.tick(4001);
            CHECK(f.flow.stage() == DisplayStage::Error && f.runtime.ownsMotion());
            if (failedWrite) fake::fail(Op::Set, fake::count(Op::Set) + 1);
            f.executor.confirm();
            f.tick(4002);
            CHECK(f.flow.stage() == DisplayStage::Error && !f.runtime.ownsMotion());
            CHECK(!f.runtime.stop(oldStop, 4003) && !f.runtime.stopOwned(4003));
            const auto stops = f.executor.stops;
            f.runtime.linkLost(4004);
            CHECK(f.executor.stops == stops && !f.product.cleaning());
            if (failedWrite) {
                CHECK(f.store.faulted() && f.runtime.active() && f.store.state().slot.kind == MotionSlotKind::Intent);
                Status status;
                f.runtime.project(status, true);
                CHECK(!status.activeExecutionId[0]);
            } else {
                CHECK(f.store.ready() && !f.runtime.active());
                lookup(f.store, r, true, "accepted", MotionOutcome::Failed);
            }
        });
    scenario("explicit debug handoff prevents old retained execution from stopping new Demo", [] {
        Fixture f;
        decision(f.deliver(request()), true, "accepted");
        f.tick(1001);
        const auto oldStop = stopRequest(f);
        fake::fail(Op::OpenRO, fake::count(Op::OpenRO) + 1);
        CHECK(f.store.load(pairing()) == MotionLoad::IoError);
        const auto retained = encode(f.store.state());
        bool wasActive = false;
        CHECK(f.product.stop(1002, wasActive) && wasActive);
        f.runtime.releaseMotionOwnership();
        CHECK(!f.runtime.ownsMotion() && !f.runtime.stop(oldStop, 1003));
        f.executor.confirm();
        f.tick(1004);
        CHECK(f.runtime.active() && !f.product.active());
        CHECK(f.flow.single(0, 1005));
        f.flow.tick(1006);
        const auto stops = f.executor.stops;
        CHECK(f.executor.starts == 2 && !f.runtime.stopOwned(1007) && !f.runtime.stop(oldStop, 1007));
        f.runtime.linkLost(1008);
        CHECK(f.executor.stops == stops && encode(f.store.state()) == retained);
    });
    for (bool archive : {false, true}) for (Op point : {Op::Set, Op::Commit})
        scenario("terminal write/archive fault retains intent or frozen terminal " + std::to_string(archive) + "/" +
                 std::to_string(int(point)), [=] {
            Fixture f(true, 2);
            const auto r = request();
            decision(f.deliver(r), true, "accepted");
            const auto terminalAt = f.finishFeed();
            CHECK(f.store.state().slot.kind == MotionSlotKind::Intent && fake::count(Op::Set) == 1);
            const auto occurrence = fake::count(point) + (archive ? 2 : 1);
            Bytes before;
            io.before = [&](const fake::Call& call) {
                if (call.op == point && call.occurrence == occurrence) before = encode(f.store.state());
            };
            fake::fail(point, occurrence);
            f.executor.confirm();
            f.poll(1200);
            io.before = {};
            CHECK(!before.empty() && f.store.faulted() && encode(f.store.state()) == before && f.runtime.active());
            CHECK(f.store.state().slot.kind == (archive ? MotionSlotKind::Terminal : MotionSlotKind::Intent));
            CHECK(f.store.state().slot.uptimeMs == (archive ? terminalAt : 0));
            CHECK(f.store.state().pendingResultCount == 2 && !f.product.eventPending());
            const auto disk = io.disk;
            const auto calls = io.calls.size();
            const auto starts = f.executor.starts;
            for (unsigned i = 0; i < 5; ++i) f.poll(1300 + i);
            decision(f.deliver(request(21)), false, "storage_fault");
            CHECK(io.calls.size() == calls && io.disk == disk && encode(f.store.state()) == before);
            CHECK(f.executor.starts == starts);
            Status status;
            status.eventPending = true;
            std::strcpy(status.pendingEventId, f.store.state().pendingResults[0].eventId);
            f.runtime.project(status, true);
            CHECK(!status.executionAuthorized && !status.snapshot.startEnabled && status.eventPending);
            CHECK(!std::strcmp(status.pendingEventId, f.store.state().pendingResults[0].eventId));
        });
    scenario("Store fault never blocks targeted immediate Stop or removes results", [] {
        Fixture f(true, 2);
        decision(f.deliver(request()), true, "accepted");
        f.tick(1001);
        fake::fail(Op::OpenRO, fake::count(Op::OpenRO) + 1);
        CHECK(f.store.load(pairing()) == MotionLoad::IoError);
        CHECK(f.store.faulted());
        const auto before = encode(f.store.state());
        const auto disk = io.disk;
        const auto calls = io.calls.size();
        const auto stop = stopRequest(f);
        CHECK(f.runtime.stop(stop, 1002) && f.executor.stops == 1);
        f.executor.confirm();
        f.tick(1003);
        CHECK(f.runtime.active() && io.calls.size() == calls && io.disk == disk && encode(f.store.state()) == before);
        CHECK(f.store.state().pendingResultCount == 2);
        f.runtime.linkLost(1004);
        CHECK(f.executor.stops == 1);
        Status status;
        f.runtime.project(status, true);
        CHECK(!status.activeExecutionId[0]);
        CHECK(!f.runtime.stop(stop, 1005));
        CHECK(f.flow.single(0, 1006));
        f.flow.tick(1007);
        CHECK(f.executor.starts == 2);
        CHECK(!f.runtime.stop(stop, 1008) && f.executor.stops == 1);
        f.runtime.linkLost(1009);
        CHECK(f.executor.stops == 1 && encode(f.store.state()) == before);
    });
}

void uartIntegration() {
    for (auto source : {v4::Source::LocalTouch, v4::Source::CloudCommand})
        scenario("real UART workbench STATUS target/Stop routes current ID only source=" +
                 std::to_string(int(source)), [=] {
            ++brainSenderScenarios;
            LinkFixture link;
            link.handshake();
            auto& f = link.device;
            f.hardware.workbenchWriter = true;
            f.hardware.workbenchFresh = f.hardware.workbenchSettled = false;
            CHECK(f.runtime.workbenchAccepted());
            auto old = link.observedTarget(source, source == v4::Source::CloudCommand ? 100 : 0);
            CHECK(old.scope == v4::StopScope::Workbench);
            CHECK(f.runtime.workbenchAccepted());
            CHECK(link.brain.requestStop(old, link.now));
            for (unsigned i = 0; i < 300 && link.brain.stopSendState() == StopSendState::Pending; ++i) link.step();
            CHECK(link.brain.stopSendState() == StopSendState::Rejected && !f.hardware.workbenchStops);
            link.run(1000);
            auto current = link.observedTarget(source, source == v4::Source::CloudCommand ? 101 : 0);
            CHECK(current.scope == v4::StopScope::Workbench);
            CHECK(link.brain.requestStop(current, link.now));
            for (unsigned i = 0; i < 300 && link.brain.stopSendState() == StopSendState::Pending; ++i) link.step();
            CHECK(link.brain.stopSendState() == StopSendState::Received && f.hardware.workbenchStops == 1);
            CHECK(!f.executor.starts && !f.executor.stops && io.calls.empty());
            CHECK(f.runtime.workbenchExecutionId()[0]);
            f.hardware.workbenchFresh = f.hardware.workbenchSettled = true;
            link.run(100);
            CHECK(!f.runtime.workbenchExecutionId()[0]);
            CHECK(f.store.state().slot.kind == MotionSlotKind::Empty && !f.store.state().pendingResultCount);
            if (source == v4::Source::CloudCommand) {
                CHECK(decodedDisk().cloudSequence == 101);
                ResultQuery query;
                query.source = source; query.sequence = 101;
                std::strcpy(query.deviceId, pairing().deviceId);
                std::strcpy(query.commandId, current.commandId);
                QueriedResult result;
                CHECK(queryMotionResult(f.store, query, result));
                CHECK(result.status == ResultQueryStatus::Known && result.accepted && result.outcome == MotionOutcome::None);
            } else CHECK(io.calls.empty());
            ++uartExchanges;
        });
    scenario("real two-role UART command acceptance Stop between fragments archive original query", [] {
        LinkFixture link;
        link.handshake();
        auto& f = link.device;
        const auto original = request(20, ProductCommand::Prepare, v4::Source::CloudCommand);
        bool verifiedBeforeAction = false;
        f.executor.beforeStart = [&] {
            CHECK(sameMotionState(f.store.state(), decodedDisk()));
            CHECK(decodedDisk().slot.kind == MotionSlotKind::Intent && fake::count(Op::Commit) == 1);
            CHECK(sameProductRequest(decodedDisk().slot.request, original));
            verifiedBeforeAction = true;
        };
        link.send(link.command(original, 10000));
        CHECK(f.runtime.active() && !f.executor.starts && fake::count(Op::Commit) == 1);
        link.accepted(original);
        CHECK(verifiedBeforeAction && f.product.active() && f.executor.starts == 1);
        const auto partial = link.command(request(21, ProductCommand::Prepare, v4::Source::CloudCommand), 11000);
        v4::Frame first;
        CHECK(v4::fragment(partial, 0, first) && first.length < partial.length);
        link.send(first);
        CHECK(uartCommands == 1 && f.store.state().cloudSequence == 20);
        auto stop = stopRequest(f);
        constexpr uint32_t stopId = 11001;
        std::snprintf(stop.commandId, sizeof(stop.commandId), "stop-%016llx-%08x", 11ULL, stopId);
        stop.commandIdLength = uint8_t(std::strlen(stop.commandId));
        v4::Frame urgent;
        urgent.kind = v4::Kind::Stop;
        urgent.senderBoot = 11;
        urgent.receiverBoot = 22;
        urgent.messageId = stopId;
        urgent.total = urgent.length = uint16_t(v4::encodeStop(stop, urgent.payload, sizeof(urgent.payload)));
        CHECK(urgent.length);
        const auto calls = io.calls.size();
        const auto disk = io.disk;
        f.executor.beforeStop = [&] { CHECK(io.calls.size() == calls && io.disk == disk); };
        link.send(urgent);
        f.executor.beforeStop = {};
        CHECK(uartStops == 1 && f.executor.stops == 1 && io.calls.size() == calls && io.disk == disk);
        link.send(partial, first.length);
        CHECK(uartCommands == 1 && f.store.state().cloudSequence == 20);
        link.run(10);
        CHECK(f.runtime.active() && io.calls.size() == calls && io.disk == disk);
        // The session observes stopped axes, then board feedback expires before
        // the runtime poll. The frozen terminal must survive later moving polls.
        const auto terminalAt = link.now;
        f.hardware.clock = terminalAt;
        f.executor.confirm();
        f.flow.tick(terminalAt);
        f.product.tick(terminalAt);
        f.executor.sample.fresh = false;
        f.runtime.poll(terminalAt);
        CHECK(f.store.state().slot.kind == MotionSlotKind::Intent && f.runtime.active());
        CHECK(!f.store.state().slot.uptimeMs && !f.store.state().slot.completed);
        const auto writes = fake::count(Op::Set);
        link.run(20);
        CHECK(fake::count(Op::Set) == writes && f.runtime.active());
        CHECK(!f.store.state().slot.uptimeMs);
        f.executor.confirm();
        link.step();
        CHECK(!f.runtime.active() && f.store.state().pendingResultCount == 1);
        CHECK(f.store.state().pendingResults[0].uptimeMs == terminalAt && fake::count(Op::Set) == writes + 2);
        CHECK(!std::strcmp(f.store.state().pendingResults[0].reason, "stopped"));
        CHECK(f.executor.starts == 1 && f.executor.stops == 1);
        link.queryOriginal(original, MotionOutcome::Failed);
    });
    scenario("real UART holds moving rejection until resultReady confirms durable original rejection", [] {
        LinkFixture link;
        link.handshake();
        auto& f = link.device;
        const auto original = request();
        link.send(link.command(original, 10000));
        link.accepted(original);
        CHECK(f.executor.starts == 1 && f.product.active());
        const auto rejected = request(21, ProductCommand::SetTargetTemp);
        const auto calls = io.calls.size();
        const auto disk = io.disk;
        link.send(link.command(rejected, 11000));
        CHECK(uartCommands == 2 && io.calls.size() == calls && io.disk == disk);
        link.run(100);
        for (const auto& message : link.toBrain.messages) {
            if (message.kind != v4::Kind::CommandResult) continue;
            CommandResult result;
            CHECK(decodeCommandResult(message, result) && result.sequence != 21);
        }
        CHECK(f.store.state().localSequence == 20 && f.product.targetTemp() == 45);
        CHECK(io.calls.size() == calls && io.disk == disk);
        auto stop = stopRequest(f);
        constexpr uint32_t stopId = 12000;
        std::snprintf(stop.commandId, sizeof(stop.commandId), "stop-%016llx-%08x", 11ULL, stopId);
        stop.commandIdLength = uint8_t(std::strlen(stop.commandId));
        v4::Frame urgent;
        urgent.kind = v4::Kind::Stop;
        urgent.senderBoot = 11;
        urgent.receiverBoot = 22;
        urgent.messageId = stopId;
        urgent.total = urgent.length = uint16_t(v4::encodeStop(stop, urgent.payload, sizeof(urgent.payload)));
        f.executor.beforeStop = [&] { CHECK(io.calls.size() == calls && io.disk == disk); };
        link.send(urgent);
        f.executor.beforeStop = {};
        CHECK(f.executor.stops == 1 && uartStops == 1 && io.calls.size() == calls);
        f.executor.confirm();
        link.step();
        CHECK(!f.runtime.active() && f.store.state().localSequence == 21 && !f.store.state().localResult.accepted);
        CHECK(!std::strcmp(f.store.state().localResult.reason, "busy"));
        link.run(200);
        unsigned replies = 0;
        for (const auto& message : link.toBrain.messages) {
            if (message.kind != v4::Kind::CommandResult) continue;
            CommandResult result;
            CHECK(decodeCommandResult(message, result));
            if (result.sequence != 21) continue;
            decision(result, false, "busy");
            CHECK(!std::strcmp(result.commandId, rejected.commandId));
            ++replies;
        }
        CHECK(replies == 1 && f.executor.starts == 1 && f.executor.stops == 1);
        CHECK(f.product.targetTemp() == 45 && sameProductRequest(f.store.state().localResult.request, rejected));
        link.queryOriginal(original, MotionOutcome::Failed);
    });
    scenario("real UART accepted prepare completes five production stages and returns original result", [] {
        ++brainSenderScenarios;
        LinkFixture link;
        link.handshake();
        auto& f = link.device;
        const auto original = request();
        f.executor.beforeStart = [&] {
            CHECK(sameMotionState(f.store.state(), decodedDisk()));
            CHECK(decodedDisk().slot.kind == MotionSlotKind::Intent && sameProductRequest(decodedDisk().slot.request, original));
        };
        CHECK(!link.brain.freshStatus(link.now));
        link.sendBrain(original);
        link.awaitBrain(original, true, "accepted");
        link.accepted(original);
        CHECK(f.executor.starts == 1 && f.product.active());
        for (unsigned i = 0; i < 5; ++i) {
            CHECK(f.executor.starts == i + 1);
            f.executor.state = motion::DemoExecution::Done;
            link.step();
            if (i < 4) link.step();
        }
        CHECK(!f.product.active() && f.runtime.active() && f.store.state().slot.kind == MotionSlotKind::Intent);
        const auto terminalAt = link.now - 1;
        link.run(10);
        f.executor.confirm();
        link.step();
        CHECK(!f.runtime.active() && f.store.state().pendingResultCount == 1);
        CHECK(f.store.state().pendingResults[0].completed && f.store.state().pendingResults[0].uptimeMs == terminalAt);
        CHECK(f.executor.starts == 5 && !f.executor.stops);
        link.queryOriginal(original, MotionOutcome::Succeeded);
    });
    for (auto source : {v4::Source::LocalTouch, v4::Source::CloudCommand})
        scenario("real Brain lost command reply then targeted Stop preserves moving intent " +
                 std::to_string(int(source)), [=] {
            ++brainSenderScenarios;
            LinkFixture link;
            link.handshake();
            auto& f = link.device;
            const auto original = request(20, ProductCommand::Prepare, source);
            bool durableBeforeStart = false;
            f.executor.beforeStart = [&] {
                CHECK(sameMotionState(f.store.state(), decodedDisk()));
                CHECK(decodedDisk().slot.kind == MotionSlotKind::Intent);
                CHECK(sameProductRequest(decodedDisk().slot.request, original));
                CHECK(fake::count(Op::Commit) == 1);
                durableBeforeStart = true;
            };
            link.toBrain.drop = true;
            link.sendBrain(original);
            link.run(200);
            CHECK(durableBeforeStart && uartCommands == 1 && f.executor.starts == 1);
            CHECK(link.brain.commandSendState() == CommandSendState::Pending);
            CHECK(uartCommandTtl > 0 && uartCommandTtl <= 4950);
            link.toBrain.drop = false;
            const auto stop = link.observedTarget(source, source == v4::Source::CloudCommand ? 21 : 0);
            CHECK(link.brain.commandSendState() == CommandSendState::Pending);
            const auto retained = encode(f.store.state());
            const auto disk = io.disk;
            const auto calls = io.calls.size();
            f.executor.beforeStop = [&] { CHECK(io.calls.size() == calls && io.disk == disk); };
            CHECK(link.brain.requestStop(stop, link.now));
            CHECK(link.brain.commandSendState() == CommandSendState::Cancelled);
            for (unsigned i = 0; i < 300 && link.brain.stopSendState() == StopSendState::Pending; ++i) link.step();
            f.executor.beforeStop = {};
            CHECK(link.brain.stopSendState() == StopSendState::Received && uartStops == 1 && f.executor.stops == 1);
            CHECK(!f.hardware.stationary() && f.runtime.active() && f.product.active());
            CHECK(encode(f.store.state()) == retained && io.calls.size() == calls && io.disk == disk);
            CHECK(f.store.state().slot.kind == MotionSlotKind::Intent);
            CHECK(link.toMotion.count(v4::Kind::Stop) == 1);
            v4::StopRequest transmitted;
            bool found = false;
            for (const auto& frame : link.toMotion.frames) if (frame.kind == v4::Kind::Stop) {
                CHECK(v4::decodeStop(frame.payload, frame.length, transmitted));
                CHECK(transmitted.source == source && transmitted.sequence == stop.sequence);
                CHECK(!std::memcmp(transmitted.executionId, stop.executionId, 16));
                if (source == v4::Source::CloudCommand) CHECK(!std::strcmp(transmitted.commandId, stop.commandId));
                else {
                    char expected[40];
                    std::snprintf(expected, sizeof(expected), "stop-%016llx-%08x", 11ULL, frame.messageId);
                    CHECK(!std::strcmp(transmitted.commandId, expected));
                }
                found = true;
            }
            CHECK(found);
            link.run(100);
            CHECK(encode(f.store.state()) == retained && io.calls.size() == calls && f.executor.starts == 1);
            f.executor.confirm();
            link.step();
            CHECK(!f.runtime.active() && f.store.state().pendingResultCount == 1);
            CHECK(!std::strcmp(f.store.state().pendingResults[0].reason, "stopped"));
            CHECK(f.executor.starts == 1 && f.executor.stops == 1 && uartCommands == 1);
            link.publishStatus = false;
            link.queryOriginal(original, MotionOutcome::Failed);
        });
    for (auto source : {v4::Source::LocalTouch, v4::Source::CloudCommand})
        scenario("real Brain low-water rejection persists and original query never retries action " +
                 std::to_string(int(source)), [=] {
            ++brainSenderScenarios;
            LinkFixture link;
            link.handshake();
            auto& f = link.device;
            f.product.resources(true, true, true, 300, link.now);
            const auto original = request(20, ProductCommand::Prepare, source);
            link.sendBrain(original);
            link.awaitBrain(original, false, "low_water");
            CHECK(uartCommands == 1 && !f.executor.starts && !f.executor.stops && !f.runtime.active());
            CHECK(fake::count(Op::Commit) == 1 && sameMotionState(f.store.state(), decodedDisk()));
            f.product.resources(true, false, true, 300, link.now);
            link.queryOriginal(original, MotionOutcome::None, false, "low_water");
            CHECK(uartCommands == 1 && !f.executor.starts && !f.hardware.generated);
        });
    scenario("real Brain moving busy rejection stays pending while urgent Stop bypasses Flash", [] {
        ++brainSenderScenarios;
        LinkFixture link;
        link.handshake();
        auto& f = link.device;
        const auto original = request();
        link.sendBrain(original);
        link.awaitBrain(original, true, "accepted");
        CHECK(f.executor.starts == 1);
        const auto stop = link.observedTarget(v4::Source::LocalTouch);
        // The real queued STATUS receipt is control traffic, not a global
        // admission gate; sendBrain must accept while the ordinary slot is free.
        link.publishStatus = false;
        const auto calls = io.calls.size();
        const auto disk = io.disk;
        const auto rejected = request(21, ProductCommand::SetTargetTemp);
        link.sendBrain(rejected);
        link.run(150);
        CHECK(uartCommands == 2 && link.brain.commandSendState() == CommandSendState::Pending);
        CHECK(io.calls.size() == calls && io.disk == disk && f.product.targetTemp() == 45);
        f.executor.beforeStop = [&] { CHECK(io.calls.size() == calls && io.disk == disk); };
        CHECK(link.brain.requestStop(stop, link.now));
        CHECK(link.brain.commandSendState() == CommandSendState::Cancelled);
        for (unsigned i = 0; i < 300 && link.brain.stopSendState() == StopSendState::Pending; ++i) link.step();
        f.executor.beforeStop = {};
        CHECK(link.brain.stopSendState() == StopSendState::Received && f.executor.stops == 1);
        CHECK(io.calls.size() == calls && io.disk == disk && f.runtime.active());
        f.executor.confirm();
        link.run(300);
        CHECK(link.brain.commandSendState() == CommandSendState::Cancelled);
        CHECK(f.store.state().localSequence == 21 && !f.store.state().localResult.accepted);
        CHECK(!std::strcmp(f.store.state().localResult.reason, "busy"));
        CHECK(f.executor.starts == 1 && f.product.targetTemp() == 45);
        link.publishStatus = false;
        link.queryOriginal(original, MotionOutcome::Failed);
    });
    for (bool partial : {false, true})
        scenario("real Brain first-frame UART expiry never reaches Store or starts Flow " +
                 std::to_string(partial), [=] {
            ++brainSenderScenarios;
            LinkFixture link;
            link.handshake();
            auto& f = link.device;
            const auto before = encode(f.store.state());
            const auto disk = io.disk;
            link.toMotion.forcedZero = !partial;
            link.toMotion.writeLimit = partial ? 1 : 7;
            link.sendBrain(request());
            link.run(60);
            CHECK(link.brain.commandSendState() == CommandSendState::TimedOut);
            link.toMotion.forcedZero = false;
            link.toMotion.writeLimit = 7;
            link.run(1500);
            CHECK(!uartCommands && !f.executor.starts && !f.executor.stops && !f.hardware.generated);
            CHECK(io.calls.empty() && io.disk == disk && encode(f.store.state()) == before);
            CHECK(link.brain.healthy() && link.motionLink.healthy());
            // CRC-poisoned residual bytes must not prevent a distinct explicit request.
            const auto next = request(21);
            link.sendBrain(next);
            link.awaitBrain(next, true, "accepted");
            CHECK(uartCommands == 1 && f.executor.starts == 1 && f.runtime.active());
        });
    scenario("real Brain wrong-target Stop is rejected without cancelling physical ownership", [] {
        ++brainSenderScenarios;
        LinkFixture link;
        link.handshake();
        auto& f = link.device;
        const auto original = request();
        link.sendBrain(original);
        link.awaitBrain(original, true, "accepted");
        const auto correct = link.observedTarget(v4::Source::LocalTouch);
        auto wrong = correct;
        wrong.executionId[0] ^= 1;
        const auto before = encode(f.store.state());
        const auto disk = io.disk;
        const auto calls = io.calls.size();
        CHECK(link.brain.requestStop(wrong, link.now));
        for (unsigned i = 0; i < 300 && link.brain.stopSendState() == StopSendState::Pending; ++i) link.step();
        CHECK(link.brain.stopSendState() == StopSendState::Rejected && uartStops == 1);
        CHECK(f.executor.starts == 1 && !f.executor.stops && f.runtime.ownsMotion());
        CHECK(encode(f.store.state()) == before && io.disk == disk && io.calls.size() == calls);
        CHECK(link.brain.requestStop(correct, link.now));
        for (unsigned i = 0; i < 300 && link.brain.stopSendState() == StopSendState::Pending; ++i) link.step();
        CHECK(link.brain.stopSendState() == StopSendState::Received && uartStops == 2 && f.executor.stops == 1);
        CHECK(encode(f.store.state()) == before && io.disk == disk && io.calls.size() == calls);
        f.executor.confirm();
        link.run(200);
        link.publishStatus = false;
        link.queryOriginal(original, MotionOutcome::Failed);
    });
}
}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::pair<const char*, std::function<void()>>> groups = {
        {"acceptance", acceptance}, {"temperature", temperatureProjection}, {"contexts", contexts}, {"context_uart", contextUart},
        {"writes", writes}, {"duplicates", duplicates},
        {"rejections", rejections}, {"deferred", deferredRejections}, {"ttl", ttl}, {"terminals", terminals}, {"queues", queues},
        {"manual_owner", manualOwnership}, {"offline_link", offlineAndLink},
        {"stops", stops}, {"workbench", workbenchOwnership}, {"faults", faults}, {"uart", uartIntegration}};
    if (argc > 2) { std::fprintf(stderr, "Expected at most one test group\n"); return 2; }
    bool found = argc == 1;
    for (const auto& group : groups) {
        if (argc > 1 && std::strcmp(argv[1], group.first)) continue;
        found = true;
        const auto before = failures;
        group.second();
        std::printf("%s: %s\n", group.first, failures == before ? "PASS" : "FAIL");
    }
    if (!found) { std::fprintf(stderr, "Unknown test group\n"); return 2; }
    std::printf("%u scenarios, %u production command deliveries, %u read-only result lookups, %u failures\n",
                scenarios, deliveries, lookups, failures);
    std::printf("%u real UART exchanges, %u parsed frames, %u bounded short writes, %u zero writes (23-byte FIFO)\n",
                uartExchanges, uartFrames, uartShortWrites, uartZeroWrites);
    std::printf("%u real Brain sender scenarios (manual receive/replay counterexamples retained)\n", brainSenderScenarios);
    return failures ? 1 : 0;
}
