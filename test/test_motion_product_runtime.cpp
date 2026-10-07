#include "MotionProductRuntime.h"
#include "ReadOnlyBoardLink.h"
#include "FakeBrainNvs.h"
#include "FakeProductCrypto.h"

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
};
struct Hardware : motion::MotionProductHardware {
    explicit Hardware(Executor& executor) : executor(executor) {}
    Executor& executor;
    uint32_t clock = 1000;
    const char* owner = nullptr;
    bool executionValue = true;
    unsigned generated = 0;
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
};
struct Fixture {
    MotionStateStore store;
    Executor executor;
    motion::DemoFlowController flow{executor};
    motion::ProductSession product{flow};
    Hardware hardware{executor};
    motion::MotionProductRuntime runtime{store, product, flow, hardware};
    explicit Fixture(bool ready = true, unsigned queued = 0, bool restored = false) {
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
unsigned uartCommands = 0, uartStops = 0, uartQueries = 0;
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
struct PeerWire : v4::ByteSink {
    static constexpr size_t capacity = 23;
    std::array<uint8_t, capacity> bytes{};
    size_t used = 0, highWater = 0;
    unsigned calls = 0, shortWrites = 0, zeroWrites = 0;
    v4::Parser observer;
    v4::Assembler assembled;
    std::vector<v4::Frame> frames;
    std::vector<v4::Message> messages;
    bool idle() const override { return used == 0; }
    size_t available() const override { return capacity - used; }
    size_t write(const uint8_t* input, size_t length) override {
        CHECK(length && length <= available());
        ++calls;
        if (calls % 13 == 0) { ++zeroWrites; return 0; }
        const size_t written = length > 7 ? 7 : length;
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
            peer.receive(bytes[i], now);
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
    LinkFixture() {
        CHECK(!uartRuntime);
        uartRuntime = &device;
        uartCommands = uartStops = uartQueries = 0;
        uartCommandTtl = 0;
        auto brainPair = pairing();
        brainPair.role = v4::Role::Brain;
        std::swap(brainPair.localPhysicalId, brainPair.peerPhysicalId);
        CHECK(brain.begin(brainPair, 11) && motionLink.begin(pairing(), 22));
        CHECK(motionLink.setCommandHandler(routeCommand) && motionLink.setStopHandler(routeStop));
        CHECK(motionLink.setCommandReadyHandler(routeReady));
        CHECK(motionLink.setResultQueryHandler(routeQuery));
        CHECK(!brain.setCommandHandler(routeCommand) && !brain.setStopHandler(routeStop));
    }
    ~LinkFixture() { uartRuntime = nullptr; }
    void step() {
        device.hardware.clock = now;
        brain.poll(now, toMotion);
        motionLink.poll(now, toBrain);
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
        // Brain's public command sender is intentionally disabled. Inject real
        // encoded frames into its bounded UART wire, preserving frame boundaries
        // and using the production Motion parser/session/reassembler/handlers.
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
    void queryOriginal(const ProductRequest& request, MotionOutcome outcome) {
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
        CHECK(original.accepted && !std::strcmp(original.reason, "accepted") && original.outcome == outcome);
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
        LinkFixture link;
        link.handshake();
        auto& f = link.device;
        const auto original = request();
        f.executor.beforeStart = [&] {
            CHECK(sameMotionState(f.store.state(), decodedDisk()));
            CHECK(decodedDisk().slot.kind == MotionSlotKind::Intent && sameProductRequest(decodedDisk().slot.request, original));
        };
        link.send(link.command(original, 10000));
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
}
}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::pair<const char*, std::function<void()>>> groups = {
        {"acceptance", acceptance}, {"writes", writes}, {"duplicates", duplicates},
        {"rejections", rejections}, {"deferred", deferredRejections}, {"ttl", ttl}, {"terminals", terminals}, {"queues", queues},
        {"offline_link", offlineAndLink}, {"stops", stops}, {"faults", faults}, {"uart", uartIntegration}};
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
    return failures ? 1 : 0;
}
