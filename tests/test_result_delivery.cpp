#include "MotionResultDelivery.h"
#include "brain_result_delivery.h"
#include "ReadOnlyBoardLink.h"
#include "MotionProductRuntime.h"
#include "MotionStateRecovery.h"
#include "FakeBrainNvs.h"
#include "FakeProductCrypto.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace babytech::boardlink;
namespace v4 = babytech::v4;
namespace fake = fake_brain;
using fake::Bytes;
using fake::Op;
using fake::io;

namespace {
unsigned scenarios = 0, failures = 0;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + \
    std::to_string(__LINE__) + ": " #x); } while (false)
constexpr const char* execution = "123456789abcdef0123456789abcdef0";
constexpr const char* newExecution = "223456789abcdef0123456789abcdef0";
static_assert(kMotionResultQueueCapacity == 4, "capacity includes active prepare");

std::string executionId(uint64_t value) {
    char id[33];
    std::snprintf(id, sizeof(id), "%032llx", static_cast<unsigned long long>(value));
    return id;
}

v4::Pairing pairing() {
    v4::Pairing p{};
    p.role = v4::Role::Motion;
    std::strcpy(p.deviceId, "Babytech_01-result-test");
    std::strcpy(p.epoch, "0123456789abcdef0123456789abcdef");
    std::strcpy(p.localPhysicalId, "aabbccddeeff");
    std::strcpy(p.peerPhysicalId, "112233445566");
    return p;
}
v4::Pairing brainPairing(const v4::Pairing& motion) {
    auto p = motion;
    p.role = v4::Role::Brain;
    std::swap(p.localPhysicalId, p.peerPhysicalId);
    return p;
}
ProductContext context(uint32_t version = 10, bool cleared = false) {
    ProductContext c;
    std::strcpy(c.deviceId, pairing().deviceId);
    c.profileVersion = version;
    c.cleared = cleared;
    if (!cleared) {
        std::strcpy(c.babyId, "baby-original");
        std::strcpy(c.babyName, "Original baby name");
        std::strcpy(c.formulaBrand, "Original formula");
        c.waterMl = 120;
        c.temperatureC = 40;
        c.powderGPer100Ml = 13.512345f;
    }
    CHECK(validProductContext(c));
    return c;
}
ProductRequest request(uint64_t seq, v4::Source source,
                       ProductCommand command = ProductCommand::Prepare) {
    ProductRequest r;
    r.sequence = seq;
    r.source = source;
    r.command = command;
    std::strcpy(r.deviceId, pairing().deviceId);
    if (source == v4::Source::LocalTouch) {
        CHECK(makeLocalCommandId(brainPairing(pairing()), seq, r.commandId));
    } else std::snprintf(r.commandId, sizeof(r.commandId), "cloud-original-%llu",
                        static_cast<unsigned long long>(seq));
    if (command == ProductCommand::Prepare) {
        std::strcpy(r.babyId, context().babyId);
        r.profileVersion = 10;
        r.waterMl = 180;
        r.temperatureC = 45;
        r.powderGPer100Ml = context().powderGPer100Ml;
    }
    CHECK(validProductRequest(r));
    return r;
}
Bytes encode(const MotionState& state) {
    std::array<uint8_t, kMotionStateMaxSize> bytes{};
    const size_t length = encodeMotionState(state, bytes.data(), bytes.size());
    CHECK(length);
    return Bytes(bytes.begin(), bytes.begin() + length);
}
void seed() {
    MotionState state;
    state.pairing = pairing();
    CHECK(makeMotionContextBarrier(context(), state.context));
    CHECK(validMotionState(state));
    io.disk["productstate"]["record"] = {encode(state), fake::Type::Blob};
}
Bytes durable() { return io.disk.at("productstate").at("record").bytes; }
// Compare all active evidence, watermarks, recent decisions and context while
// excluding only the archived outbox that this receipt is allowed to change.
Bytes nonOutbox(const MotionState& state) {
    auto masked = state;
    masked.pendingResultCount = 0;
    for (auto& slot : masked.pendingResults) slot = MotionExecutionSlot{};
    return encode(masked);
}
fake::Database protectedData() {
    auto disk = io.disk;
    if (disk.count("productstate")) disk["productstate"].erase("record");
    return disk;
}
void audit() {
    CHECK(io.handles.empty());
    CHECK(!fake::count(Op::Erase) && !fake::count(Op::Init));
    for (const auto& call : io.calls) {
        CHECK(call.name == "productstate");
        CHECK(call.key.empty() || call.key == "record");
    }
    fake::verifyFaults();
}
void scenario(const std::string& name, const std::function<void()>& run) {
    fake::reset();
    fake_product_crypto::reset();
    for (const char* ns : {"productpair", "brainstate", "productctx", "outbox", "wifi-cfg",
                          "actuatorcfg", "sensorcfg"})
        io.disk[ns]["record"] = {{0, 0xff, 0x7f, 1}, fake::Type::Blob};
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
    }
}

class Wire : public v4::ByteSink {
public:
    Bytes bytes, history;
    size_t capacity = 64, writeLimit = 17;
    bool zeroWrite = false;
    bool idle() const override { return bytes.empty(); }
    size_t available() const override { return capacity - bytes.size(); }
    size_t write(const uint8_t* data, size_t size) override {
        CHECK(size <= available());
        if (zeroWrite) return 0;
        size = std::min(size, writeLimit);
        bytes.insert(bytes.end(), data, data + size);
        history.insert(history.end(), data, data + size);
        return size;
    }
    void deliver(ReadOnlyLink& peer, uint32_t now, bool drop) {
        for (const auto byte : bytes) if (!drop) peer.receive(byte, now);
        bytes.clear();
    }
};
std::vector<v4::Message> messages(const Bytes& bytes, v4::Kind kind) {
    v4::Parser parser;
    v4::Assembler assembler;
    v4::Frame frame;
    v4::Message message;
    std::vector<v4::Message> found;
    for (const auto byte : bytes)
        if (parser.push(byte, 0, frame) &&
            assembler.accept(frame, 0, message) == v4::AssemblyResult::Complete && message.kind == kind)
            found.push_back(message);
    return found;
}
bool fragmentedTerminal(const Bytes& bytes) {
    v4::Parser parser;
    v4::Frame frame;
    for (const auto byte : bytes)
        if (parser.push(byte, 0, frame) && frame.kind == v4::Kind::Terminal &&
            frame.offset == 0 && frame.total > frame.length) return true;
    return false;
}
std::string json(const v4::Message& message) {
    return std::string(reinterpret_cast<const char*>(message.payload), message.length);
}

// Only the thin caller surface adapts the actual link. Identity is derived
// from the loaded Store pairing, not a new readiness/identity gate.
struct BrainPort {
    ReadOnlyLink& core;
    v4::Pairing pairing;
    const v4::Pairing* verifiedPairing() const { return &pairing; }
    bool forwardCloudReceipt(const CloudReceipt& receipt, uint32_t now) {
        return core.forwardCloudReceipt(receipt, now);
    }
};
struct Capture {
    std::string eventId, originalJson;
};
struct NetworkIo {
    bool online = true;
    std::string rejectEvent;
    std::vector<Capture> attempts, published;
    bool publishTerminalEvent(const v4::Pairing& p, const v4::Message& message) {
        TerminalEvent decoded;
        CHECK(decodeTerminalEvent(message, p, decoded));
        Capture capture{decoded.eventId, json(message)};
        attempts.push_back(capture);
        if (!online || rejectEvent == decoded.eventId) return false;
        published.push_back(capture);
        return true;
    }
};
struct Attempt {
    std::string eventId;
    uint32_t at;
    bool accepted;
};
// Observation only: every operation is delegated to the real link unchanged.
struct ObservedMotionLink : ReadOnlyLink {
    std::vector<Attempt> attempts;
    bool publishTerminal(const TerminalEvent& event, uint32_t now) {
        const bool accepted = ReadOnlyLink::publishTerminal(event, now);
        attempts.push_back({event.eventId, now, accepted});
        return accepted;
    }
};
using MotionDelivery = motion::MotionResultDelivery<ObservedMotionLink>;
using BrainDelivery = babytech::brain::BrainResultDelivery<BrainPort, NetworkIo>;
struct Fixture;
Fixture* owner = nullptr;
bool receiveTerminal(const v4::Message&, const TerminalEvent&, uint32_t);
bool receiveReceipt(const CloudReceipt&, uint32_t);
bool receiveCommand(const CommandMessage&, uint32_t, CommandResult&);
bool runtimeResultReady(CommandResult&);
bool runtimeStop(const v4::StopRequest&, uint32_t);
bool storedQuery(const ResultQuery&, QueriedResult&);

struct Fixture {
    // The fixture itself is heap-owned; no large production Store/link object
    // is put on a small MCU-style caller stack.
    std::unique_ptr<MotionStateStore> store = std::make_unique<MotionStateStore>();
    ReadOnlyLink brain;
    ObservedMotionLink motion;
    BrainPort port{brain, {}};
    NetworkIo network;
    std::unique_ptr<MotionDelivery> delivery;
    std::unique_ptr<BrainDelivery> relay;
    Wire toMotion, toBrain;
    Status status;
    uint32_t now = 0;
    unsigned stepMs = 5;
    uint64_t brainBoot = 11, motionBoot = 22;
    unsigned receiptCalls = 0, commandCalls = 0;
    motion::MotionProductRuntime* runtime = nullptr;
    bool stationary = true, slotClearAllowed = true;
    bool motionMaintenance = false, brainMaintenance = false;
    bool telemetry = true;
    explicit Fixture(bool initial = true, uint32_t clock = 0) : now(clock) {
        if (initial) seed();
        CHECK(store->load(pairing()) == MotionLoad::Ready);
        port.pairing = brainPairing(store->state().pairing);
        CHECK(brain.begin(port.pairing, brainBoot));
        CHECK(motion.begin(store->state().pairing, motionBoot));
        delivery = std::make_unique<MotionDelivery>(motion, *store);
        relay = std::make_unique<BrainDelivery>(port, network);
        status.snapshot.stage = babytech::display::DisplayStage::Ready;
        std::strcpy(status.productProgress, "ready");
        owner = this;
        handlers();
        run(500, false);
        CHECK(brain.connected(now) && motion.connected(now));
    }
    ~Fixture() { if (owner == this) owner = nullptr; }
    void handlers() {
        CHECK(brain.setTerminalHandler(receiveTerminal));
        CHECK(motion.setCloudReceiptHandler(receiveReceipt));
        CHECK(motion.setCommandHandler(receiveCommand));
    }
    void step(bool owners = true, bool dropToMotion = false, bool dropToBrain = false) {
        status.sampleUptimeMs = now;
        status.stationary = stationary;
        if (owners) {
            relay->poll(now, brainMaintenance);
            delivery->poll(now, stationary && slotClearAllowed, motionMaintenance);
        }
        brain.poll(now, toMotion);
        motion.poll(now, toBrain, telemetry ? &status : nullptr);
        toMotion.deliver(motion, now, dropToMotion);
        toBrain.deliver(brain, now, dropToBrain);
        now += stepMs;
    }
    void run(unsigned duration, bool owners = true, bool dropToMotion = false, bool dropToBrain = false) {
        for (unsigned elapsed = 0; elapsed < duration; elapsed += stepMs)
            step(owners, dropToMotion, dropToBrain);
    }
    void add(v4::Source source, uint64_t seq, bool completed, bool archived = true) {
        const auto id = executionId(100 + 2 * seq + (source == v4::Source::CloudCommand));
        CHECK(store->recordDecision(request(seq, source), true, "accepted", id.c_str()) == MotionWrite::Stored);
        CHECK(store->finishFeeding(id.c_str(), completed, completed ? "" : "motor_fault",
                                  completed ? "" : "E_MOTOR", 987654 + uint32_t(seq)) == MotionWrite::Stored);
        if (archived) CHECK(store->archiveFeeding(true) == MotionWrite::Stored);
    }
    void brainReset() {
        relay = std::make_unique<BrainDelivery>(port, network);
        CHECK(brain.begin(port.pairing, ++brainBoot));
        toMotion.bytes.clear();
        handlers();
    }
    void motionReset() {
        audit();
        fake::reboot();
        fake_product_crypto::reset();
        delivery.reset();
        store = std::make_unique<MotionStateStore>();
        CHECK(store->load(pairing()) == MotionLoad::Ready);
        CHECK(motion.begin(store->state().pairing, ++motionBoot));
        delivery = std::make_unique<MotionDelivery>(motion, *store);
        toBrain.bytes.clear();
        handlers();
    }
    void noCommands() const {
        CHECK(commandCalls == 0);
        CHECK(messages(toMotion.history, v4::Kind::Command).empty());
        CHECK(messages(toBrain.history, v4::Kind::Command).empty());
    }
};
bool receiveTerminal(const v4::Message& message, const TerminalEvent&, uint32_t) {
    CHECK(owner);
    return owner->relay->terminal(message);
}
bool receiveReceipt(const CloudReceipt& receipt, uint32_t) {
    CHECK(owner);
    ++owner->receiptCalls;
    return owner->delivery->receipt(receipt);
}
bool receiveCommand(const CommandMessage& command, uint32_t now, CommandResult& result) {
    CHECK(owner);
    ++owner->commandCalls;
    return owner->runtime && owner->runtime->command(command, now, result);
}
bool runtimeResultReady(CommandResult& result) {
    CHECK(owner && owner->runtime);
    return owner->runtime->resultReady(result);
}
bool runtimeStop(const v4::StopRequest& stop, uint32_t now) {
    CHECK(owner && owner->runtime);
    return owner->runtime->stop(stop, now);
}
bool storedQuery(const ResultQuery& query, QueriedResult& result) {
    CHECK(owner);
    return queryMotionResult(*owner->store, query, result);
}
CloudReceipt receipt(const std::string& id) {
    CloudReceipt r;
    std::strcpy(r.deviceId, pairing().deviceId);
    CHECK(id.size() < sizeof(r.eventId));
    std::strcpy(r.eventId, id.c_str());
    return r;
}
bool cloudInput(Fixture& f, const CloudReceipt& input) {
    v4::Message encoded;
    CHECK(encodeCloudReceipt(input, encoded));
    CloudReceipt decoded;
    return decodeCloudReceipt(encoded, f.port.pairing.deviceId, decoded) && f.relay->receipt(decoded);
}
std::string expectedJson(const MotionState& state, const MotionExecutionSlot& slot) {
    TerminalEvent event;
    v4::Message message;
    CHECK(terminalEventFromSlot(state.pairing, slot, event));
    CHECK(encodeTerminalEvent(state.pairing, event, message));
    return json(message);
}
void unchanged(Fixture& f, const Bytes& ram, const Bytes& disk, unsigned writes) {
    CHECK(encode(f.store->state()) == ram && durable() == disk);
    CHECK(fake::count(Op::Set) == writes);
    f.noCommands();
}
void requireProgress(const Fixture& f, bool progressed, size_t since = 0) {
    if (progressed) return;
    const auto accepted = std::count_if(f.motion.attempts.begin() + since, f.motion.attempts.end(),
                                        [](const Attempt& a) { return a.accepted; });
    throw std::runtime_error("No complete delivery progress: Brain/Motion connected=" +
        std::to_string(f.brain.connected(f.now)) + "/" + std::to_string(f.motion.connected(f.now)) +
        ", owner attempts=" + std::to_string(f.motion.attempts.size() - since) +
        ", real-link admissions=" + std::to_string(accepted) +
        ", Network captures=" + std::to_string(f.network.published.size()));
}
void assertPayload(const Capture& capture, const MotionExecutionSlot& slot, const std::string& expected) {
    CHECK(capture.originalJson == expected && capture.eventId == slot.eventId);
    DynamicJsonDocument doc(4096);
    CHECK(!deserializeJson(doc, capture.originalJson));
    CHECK(doc["command_id"].as<std::string>() == slot.request.commandId);
    CHECK(doc["baby_id"].as<std::string>() == slot.request.babyId);
    CHECK(doc["feeding_context_profile_version"].as<uint32_t>() == slot.request.profileVersion);
    CHECK(doc["water_ml"].as<uint16_t>() == slot.request.waterMl);
    CHECK(doc["temp"].as<uint8_t>() == slot.request.temperatureC);
    CHECK(doc["uptime_ms"].as<uint32_t>() == slot.uptimeMs);
    CHECK(doc["powder_g_per_100ml"].as<float>() == slot.request.powderGPer100Ml);
    CHECK(doc["target_powder_g"].as<float>() == slot.targetPowderG);
    CHECK(doc["event"].as<std::string>() == (slot.completed ? "feeding_completed" : "feeding_failed"));
    CHECK(doc["water_delivery_basis"].as<std::string>() == "estimated_turns");
    CHECK(doc.containsKey("dispensed_water_ml") && doc["dispensed_water_ml"].isNull());
    CHECK(!doc.containsKey("execution_mode"));
    if (slot.request.source == v4::Source::CloudCommand)
        CHECK(doc["command_seq"].as<std::string>() == std::to_string(slot.request.sequence));
    else CHECK(!doc.containsKey("command_seq"));
    if (!slot.completed) {
        CHECK(doc["reason"].as<std::string>() == slot.reason);
        CHECK(doc["error_code"].as<std::string>() == slot.errorCode);
    }
}

// Hardware-only substitution for the additional real Runtime/Recovery groups.
// Session, flow, finishFeeding/archive and the ownership flags are production.
struct Executor : motion::DemoExecutor {
    motion::DemoEvidence sample{true, true, false, 0};
    motion::DemoExecution state = motion::DemoExecution::Running;
    unsigned starts = 0, stops = 0;
    bool healthy() const override { return true; }
    bool available() const override { return true; }
    motion::DemoEvidence evidence(uint8_t id) const override { CHECK(id == 1); return sample; }
    bool start(const motion::DemoScript&, bool, const std::array<int32_t, 256>&, uint32_t) override {
        ++starts;
        state = motion::DemoExecution::Running;
        sample = {true, false, false, 0};
        return true;
    }
    motion::DemoExecution execution() const override { return state; }
    bool stop() override { ++stops; sample.fresh = false; return true; }
    bool reset() override { return true; }
    void confirm() { sample = {true, true, false, 0}; }
};
struct RuntimeHardware : motion::MotionProductHardware {
    Executor& executor;
    uint32_t clock;
    unsigned generated = 0;
    RuntimeHardware(Executor& e, uint32_t now) : executor(e), clock(now) {}
    uint32_t nowMs() const override { return clock; }
    const char* unavailable() const override { return nullptr; }
    bool newExecution(char (&id)[33]) override {
        std::strcpy(id, executionId(1000 + ++generated).c_str());
        return true;
    }
    bool stationary() const override {
        return executor.sample.fresh && executor.sample.stationary && !executor.sample.fault;
    }
};
struct RuntimeHarness {
    Executor executor;
    motion::DemoFlowController flow{executor};
    motion::ProductSession product{flow};
    RuntimeHardware hardware;
    motion::MotionProductRuntime runtime;
    RuntimeHarness(MotionStateStore& store, uint32_t now, bool initialize = true)
        : hardware(executor, now), runtime(store, product, flow, hardware) {
        motion::DemoConfig config;
        config.configured = true;
        config.axes.push_back({1, 10, 0, true});
        config.initialization.commands = {"initialize"};
        for (auto& stage : config.stages) stage.commands = {"stage"};
        CHECK(flow.apply(config));
        ContextResult result;
        CHECK(runtime.context(context(), now, result));
        CHECK(result.status == ContextStatus::Unchanged);
        product.setExecutionAuthorized(true);
        product.resources(true, false, true, 300, now);
        if (initialize) {
            CHECK(product.initialize(now - 3));
            flow.tick(now - 2);
            executor.state = motion::DemoExecution::Done;
            executor.confirm();
            flow.tick(now - 1);
            product.tick(now - 1);
            CHECK(flow.startEnabled() && product.canStart());
        }
        executor.starts = executor.stops = 0;
    }
    CommandResult command(const ProductRequest& request, uint32_t now) {
        hardware.clock = now;
        CommandMessage command;
        command.request = request;
        command.remainingTtlMs = 5000;
        v4::Message encoded;
        CommandMessage decoded;
        CHECK(encodeCommand(command, encoded) && decodeCommand(encoded, decoded));
        CommandResult result;
        CHECK(runtime.command(decoded, now, result));
        CHECK(runtime.resultReady(result));
        return result;
    }
    void tick(uint32_t now) {
        hardware.clock = now;
        flow.tick(now);
        product.tick(now);
        runtime.poll(now);
    }
    uint32_t finish(uint32_t now) {
        for (unsigned stage = 0; stage < 5; ++stage) {
            tick(now + 2 * stage);
            executor.state = motion::DemoExecution::Done;
            if (stage == 4) executor.confirm();
            tick(now + 2 * stage + 1);
        }
        CHECK(!product.active() && executor.starts == 5);
        return now + 9;
    }
};
struct RecoveryHardware : motion::MotionRecoveryHardware {
    bool fresh = true, atRest = true;
    unsigned stops = 0;
    void supervisedStop(uint32_t) override { ++stops; fresh = atRest = false; }
    bool stationary() const override { return fresh && atRest; }
    void confirm() { fresh = atRest = true; }
};

void runtimeOwnership() {
    for (const auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch})
        for (const auto point : {Op::Set, Op::Commit, Op::OpenRO})
            scenario("Runtime owns unarchived Terminal after Flash ages stationary evidence", [&] {
                auto f = std::make_unique<Fixture>();
                auto r = std::make_unique<RuntimeHarness>(*f->store, f->now);
                const auto accepted = r->command(request(1, source), f->now);
                CHECK(accepted.accepted && r->runtime.active());
                bool hit = false;
                const unsigned occurrence = fake::count(point) + 1;
                const unsigned commit = fake::count(Op::Commit) + 1;
                io.before = [&](const fake::Call& call) {
                    if (!hit && call.op == point && (point == Op::OpenRO ?
                        fake::count(Op::Commit) == commit : call.occurrence == occurrence)) {
                        hit = true;
                        r->executor.sample.fresh = false;
                    }
                };
                f->now = r->finish(f->now + 10);
                io.before = {};
                CHECK(hit && r->runtime.active() && !r->hardware.stationary());
                CHECK(f->store->state().slot.kind == MotionSlotKind::Terminal);
                CHECK(f->store->state().pendingResultCount == 0);
                const auto slot = f->store->state().slot;
                const auto ram = encode(f->store->state()), disk = durable();
                const unsigned writes = fake::count(Op::Set);
                r->executor.confirm();
                CHECK(r->hardware.stationary() && r->runtime.active());
                f->stationary = r->hardware.stationary();
                f->slotClearAllowed = !r->runtime.active();
                CHECK(f->stationary && !f->slotClearAllowed);
                f->run(1000);
                CHECK(cloudInput(*f, receipt(slot.eventId)));
                f->run(1000);
                unchanged(*f, ram, disk, writes);
                CHECK(r->runtime.active() && r->executor.starts == 5);
                r->hardware.clock = f->now;
                r->runtime.poll(f->now);
                CHECK(!r->runtime.active() && f->store->state().slot.kind == MotionSlotKind::Empty);
                CHECK(f->store->state().pendingResultCount == 1);
                CHECK(fake::count(Op::Set) == writes + 1);
                const auto archived = f->store->state().pendingResults[0];
                CHECK(expectedJson(f->store->state(), archived) == expectedJson(f->store->state(), slot));
                f->stationary = r->hardware.stationary();
                f->slotClearAllowed = !r->runtime.active();
                f->delivery->poll(f->now, f->stationary && f->slotClearAllowed);
                CHECK(f->store->state().pendingResultCount == 0 && fake::count(Op::Set) == writes + 2);
                r->tick(f->now + 4000);
                CHECK(r->flow.startEnabled());
                // Preserve the normal bottle-full latch: a new Initialize,
                // not delivery or a test-only reset, authorizes another feed.
                const auto initialize = r->command(request(source == v4::Source::LocalTouch ? 2 : 1,
                                                          v4::Source::LocalTouch, ProductCommand::Initialize),
                                                   f->now + 4001);
                CHECK(initialize.accepted);
                for (unsigned tick = 2; tick <= 5; ++tick) {
                    r->executor.confirm();
                    r->executor.state = motion::DemoExecution::Done;
                    r->tick(f->now + 4000 + tick);
                }
                CHECK(!r->runtime.active() && r->product.canStart());
                const unsigned starts = r->executor.starts;
                const auto next = r->command(request(source == v4::Source::LocalTouch ? 3 : 2, source),
                                             f->now + 4006);
                CHECK(next.accepted && r->runtime.active());
                CHECK(f->store->state().slot.kind == MotionSlotKind::Intent);
                CHECK(r->executor.starts == starts); // Next feed stage has not ticked.
            });
}

void recoveryOwnership() {
    for (const auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch})
        for (const auto point : {Op::Set, Op::Commit, Op::OpenRO})
            scenario("Stop confirmed is not Recovery execution release after stale Flash feedback", [&] {
                auto f = std::make_unique<Fixture>();
                CHECK(f->store->recordDecision(request(1, source), true, "accepted", execution) == MotionWrite::Stored);
                f->motionReset();
                auto hardware = std::make_unique<RecoveryHardware>();
                auto recovery = std::make_unique<motion::MotionStateRecovery>(*f->store, *hardware);
                CHECK(recovery->begin(pairing(), f->now) == MotionLoad::Ready);
                CHECK(hardware->stops == 1 && recovery->motionPending() && recovery->executionPending());
                hardware->confirm();
                bool hit = false;
                const unsigned occurrence = fake::count(point) + 1;
                const unsigned commit = fake::count(Op::Commit) + 1;
                io.before = [&](const fake::Call& call) {
                    if (!hit && call.op == point && (point == Op::OpenRO ?
                        fake::count(Op::Commit) == commit : call.occurrence == occurrence)) {
                        hit = true;
                        hardware->fresh = false;
                    }
                };
                recovery->poll();
                io.before = {};
                CHECK(hit && !recovery->motionPending() && recovery->executionPending());
                CHECK(!hardware->stationary() && f->store->state().slot.kind == MotionSlotKind::Terminal);
                const auto slot = f->store->state().slot;
                CHECK(!slot.completed && !std::strcmp(slot.reason, "reboot_during_feed"));
                const auto ram = encode(f->store->state()), disk = durable();
                const unsigned writes = fake::count(Op::Set);
                hardware->confirm();
                CHECK(!recovery->motionPending() && recovery->executionPending() && hardware->stationary());
                f->stationary = hardware->stationary();
                f->slotClearAllowed = !recovery->executionPending();
                CHECK(f->stationary && !f->slotClearAllowed);
                // Complete the new-boot handshake before injecting a stored
                // receipt; otherwise the old Brain session can lose it on wire.
                f->run(1000);
                CHECK(cloudInput(*f, receipt(slot.eventId)));
                f->run(2000);
                unchanged(*f, ram, disk, writes);
                CHECK(recovery->executionPending());
                recovery->poll();
                CHECK(!recovery->executionPending() && !recovery->motionPending());
                CHECK(f->store->state().pendingResultCount == 1 && fake::count(Op::Set) == writes + 1);
                f->stationary = hardware->stationary();
                f->slotClearAllowed = !recovery->executionPending();
                f->delivery->poll(f->now, f->stationary && f->slotClearAllowed);
                CHECK(f->receiptCalls > 0);
                CHECK(f->store->state().pendingResultCount == 0 && fake::count(Op::Set) == writes + 2);
                CHECK(hardware->stops == 1);
                auto runtime = std::make_unique<RuntimeHarness>(*f->store, f->now);
                CHECK(runtime->command(request(2, source), f->now).accepted);
                CHECK(runtime->runtime.active() && runtime->executor.starts == 0);
            });
}

void fullQueueRuntime() {
    for (const auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch})
        scenario("stored releases full capacity; another old receipt preserves real new Runtime Intent", [&] {
            auto f = std::make_unique<Fixture>();
            for (uint64_t seq = 1; seq <= 4; ++seq) f->add(source, seq, seq % 2);
            const auto full = encode(f->store->state()), disk = durable();
            const unsigned writes = fake::count(Op::Set);
            CHECK(f->store->recordDecision(request(5, source), true, "accepted", newExecution) == MotionWrite::QueueFull);
            unchanged(*f, full, disk, writes);
            CHECK(cloudInput(*f, receipt(f->store->state().pendingResults[0].eventId)));
            f->run(1000);
            CHECK(f->store->state().pendingResultCount == 3);
            auto runtime = std::make_unique<RuntimeHarness>(*f->store, f->now);
            CHECK(runtime->command(request(5, source), f->now).accepted);
            CHECK(runtime->runtime.active() && f->store->state().slot.kind == MotionSlotKind::Intent);
            runtime->tick(f->now + 1);
            CHECK(runtime->executor.starts == 1 && !runtime->hardware.stationary());
            const auto frozen = nonOutbox(f->store->state());
            const auto original = f->store->state().pendingResults[0];
            f->stationary = runtime->hardware.stationary();
            f->slotClearAllowed = !runtime->runtime.active();
            CHECK(!f->stationary && !f->slotClearAllowed && cloudInput(*f, receipt(original.eventId)));
            f->run(1000);
            CHECK(f->store->state().pendingResultCount == 2);
            CHECK(nonOutbox(f->store->state()) == frozen);
            CHECK(runtime->runtime.active() && runtime->executor.starts == 1);
        });
}

struct SessionRecoveryHardware : motion::MotionRecoveryHardware {
    RuntimeHarness& device;
    explicit SessionRecoveryHardware(RuntimeHarness& runtime) : device(runtime) {}
    void supervisedStop(uint32_t now) override { device.product.recoverAfterRestart(now); }
    bool stationary() const override {
        return device.hardware.stationary() && !device.flow.busy() && !device.product.ownsMotion();
    }
};

void bindRuntime(Fixture& f, RuntimeHarness& device) {
    f.runtime = &device.runtime;
    CHECK(f.motion.setCommandReadyHandler(runtimeResultReady));
    CHECK(f.motion.setStopHandler(runtimeStop));
    CHECK(f.motion.setResultQueryHandler(storedQuery));
}
void runtimeLoop(Fixture& f, RuntimeHarness& device, motion::MotionStateRecovery* recovery = nullptr) {
    // Match pollDemo's production ownership order, with only feedback/I/O
    // substituted. This harness does not compile or execute main.cpp.
    device.hardware.clock = f.now;
    device.flow.tick(f.now);
    device.product.tick(f.now);
    if (recovery) recovery->poll();
    device.runtime.poll(f.now);
    f.stationary = device.hardware.stationary();
    f.slotClearAllowed = !device.runtime.active() && (!recovery || !recovery->executionPending());
    f.status.snapshot = device.product.displaySnapshot();
    f.status.motionBusy = device.runtime.ownsMotion() || device.product.ownsMotion() || device.flow.busy();
    device.runtime.project(f.status, f.motion.connected(f.now));
    f.step();
}
void runRuntime(Fixture& f, RuntimeHarness& device, unsigned duration,
                motion::MotionStateRecovery* recovery = nullptr) {
    for (unsigned elapsed = 0; elapsed < duration; elapsed += f.stepMs) runtimeLoop(f, device, recovery);
}
CommandResult uartCommand(Fixture& f, RuntimeHarness& device, const ProductRequest& request,
                          motion::MotionStateRecovery* recovery = nullptr) {
    CommandMessage command;
    command.request = request;
    command.remainingTtlMs = 5000;
    CHECK(f.brain.requestCommand(command, f.now));
    for (unsigned elapsed = 0; elapsed < 4000 && f.brain.commandSendState() == CommandSendState::Pending;
         elapsed += f.stepMs) runtimeLoop(f, device, recovery);
    CHECK(f.brain.commandSendState() == CommandSendState::Complete);
    const auto result = f.brain.commandResponse();
    CHECK(result.source == request.source && result.sequence == request.sequence);
    CHECK(!std::strcmp(result.commandId, request.commandId));
    return result;
}
QueriedResult uartQuery(Fixture& f, RuntimeHarness& device, const ProductRequest& request,
                       motion::MotionStateRecovery* recovery = nullptr) {
    ResultQuery query;
    query.source = request.source;
    query.sequence = request.sequence;
    std::strcpy(query.deviceId, request.deviceId);
    std::strcpy(query.commandId, request.commandId);
    CHECK(f.brain.requestResult(query, f.now));
    for (unsigned elapsed = 0; elapsed < 4000 && f.brain.resultLookupState() == ResultLookupState::Pending;
         elapsed += f.stepMs) runtimeLoop(f, device, recovery);
    CHECK(f.brain.resultLookupState() == ResultLookupState::Complete);
    const auto result = f.brain.resultQueryResponse();
    CHECK(sameResultQuery(result.query, query));
    return result;
}
const MotionExecutionSlot* archivedResult(const MotionState& state, const std::string& eventId) {
    for (size_t i = 0; i < state.pendingResultCount; ++i)
        if (state.pendingResults[i].eventId == eventId) return &state.pendingResults[i];
    return nullptr;
}
void assertWatermarksAndDecisions(const MotionState& actual, const MotionState& original) {
    auto expected = original;
    expected.slot = actual.slot;
    CHECK(nonOutbox(actual) == nonOutbox(expected));
}

void activeHistoryFaults() {
    for (const auto oldSource : {v4::Source::CloudCommand, v4::Source::LocalTouch})
        for (const auto newSource : {v4::Source::CloudCommand, v4::Source::LocalTouch})
            for (unsigned fault = 0; fault < 6; ++fault) for (bool stop : {false, true})
                scenario("active Runtime/history receipt fault old=" + std::to_string(unsigned(oldSource)) +
                         " new=" + std::to_string(unsigned(newSource)) + " fault=" + std::to_string(fault) +
                         " stop=" + std::to_string(stop), [&] {
                    auto f = std::make_unique<Fixture>();
                    // 17-byte short writes at 1 ms meet the real 50 ms Command
                    // first-frame budget; the existing terminal suites use 5 ms.
                    f->stepMs = 1;
                    f->add(oldSource, 1, true);
                    f->add(oldSource == v4::Source::CloudCommand ? v4::Source::LocalTouch :
                           v4::Source::CloudCommand, 1, false);
                    const auto old = f->store->state().pendingResults[0];
                    const auto sibling = f->store->state().pendingResults[1];
                    const auto oldJson = expectedJson(f->store->state(), old);
                    const auto siblingJson = expectedJson(f->store->state(), sibling);
                    auto device = std::make_unique<RuntimeHarness>(*f->store, f->now);
                    bindRuntime(*f, *device);
                    auto next = request(2, newSource);
                    next.waterMl = 210;
                    next.temperatureC = 41;
                    const auto accepted = uartCommand(*f, *device, next);
                    CHECK(accepted.accepted && !std::strcmp(accepted.reason, "accepted"));
                    runRuntime(*f, *device, 1500);
                    CHECK(device->runtime.active() && device->runtime.ownsMotion() && device->product.active());
                    CHECK(device->executor.starts == 1 && device->executor.stops == 0 && device->hardware.generated == 1);
                    CHECK(!device->hardware.stationary() && !f->stationary);
                    CHECK(f->commandCalls == 1);
                    CHECK(messages(f->toMotion.history, v4::Kind::Command).size() == 1);
                    bool publishedOld = false;
                    for (const auto& capture : f->network.published) if (capture.eventId == old.eventId) {
                        assertPayload(capture, old, oldJson);
                        publishedOld = true;
                    }
                    CHECK(publishedOld);
                    const auto originalQuery = uartQuery(*f, *device, next);
                    CHECK(originalQuery.status == ResultQueryStatus::Known && originalQuery.accepted);
                    CHECK(originalQuery.outcome == MotionOutcome::None && std::strlen(originalQuery.requestDigestHex) == 64);
                    const auto original = f->store->state();
                    const auto ram = encode(original), disk = durable(), frozen = nonOutbox(original);
                    CHECK(original.slot.kind == MotionSlotKind::Intent && sameProductRequest(original.slot.request, next));
                    const unsigned sets = fake::count(Op::Set), commits = fake::count(Op::Commit);
                    // Faults target only deletion of the archived result. Runtime
                    // remains physically active, so no terminal write can race it.
                    if (fault < 2) fake::fail(Op::Set, sets + 1, ESP_FAIL, fault == 1);
                    else if (fault < 4) fake::fail(Op::Commit, commits + 1, ESP_FAIL, fault == 3);
                    else if (fault == 4) {
                        io.durableOnSet = true;
                        fake::fail(Op::Commit, commits + 1);
                    } else fake::fail(Op::Read, fake::count(Op::Read) + 3);
                    const bool persisted = fault == 1 || fault >= 3;
                    const unsigned received = f->receiptCalls;
                    const auto receiptFrames = messages(f->toMotion.history, v4::Kind::CloudReceipt).size();
                    CHECK(cloudInput(*f, receipt(old.eventId)));
                    for (unsigned elapsed = 0; elapsed < 2000 && !f->store->faulted(); elapsed += f->stepMs)
                        runtimeLoop(*f, *device);
                    CHECK(f->receiptCalls == received + 1 && f->store->faulted() && !f->store->ready());
                    runtimeLoop(*f, *device); // Project the fault on the following owner tick.
                    audit();
                    const auto receipts = messages(f->toMotion.history, v4::Kind::CloudReceipt);
                    CHECK(receipts.size() == receiptFrames + 1);
                    v4::Message encodedReceipt;
                    CHECK(encodeCloudReceipt(receipt(old.eventId), encodedReceipt));
                    CHECK(json(receipts.back()) == json(encodedReceipt));
                    CHECK(encode(f->store->state()) == ram && nonOutbox(f->store->state()) == frozen);
                    CHECK(fake::count(Op::Set) == sets + 1);
                    CHECK(fake::count(Op::Commit) == commits + (fault >= 2 ? 1 : 0));
                    CHECK(device->executor.starts == 1 && device->executor.stops == 0 && device->hardware.generated == 1);
                    CHECK(device->runtime.active() && device->runtime.ownsMotion() && device->product.active());
                    CHECK(!f->status.executionAuthorized && !f->status.snapshot.startEnabled);
                    auto onDisk = std::make_unique<MotionState>();
                    const auto written = durable();
                    CHECK(decodeMotionState(written.data(), written.size(), *onDisk));
                    CHECK(nonOutbox(*onDisk) == frozen);
                    CHECK((written != disk) == persisted);
                    CHECK(onDisk->pendingResultCount == (persisted ? 1 : 2));
                    CHECK((archivedResult(*onDisk, old.eventId) == nullptr) == persisted);
                    const auto* retainedSibling = archivedResult(*onDisk, sibling.eventId);
                    CHECK(retainedSibling && expectedJson(*onDisk, *retainedSibling) == siblingJson);
                    const size_t calls = io.calls.size();
                    // No fabricated replacement Store and no hidden auto-Stop:
                    // exercise the real flow's completion or explicit Stop path.
                    if (stop) {
                        v4::StopRequest request;
                        request.source = v4::Source::LocalTouch;
                        request.scope = v4::StopScope::Product;
                        for (size_t i = 0; i < 16; ++i) {
                            unsigned byte = 0;
                            CHECK(std::sscanf(original.slot.executionId + 2 * i, "%2x", &byte) == 1);
                            request.executionId[i] = uint8_t(byte);
                        }
                        CHECK(f->brain.requestStop(request, f->now));
                        for (unsigned elapsed = 0; elapsed < 2000 && f->brain.stopSendState() == StopSendState::Pending;
                             elapsed += f->stepMs) runtimeLoop(*f, *device);
                        CHECK(f->brain.stopSendState() == StopSendState::Received);
                        CHECK(device->executor.stops == 1 && device->executor.starts == 1);
                        CHECK(device->product.active() && device->runtime.ownsMotion());
                        runRuntime(*f, *device, 100);
                        CHECK(device->product.active()); // Stop receipt is not stationary evidence.
                        device->executor.confirm();
                        runtimeLoop(*f, *device);
                    } else {
                        for (unsigned stage = 0; stage < 5; ++stage) {
                            device->executor.state = motion::DemoExecution::Done;
                            if (stage == 4) device->executor.confirm();
                            runtimeLoop(*f, *device);
                            if (stage < 4) runtimeLoop(*f, *device);
                        }
                    }
                    CHECK(!device->product.active() && !device->runtime.ownsMotion());
                    CHECK(device->runtime.active()); // Faulted Store cannot archive the retained Intent.
                    CHECK(f->stationary && f->status.stationary && !f->slotClearAllowed);
                    CHECK(device->executor.starts == (stop ? 1u : 5u));
                    CHECK(device->executor.stops == (stop ? 1u : 0u) && device->hardware.generated == 1);
                    CHECK(!device->product.eventPending());
                    CHECK(io.calls.size() == calls && encode(f->store->state()) == ram && durable() == written);
                    runRuntime(*f, *device, 500);
                    const auto failedQuery = uartQuery(*f, *device, next);
                    CHECK(failedQuery.status == ResultQueryStatus::StorageFault && !failedQuery.accepted);
                    const auto rejected = uartCommand(*f, *device, next);
                    CHECK(!rejected.accepted && !std::strcmp(rejected.reason, "storage_fault"));
                    CHECK(device->executor.starts == (stop ? 1u : 5u) && device->hardware.generated == 1);
                    CHECK(io.calls.size() == calls && encode(f->store->state()) == ram && durable() == written);

                    f->runtime = nullptr;
                    device.reset();
                    f->motionReset();
                    CHECK(encode(f->store->state()) == written && f->store->state().slot.kind == MotionSlotKind::Intent);
                    device = std::make_unique<RuntimeHarness>(*f->store, f->now, false);
                    bindRuntime(*f, *device);
                    auto hardware = std::make_unique<SessionRecoveryHardware>(*device);
                    auto recovery = std::make_unique<motion::MotionStateRecovery>(*f->store, *hardware);
                    const uint32_t rebootAt = f->now;
                    CHECK(recovery->begin(f->store->state().pairing, rebootAt) == MotionLoad::Ready);
                    CHECK(recovery->motionPending() && recovery->executionPending());
                    CHECK(device->executor.stops == 1 && device->executor.starts == 0 && device->hardware.generated == 0);
                    f->network.published.clear();
                    runRuntime(*f, *device, 1000, recovery.get());
                    CHECK(recovery->motionPending() && recovery->executionPending());
                    CHECK(!fake::count(Op::Set) && !fake::count(Op::Commit));
                    CHECK(encode(f->store->state()) == written && durable() == written);
                    const auto pendingQuery = uartQuery(*f, *device, next, recovery.get());
                    CHECK(pendingQuery.status == ResultQueryStatus::Known && pendingQuery.accepted);
                    CHECK(pendingQuery.outcome == MotionOutcome::None);
                    CHECK(!std::strcmp(pendingQuery.requestDigestHex, originalQuery.requestDigestHex));
                    const auto duplicate = uartCommand(*f, *device, next, recovery.get());
                    CHECK(duplicate.accepted && !std::strcmp(duplicate.reason, "accepted"));
                    CHECK(device->executor.starts == 0 && device->hardware.generated == 0 && !device->runtime.active());
                    CHECK(encode(f->store->state()) == written && durable() == written);
                    CHECK(!fake::count(Op::Set) && !fake::count(Op::Commit));
                    device->executor.confirm();
                    runtimeLoop(*f, *device, recovery.get());
                    CHECK(!recovery->motionPending() && !recovery->executionPending());
                    CHECK(device->executor.starts == 0 && device->executor.stops == 1 && device->hardware.generated == 0);
                    CHECK(f->store->state().slot.kind == MotionSlotKind::Empty);
                    CHECK(fake::count(Op::Set) == 2 && fake::count(Op::Commit) == 2);
                    CHECK(f->store->state().pendingResultCount == (persisted ? 2 : 3));
                    assertWatermarksAndDecisions(f->store->state(), *onDisk);
                    const auto* failed = archivedResult(f->store->state(), original.slot.eventId);
                    CHECK(failed && !failed->completed && sameProductRequest(failed->request, next));
                    CHECK(!std::memcmp(failed->digest, original.slot.digest, sizeof(failed->digest)));
                    CHECK(!std::strcmp(failed->executionId, original.slot.executionId));
                    CHECK(!std::strcmp(failed->reason, "reboot_during_feed"));
                    CHECK(!std::strcmp(failed->errorCode, "E_REBOOT_DURING_FEED") && failed->uptimeMs == rebootAt);
                    const auto recoveredSlot = *failed;
                    const auto recoveredJson = expectedJson(f->store->state(), recoveredSlot);
                    const auto recoveredBytes = encode(f->store->state());
                    runRuntime(*f, *device, 6000, recovery.get());
                    std::set<std::string> ids;
                    for (const auto& capture : f->network.published) {
                        ids.insert(capture.eventId);
                        if (capture.eventId == old.eventId) assertPayload(capture, old, oldJson);
                        else if (capture.eventId == sibling.eventId) assertPayload(capture, sibling, siblingJson);
                        else {
                            CHECK(capture.eventId == recoveredSlot.eventId);
                            assertPayload(capture, recoveredSlot, recoveredJson);
                        }
                    }
                    CHECK(ids.count(sibling.eventId) && ids.count(recoveredSlot.eventId));
                    CHECK(ids.count(old.eventId) == (persisted ? 0u : 1u));
                    const auto recoveredQuery = uartQuery(*f, *device, next, recovery.get());
                    CHECK(recoveredQuery.status == ResultQueryStatus::Known && recoveredQuery.accepted);
                    CHECK(recoveredQuery.outcome == MotionOutcome::Failed);
                    CHECK(!std::strcmp(recoveredQuery.requestDigestHex, originalQuery.requestDigestHex));
                    CHECK(uartCommand(*f, *device, next, recovery.get()).accepted);
                    CHECK(!device->runtime.active() && !device->product.active());
                    CHECK(device->executor.starts == 0 && device->executor.stops == 1 && device->hardware.generated == 0);
                    CHECK(encode(f->store->state()) == recoveredBytes && durable() == recoveredBytes);
                    CHECK(fake::count(Op::Set) == 2 && fake::count(Op::Commit) == 2);
                    f->runtime = nullptr;
                });
}

void roundtrip() {
    for (const auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch})
        for (bool completed : {false, true}) for (bool archived : {false, true})
            scenario("fragmented original JSON and stored-only removal", [&] {
                auto f = std::make_unique<Fixture>();
                f->add(source, 1, completed, archived);
                const auto slot = archived ? f->store->state().pendingResults[0] : f->store->state().slot;
                const auto expected = expectedJson(f->store->state(), slot);
                const auto ram = encode(f->store->state()), disk = durable();
                const unsigned writes = fake::count(Op::Set);
                f->stationary = false;
                f->run(2000);
                CHECK(f->network.published.size() >= 2);
                CHECK(fragmentedTerminal(f->toBrain.history));
                CHECK(!messages(f->toMotion.history, v4::Kind::LinkAck).empty());
                for (const auto& capture : f->network.published) assertPayload(capture, slot, expected);
                unchanged(*f, ram, disk, writes);
                CHECK(cloudInput(*f, receipt(slot.eventId)));
                f->run(500);
                if (!archived) {
                    unchanged(*f, ram, disk, writes);
                    f->stationary = true;
                    f->run(500);
                }
                CHECK(f->store->ready() && !f->store->state().pendingResultCount);
                CHECK(f->store->state().slot.kind == MotionSlotKind::Empty);
                CHECK(fake::count(Op::Set) == writes + 1);
                const auto receipts = messages(f->toMotion.history, v4::Kind::CloudReceipt);
                CHECK(receipts.size() == 1);
                v4::Message encoded;
                CHECK(encodeCloudReceipt(receipt(slot.eventId), encoded));
                CHECK(json(receipts[0]) == json(encoded));
                const auto removed = durable();
                CHECK(cloudInput(*f, receipt(slot.eventId)));
                f->run(1500);
                CHECK(durable() == removed && fake::count(Op::Set) == writes + 1);
                f->noCommands();
            });
}

void intentAndCapacity() {
    for (const auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch}) {
        scenario("unfinished Intent is never published, archived or acknowledged on restart", [&] {
            auto f = std::make_unique<Fixture>();
            CHECK(f->store->recordDecision(request(1, source), true, "accepted", execution) == MotionWrite::Stored);
            const auto ram = encode(f->store->state()), disk = durable();
            CHECK(f->store->archiveFeeding(true) == MotionWrite::Busy);
            CHECK(!f->delivery->receipt(receipt(f->store->state().slot.eventId)));
            f->run(3000);
            CHECK(f->network.attempts.empty());
            CHECK(messages(f->toBrain.history, v4::Kind::Terminal).empty());
            f->motionReset();
            f->run(3000);
            unchanged(*f, ram, disk, 0);
            CHECK(f->store->state().slot.kind == MotionSlotKind::Intent);
            CHECK(f->network.attempts.empty());
        });
        scenario("three archived plus active is four; full outbox permits non-prepare active", [&] {
            auto f = std::make_unique<Fixture>();
            for (uint64_t seq = 1; seq <= 3; ++seq) f->add(source, seq, seq % 2);
            CHECK(f->store->recordDecision(request(4, source), true, "accepted", newExecution) == MotionWrite::Stored);
            CHECK(f->store->state().pendingResultCount == 3);
            CHECK(f->store->state().slot.kind == MotionSlotKind::Intent);
            f->run(5000);
            std::set<std::string> ids;
            for (const auto& capture : f->network.published) ids.insert(capture.eventId);
            CHECK(ids.size() == 3);
            CHECK(f->store->finishFeeding(newExecution, true, "", "", 888) == MotionWrite::Stored);
            CHECK(f->store->archiveFeeding(true) == MotionWrite::Stored);
            CHECK(f->store->state().pendingResultCount == 4);
            const auto full = encode(f->store->state()), disk = durable();
            const unsigned writes = fake::count(Op::Set);
            CHECK(f->store->recordDecision(request(5, source), true, "accepted", execution) == MotionWrite::QueueFull);
            unchanged(*f, full, disk, writes);
            CHECK(f->store->recordDecision(request(5, source, ProductCommand::Clean), true,
                                          "accepted", execution) == MotionWrite::Stored);
            const auto evidence = nonOutbox(f->store->state());
            f->stationary = false;
            CHECK(cloudInput(*f, receipt(f->store->state().pendingResults[0].eventId)));
            f->run(1000);
            CHECK(f->store->state().pendingResultCount == 3);
            CHECK(nonOutbox(f->store->state()) == evidence);
            f->noCommands();
        });
    }
}

void archivedDuringActive() {
    for (const auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch})
        for (const auto command : {ProductCommand::Prepare, ProductCommand::Initialize, ProductCommand::Clean})
            scenario("old result receipt does not stop or mutate a newer task", [&] {
                auto f = std::make_unique<Fixture>();
                f->add(source, 1, false);
                f->add(source, 2, true);
                const auto old = f->store->state().pendingResults[0];
                const std::string original = expectedJson(f->store->state(), old);
                const auto nextSource = command == ProductCommand::Initialize ? v4::Source::LocalTouch : source;
                CHECK(f->store->recordDecision(request(3, nextSource, command), true,
                                              "accepted", newExecution) == MotionWrite::Stored);
                CHECK(f->store->saveContext(context(11, true)) == MotionWrite::Stored);
                const auto evidence = nonOutbox(f->store->state());
                const std::string otherId = f->store->state().pendingResults[1].eventId;
                f->stationary = false;
                f->status.snapshot.stage = babytech::display::DisplayStage::Mixing;
                f->run(3000);
                CHECK(!f->network.published.empty());
                bool sawOld = false;
                for (const auto& capture : f->network.published) if (capture.eventId == old.eventId) {
                    assertPayload(capture, old, original);
                    sawOld = true;
                }
                CHECK(sawOld && cloudInput(*f, receipt(old.eventId)));
                f->run(1000);
                CHECK(f->store->state().pendingResultCount == 1);
                CHECK(f->store->state().pendingResults[0].eventId == otherId);
                CHECK(nonOutbox(f->store->state()) == evidence);
                CHECK(f->store->state().slot.kind == MotionSlotKind::Intent);
                f->noCommands();
            });
}

void invalidReceipts() {
    for (unsigned variant = 0; variant < 6; ++variant)
        scenario("non-stored, wrong device, wrong epoch and unmatched receipts preserve evidence", [&] {
            auto f = std::make_unique<Fixture>();
            f->add(v4::Source::CloudCommand, 1, true);
            const auto ram = encode(f->store->state()), disk = durable();
            const unsigned writes = fake::count(Op::Set);
            auto r = receipt(f->store->state().pendingResults[0].eventId);
            if (variant == 0 || variant == 1) {
                v4::Message encoded;
                CHECK(encodeCloudReceipt(r, encoded));
                DynamicJsonDocument doc(1024);
                CHECK(!deserializeJson(doc, encoded.payload, encoded.length));
                doc["status"] = variant == 0 ? "queued" : "failed";
                std::string payload;
                serializeJson(doc, payload);
                CloudReceipt decoded;
                CHECK(!decodeCloudReceipt(reinterpret_cast<const uint8_t*>(payload.data()), payload.size(),
                                          pairing().deviceId, decoded));
            } else if (variant == 2) {
                std::strcpy(r.deviceId, "Babytech_other");
                CHECK(!cloudInput(*f, r));
                CHECK(!f->relay->receipt(r) && !f->delivery->receipt(r));
            } else {
                if (variant == 3) r.eventId[4] = 'a';
                if (variant == 4) r.eventId[39] = '9';
                if (variant == 5) r.eventId[37] = 'l';
                CHECK(!f->delivery->receipt(r));
                CHECK(cloudInput(*f, r));
            }
            f->run(1500);
            unchanged(*f, ram, disk, writes);
        });
    for (bool receiverBrain : {false, true}) for (bool oldSender : {false, true})
        scenario("stale boot envelopes cannot publish or delete", [&] {
            auto f = std::make_unique<Fixture>();
            f->add(v4::Source::LocalTouch, 1, true);
            const auto ram = encode(f->store->state()), disk = durable();
            const unsigned writes = fake::count(Op::Set);
            v4::Message message;
            if (receiverBrain) {
                TerminalEvent event;
                CHECK(terminalEventFromSlot(f->store->state().pairing, f->store->state().pendingResults[0], event));
                CHECK(encodeTerminalEvent(f->store->state().pairing, event, message));
                message.senderBoot = oldSender ? 21 : f->motionBoot;
                message.receiverBoot = oldSender ? f->brainBoot : 10;
            } else {
                CHECK(encodeCloudReceipt(receipt(f->store->state().pendingResults[0].eventId), message));
                message.senderBoot = oldSender ? 10 : f->brainBoot;
                message.receiverBoot = oldSender ? f->motionBoot : 21;
            }
            message.messageId = 90000;
            for (size_t offset = 0; offset < message.length;) {
                v4::Frame frame;
                CHECK(v4::fragment(message, offset, frame));
                uint8_t bytes[v4::kMaxFrame];
                const size_t length = v4::encode(frame, bytes, sizeof(bytes));
                CHECK(length);
                for (size_t i = 0; i < length; ++i)
                    (receiverBrain ? f->brain : f->motion).receive(bytes[i], f->now);
                offset += frame.length;
            }
            f->run(500, false);
            CHECK(f->network.attempts.empty() && f->receiptCalls == 0);
            unchanged(*f, ram, disk, writes);
        });
}

void maintenanceAndPending() {
    for (bool onBrain : {false, true})
        scenario("maintenance defers receipt work without losing evidence or latching a gate", [&] {
            auto f = std::make_unique<Fixture>();
            f->add(v4::Source::CloudCommand, 1, false);
            f->run(1000);
            const auto ram = encode(f->store->state()), disk = durable();
            const unsigned writes = fake::count(Op::Set);
            (onBrain ? f->brainMaintenance : f->motionMaintenance) = true;
            CHECK(cloudInput(*f, receipt(f->store->state().pendingResults[0].eventId)));
            f->run(3000);
            CHECK(f->brain.connected(f->now) && f->motion.connected(f->now));
            CHECK(onBrain ? f->receiptCalls == 0 : f->receiptCalls > 0);
            unchanged(*f, ram, disk, writes);
            (onBrain ? f->brainMaintenance : f->motionMaintenance) = false;
            f->run(1000);
            CHECK(f->store->state().pendingResultCount == 0 && fake::count(Op::Set) == writes + 1);
        });
    for (bool onBrain : {false, true})
        scenario("single pending receipt never overwrites another; retry eventually collects second", [&] {
            auto f = std::make_unique<Fixture>();
            f->add(v4::Source::CloudCommand, 1, true);
            f->add(v4::Source::LocalTouch, 1, false);
            const auto a = receipt(f->store->state().pendingResults[0].eventId);
            const auto b = receipt(f->store->state().pendingResults[1].eventId);
            const auto disk = durable();
            const unsigned writes = fake::count(Op::Set);
            if (onBrain) {
                f->brainMaintenance = true;
                CHECK(cloudInput(*f, a));
                CHECK(cloudInput(*f, a));
                CHECK(!cloudInput(*f, b));
                f->run(1500);
                CHECK(durable() == disk);
                f->brainMaintenance = false;
            } else {
                f->motionMaintenance = true;
                CHECK(cloudInput(*f, a));
                f->run(500);
                CHECK(f->receiptCalls > 0);
                CHECK(f->delivery->receipt(a));
                CHECK(!f->delivery->receipt(b));
                CHECK(cloudInput(*f, b));
                f->run(500);
                CHECK(durable() == disk);
                f->motionMaintenance = false;
            }
            f->run(2000);
            CHECK(f->store->state().pendingResultCount == 1);
            CHECK(!std::strcmp(f->store->state().pendingResults[0].eventId, b.eventId));
            CHECK(fake::count(Op::Set) == writes + 1);
            f->network.published.clear();
            f->run(2000);
            CHECK(!f->network.published.empty());
            for (const auto& capture : f->network.published) CHECK(capture.eventId == b.eventId);
            CHECK(cloudInput(*f, b));
            f->run(1000);
            CHECK(f->store->state().pendingResultCount == 0 && fake::count(Op::Set) == writes + 2);
            f->noCommands();
        });
}

void storageFaults() {
    for (bool archived : {false, true}) for (unsigned variant = 0; variant < 6; ++variant)
        scenario("failed/uncertain deletion; archived=" + std::to_string(archived) +
                 " fault=" + std::to_string(variant) + "; reboot uses durable truth", [&] {
            auto f = std::make_unique<Fixture>();
            f->add(v4::Source::LocalTouch, 1, false, archived);
            const auto slot = archived ? f->store->state().pendingResults[0] : f->store->state().slot;
            const auto expected = expectedJson(f->store->state(), slot);
            const auto ram = encode(f->store->state()), disk = durable();
            const bool persisted = variant == 1 || variant >= 3;
            if (variant < 2) fake::fail(Op::Set, fake::count(Op::Set) + 1, ESP_FAIL, variant == 1);
            if (variant == 2 || variant == 3)
                fake::fail(Op::Commit, fake::count(Op::Commit) + 1, ESP_FAIL, variant == 3);
            if (variant == 4) {
                io.durableOnSet = true;
                fake::fail(Op::Commit, fake::count(Op::Commit) + 1);
            }
            // acknowledge does checkCurrent, RW recheck, then fresh RO readback.
            if (variant == 5) fake::fail(Op::OpenRO, fake::count(Op::OpenRO) + 2);
            CHECK(cloudInput(*f, receipt(slot.eventId)));
            f->run(1000);
            CHECK(f->store->faulted() && !f->store->ready());
            CHECK(encode(f->store->state()) == ram);
            CHECK((durable() != disk) == persisted);
            audit();
            const auto calls = io.calls.size();
            f->run(2000);
            CHECK(io.calls.size() == calls);
            f->network.published.clear();
            const auto previousAttempts = f->motion.attempts.size();
            f->motionReset();
            f->run(12000);
            CHECK(f->store->ready() && !f->store->faulted());
            CHECK(fake::count(Op::Set) == 0 && fake::count(Op::Commit) == 0);
            if (persisted) {
                CHECK(f->network.published.empty());
                CHECK(!f->store->state().pendingResultCount && f->store->state().slot.kind == MotionSlotKind::Empty);
            } else {
                requireProgress(*f, !f->network.published.empty(), previousAttempts);
                for (const auto& capture : f->network.published) assertPayload(capture, slot, expected);
                CHECK(cloudInput(*f, receipt(slot.eventId)));
                f->run(1000);
                CHECK(!f->store->state().pendingResultCount && f->store->state().slot.kind == MotionSlotKind::Empty);
            }
            f->noCommands();
        });
}

void recovery() {
    for (const auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch})
        for (bool completed : {false, true}) for (unsigned failure = 0; failure < 4; ++failure)
            scenario("original JSON replay source=" + std::to_string(unsigned(source)) +
                     " completed=" + std::to_string(completed) + " loss/restart=" + std::to_string(failure), [&] {
                auto f = std::make_unique<Fixture>();
                f->add(source, 1, completed);
                const auto slot = f->store->state().pendingResults[0];
                const auto expected = expectedJson(f->store->state(), slot);
                CHECK(f->store->saveContext(context(11, true)) == MotionWrite::Stored);
                const auto ram = encode(f->store->state()), disk = durable();
                unsigned writes = fake::count(Op::Set);
                f->run(2000);
                CHECK(!f->network.published.empty());
                if (failure == 0) {
                    // Stored reached Brain, but all outbound UART receipt bytes
                    // are lost. LinkAck/queue acceptance cannot retire evidence.
                    CHECK(cloudInput(*f, receipt(slot.eventId)));
                    f->run(500, true, true, false);
                } else if (failure == 1) {
                    CHECK(cloudInput(*f, receipt(slot.eventId)));
                    f->brainReset();
                } else if (failure == 2) {
                    f->motionReset();
                    writes = 0;
                } else {
                    f->network.online = false;
                    f->run(2000);
                    CHECK(!messages(f->toMotion.history, v4::Kind::LinkReject).empty());
                    f->network.online = true;
                }
                f->network.published.clear();
                const auto previousAttempts = f->motion.attempts.size();
                f->run(12000);
                requireProgress(*f, !f->network.published.empty(), previousAttempts);
                for (const auto& capture : f->network.published) assertPayload(capture, slot, expected);
                unchanged(*f, ram, disk, writes);
                CHECK(cloudInput(*f, receipt(slot.eventId)));
                f->run(1000);
                CHECK(f->store->state().pendingResultCount == 0);
                f->noCommands();
            });
}

void fairnessAndWrap() {
    for (unsigned mode = 0; mode < 4; ++mode)
        scenario("rotation under rejection/zero write/disconnect/short write mode=" + std::to_string(mode), [&] {
            auto f = std::make_unique<Fixture>();
            for (uint64_t seq = 1; seq <= 4; ++seq)
                f->add(seq % 2 ? v4::Source::LocalTouch : v4::Source::CloudCommand, seq, seq % 2);
            std::set<std::string> expectedIds;
            for (const auto& slot : f->store->state().pendingResults) expectedIds.insert(slot.eventId);
            const auto ram = encode(f->store->state()), disk = durable();
            const unsigned writes = fake::count(Op::Set);
            if (mode == 0) f->network.rejectEvent = *expectedIds.begin();
            if (mode == 1) {
                f->toBrain.zeroWrite = true;
                f->run(2500);
                f->toBrain.zeroWrite = false;
            }
            if (mode == 2) {
                f->run(2500, true, true, true);
                CHECK(!f->brain.connected(f->now) && !f->motion.connected(f->now));
            }
            if (mode == 3) {
                f->toBrain.writeLimit = 7;
                f->toMotion.writeLimit = 7;
            }
            f->run(18000);
            std::set<std::string> attempted, published;
            for (const auto& capture : f->network.attempts) attempted.insert(capture.eventId);
            for (const auto& capture : f->network.published) published.insert(capture.eventId);
            requireProgress(*f, attempted == expectedIds);
            if (mode == 0) expectedIds.erase(f->network.rejectEvent);
            CHECK(published == expectedIds);
            unchanged(*f, ram, disk, writes);
            CHECK(f->brain.connected(f->now) && f->motion.connected(f->now));
        });
    scenario("one-second attempt boundary crosses millis wrap without burst or stall", [] {
        auto f = std::make_unique<Fixture>(true, UINT32_MAX - 800);
        // Isolate the retry clock boundary; telemetry/backpressure fairness is
        // exercised above with the same real link and STATUS enabled.
        f->telemetry = false;
        f->add(v4::Source::CloudCommand, 1, true);
        const auto disk = durable();
        const unsigned writes = fake::count(Op::Set);
        const uint32_t first = f->now;
        f->delivery->poll(first, true);
        f->run(500, false);
        CHECK(f->now < first);
        CHECK(f->network.published.size() == 1);
        const auto terminals = messages(f->toBrain.history, v4::Kind::Terminal).size();
        const auto attempts = f->motion.attempts.size();
        f->delivery->poll(first + 999, true);
        CHECK(f->motion.attempts.size() == attempts);
        f->run(300, false);
        CHECK(messages(f->toBrain.history, v4::Kind::Terminal).size() == terminals);
        f->now = first + 1000;
        f->delivery->poll(f->now, true);
        CHECK(f->motion.attempts.size() == attempts + 1);
        CHECK(f->motion.attempts.back().at == first + 1000);
        f->run(500, false);
        CHECK(f->network.published.size() == 2);
        CHECK(f->network.published[0].originalJson == f->network.published[1].originalJson);
        CHECK(durable() == disk && fake::count(Op::Set) == writes);
        CHECK(cloudInput(*f, receipt(f->store->state().pendingResults[0].eventId)));
        f->run(1000);
        CHECK(f->store->state().pendingResultCount == 0);
        f->noCommands();
    });
}
} // namespace

int main(int argc, char** argv) {
    const std::pair<const char*, void (*)()> groups[] = {
        {"roundtrip", roundtrip}, {"intent-capacity", intentAndCapacity},
        {"active", archivedDuringActive}, {"invalid", invalidReceipts},
        {"pending-maintenance", maintenanceAndPending}, {"storage-faults", storageFaults},
        {"recovery", recovery}, {"fairness-wrap", fairnessAndWrap},
        {"runtime-owner", runtimeOwnership}, {"recovery-owner", recoveryOwnership},
        {"full-queue-runtime", fullQueueRuntime}, {"active-history-faults", activeHistoryFaults}
    };
    bool selected = false;
    for (const auto& group : groups) if (argc == 1 || group.first == std::string(argv[1])) {
        selected = true;
        const unsigned before = scenarios, bad = failures;
        group.second();
        std::printf("%s: %u scenarios, %u failed\n", group.first, scenarios - before, failures - bad);
    }
    if (!selected || argc > 2) {
        std::fprintf(stderr, "Usage: result_delivery [roundtrip|intent-capacity|active|invalid|"
                             "pending-maintenance|storage-faults|recovery|fairness-wrap|"
                             "runtime-owner|recovery-owner|full-queue-runtime|active-history-faults]\n");
        return 2;
    }
    std::printf("%s %u dynamic result-delivery scenarios, %u failures\n",
                failures ? "FAIL" : "PASS", scenarios, failures);
    std::puts("Boundary: real Store/codecs/two ReadOnlyLinks/delivery/Runtime/Recovery/Session/flow; "
              "fake NVS, clock, byte sinks, executor/feedback and Network I/O. "
              "No real Network SDK/broker/Flash/hardware/main E2E.");
    return failures ? 1 : 0;
}
