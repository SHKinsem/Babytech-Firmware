#include "brain_simulation_dispatcher.h"
#include "brain_simulation_mode_guard.h"
#include "FakeProductCrypto.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

using namespace babytech::boardlink;
using namespace babytech::brain;
namespace v4 = babytech::v4;
namespace cloud = babytech::cloud;

namespace {
constexpr char device[] = "Babytech_dispatcher-test";
uint32_t nowMs = 100;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + \
    std::to_string(__LINE__) + ": " #x); } while (false)

template<size_t N> void text(char (&out)[N], const std::string& value) {
    CHECK(value.size() < N);
    std::memset(out, 0, N);
    std::memcpy(out, value.data(), value.size());
}
uint32_t clockNow() { return nowMs; }
v4::Pairing pairing() {
    v4::Pairing p;
    p.role = v4::Role::Brain;
    text(p.deviceId, device);
    text(p.epoch, "0123456789abcdef0123456789abcdef");
    text(p.localPhysicalId, "aabbccddeeff");
    text(p.peerPhysicalId, "112233445566");
    CHECK(v4::validPairing(p));
    return p;
}
ProductContext context() {
    ProductContext c;
    text(c.deviceId, device);
    text(c.babyId, "baby-7");
    text(c.babyName, "Baby \"seven\"\n");
    text(c.formulaBrand, "Formula seven");
    c.profileVersion = 7;
    c.waterMl = 120;
    c.temperatureC = 40;
    c.powderGPer100Ml = 13.123456f;
    CHECK(validProductContext(c));
    return c;
}
struct Ack {
    std::string id, command, session, reason;
    uint64_t sequence;
    bool accepted, queued;
};
struct Event {
    TerminalEvent terminal;
    std::string payload;
    bool queued;
};
struct FreshCheck {
    uint32_t generation, sampledAt, checkedAt;
    uint16_t ttl;
    cloud::Freshness result;
};

// Production freshness and wire codecs; the fake replaces only network I/O.
// resetCommandSession immediately invalidates generation, BEFORE reconnect.
struct FakeNetwork {
    cloud::CloudSession session;
    char token[33]{};
    uint32_t generation = 1;
    unsigned resets = 0, tokenNumber = 0;
    bool connected = false, ackAvailable = true, stopAckAvailable = true, eventAvailable = true;
    std::vector<Ack> acks;
    std::vector<Event> events;
    std::vector<FreshCheck> checks;
    std::function<void(size_t)> beforeCheck;

    FakeNetwork() { reconnect(0); }
    void reconnect(uint32_t at) {
        std::snprintf(token, sizeof(token), "%032x", ++tokenNumber);
        CHECK(session.open(token, generation, at));
        connected = true;
    }
    void resetCommandSession() {
        ++resets;
        ++generation;
        session.close();
        connected = false;
    }
    cloud::Freshness checkFreshness(const char* id, uint32_t gen, uint32_t sample, uint16_t ttl) {
        if (beforeCheck) beforeCheck(checks.size());
        const auto result = !connected ? cloud::Freshness::Disconnected :
            gen != generation ? cloud::Freshness::WrongSession :
            session.check(id, gen, sample, ttl, nowMs);
        checks.push_back({gen, sample, nowMs, ttl, result});
        return result;
    }
    bool publishAck(const char* id, const char* command, uint64_t sequence,
                    const char* originalSession, bool accepted, const char* reason) {
        const bool queued = connected && ackAvailable && (std::strcmp(command, "stop") || stopAckAvailable);
        acks.push_back({id, command, originalSession, reason, sequence, accepted, queued});
        return queued;
    }
    bool publishSimulationEvent(const v4::Pairing& p, const TerminalEvent& terminal) {
        v4::Message wire;
        CHECK(encodeBrainSimulationEvent(p, terminal, wire));
        CHECK(wire.kind == v4::Kind::Terminal);
        CHECK(!wire.senderBoot && !wire.receiverBoot && !wire.messageId);
        const bool queued = connected && eventAvailable;
        events.push_back({terminal, {reinterpret_cast<const char*>(wire.payload), wire.length}, queued});
        return queued;
    }
};
using Dispatcher = BrainSimulationDispatcher<FakeNetwork>;
static_assert(!std::is_copy_constructible<Dispatcher>::value, "one dispatcher owner");
static_assert(BrainSimulation::kResultCapacity == 4, "bounded RAM result reservation");
// No Link, Motion, Store, UART or local-sequence object is supplied to this rig.
struct Rig {
    FakeNetwork net;
    ProductContext ctx = context();
    Dispatcher d;
    explicit Rig(uint32_t duration = 10) : d(pairing(), net, clockNow, duration) {
        d.setContext(&ctx, true, true);
    }
    void enable() {
        CHECK(d.setEnabled(true, false) == SimulationModeResult::Changed);
        CHECK(!net.connected);
        net.reconnect(nowMs);
    }
    CloudCommand command(uint64_t seq = 1, ProductCommand kind = ProductCommand::Prepare) const {
        CloudCommand c;
        text(c.session, net.token);
        c.sampledAtMs = nowMs;
        c.ttlMs = cloud::kCommandTtlMs;
        auto& r = c.request;
        r.source = v4::Source::CloudCommand;
        r.command = kind;
        r.sequence = seq;
        text(r.deviceId, device);
        text(r.commandId, "cloud-" + std::to_string(seq));
        if (kind == ProductCommand::Prepare) {
            text(r.babyId, "baby-7");
            r.profileVersion = 7;
            r.waterMl = 120;
            r.temperatureC = 40;
            r.powderGPer100Ml = 13.123456f;
        } else if (kind == ProductCommand::SetTargetTemp) r.temperatureC = 40;
        CHECK(validProductRequest(r));
        return c;
    }
    CloudStop stop(uint64_t seq = 2) const {
        CloudStop s;
        text(s.deviceId, device);
        text(s.commandId, "stop-" + std::to_string(seq));
        text(s.session, net.token);
        s.sequence = seq;
        s.sampledAtMs = nowMs;
        s.ttlMs = cloud::kCommandTtlMs;
        return s;
    }
    void send(const CloudCommand& c) { d.command(c, net.generation, nowMs); }
    void send(const CloudStop& s) { d.stop(s, net.generation, nowMs); }
    void poll(uint32_t at) { nowMs = at; d.poll(at); }
    void finish() { poll(nowMs + d.durationMs()); CHECK(!d.running()); }
};

void ack(const Ack& actual, const CloudCommand& c, bool accepted, const char* reason,
         const char* command = "prepare") {
    CHECK(actual.id == c.request.commandId && actual.sequence == c.request.sequence);
    CHECK(actual.session == c.session && actual.command == command);
    CHECK(actual.accepted == accepted && actual.reason == reason);
}
void ack(const Ack& actual, const CloudStop& s, bool accepted, const char* reason) {
    CHECK(actual.id == s.commandId && actual.sequence == s.sequence);
    CHECK(actual.session == s.session && actual.command == "stop");
    CHECK(actual.accepted == accepted && actual.reason == reason);
}
CloudReceipt stored(const TerminalEvent& event) {
    CloudReceipt input, decoded;
    text(input.deviceId, event.request.deviceId);
    text(input.eventId, event.eventId);
    v4::Message wire;
    CHECK(encodeCloudReceipt(input, wire));
    CHECK(decodeCloudReceipt(wire.payload, wire.length, device, decoded));
    CHECK(!std::strcmp(input.eventId, decoded.eventId));
    return decoded;
}
void readJson(const std::string& bytes, JsonDocument& doc) {
    CHECK(!deserializeJson(doc, bytes));
}
std::string statusJson(const Rig& r) {
    cloud::SessionSnapshot snapshot;
    text(snapshot.id, r.net.token);
    snapshot.generation = r.net.generation;
    snapshot.uptimeMs = nowMs;
    DynamicJsonDocument doc(8192);
    writeSimulationStatus(doc.to<JsonObject>(), device, "host-test", snapshot, r.d.status());
    char bytes[4096];
    CHECK(encodeStatusJson(doc, bytes, sizeof(bytes)));
    return bytes;
}

void ownership() {
    Rig r;
    CHECK(!r.d.enabled() && !r.d.running() && !r.d.status().commandsEnabled);
    CHECK(r.d.durationMs() == 10);
    r.send(r.command());
    CHECK(r.net.acks.empty() && r.net.checks.empty());
    r.enable();
    auto local = r.command();
    local.request.source = v4::Source::LocalTouch;
    CHECK(makeLocalCommandId(pairing(), 1, local.request.commandId));
    CHECK(validProductRequest(local.request));
    r.send(local);
    auto foreign = r.command();
    text(foreign.request.deviceId, "Other_device");
    r.send(foreign);
    auto invalid = r.command();
    invalid.request.waterMl = 0;
    CHECK(!validProductRequest(invalid.request));
    r.send(invalid);
    auto otherStop = r.stop();
    text(otherStop.deviceId, "Other_device");
    r.send(otherStop);
    CHECK(r.net.acks.empty() && r.net.checks.empty() && !r.d.running());
    const auto valid = r.command();
    r.send(valid);
    ack(r.net.acks.back(), valid, true, "accepted");
    CHECK(r.d.running());
}

void modes() {
    Rig r;
    CHECK(r.d.setEnabled(true, true) == SimulationModeResult::Busy);
    CHECK(!r.d.enabled() && r.net.resets == 0);
    CHECK(r.d.setEnabled(false, true) == SimulationModeResult::Unchanged);
    r.enable();
    const auto gen = r.net.generation;
    const std::string token = r.net.token;
    CHECK(r.net.resets == 1);
    CHECK(r.d.setEnabled(true, true) == SimulationModeResult::Unchanged);
    CHECK(r.net.generation == gen && token == r.net.token && r.net.connected);
    r.send(r.command());
    CHECK(r.d.setEnabled(false, false) == SimulationModeResult::Busy);
    CHECK(r.d.running() && r.net.resets == 1 && r.net.generation == gen);
    r.finish();
    CHECK(r.d.setEnabled(false, true) == SimulationModeResult::Busy);
    CHECK(r.d.setEnabled(false, false) == SimulationModeResult::Changed);
    CHECK(r.net.resets == 2 && r.net.generation == gen + 1 && !r.net.connected);
    CHECK(!r.d.status().complete && r.d.resultCount() == 1);
    CHECK(r.d.setEnabled(false, false) == SimulationModeResult::Unchanged);
    CHECK(r.net.resets == 2);
}

void context_admission() {
    for (unsigned fault = 0; fault < 10; ++fault) {
        Rig r;
        r.enable();
        auto c = r.command(2);
        switch (fault) {
            case 0: r.d.setContext(&r.ctx, true, false); break;
            case 1: r.d.setContext(&r.ctx, false, true); break;
            case 2: r.d.setContext(nullptr, false, true); break;
            case 3: r.ctx.cleared = true; break;
            case 4: ++c.request.profileVersion; break;
            case 5: text(c.request.babyId, "other-baby"); break;
            case 6: c.request.powderGPer100Ml = 14; break;
            case 7: r.d.setContext(nullptr, true, true); break;
            case 8:
                text(r.ctx.deviceId, "Other_device");
                r.d.setContext(&r.ctx, true, true);
                break;
            case 9:
                r.ctx.profileVersion = 0;
                r.d.setContext(&r.ctx, true, true);
                break;
        }
        const char* reason = fault == 0 ? "integration_not_ready" : "context_required";
        r.send(c);
        ack(r.net.acks.back(), c, false, reason);
        CHECK(!r.d.running() && !r.d.resultCount());
        r.ctx = context();
        r.d.setContext(&r.ctx, true, true);
        r.send(c);
        ack(r.net.acks.back(), c, false, reason);
        auto corrected = r.command(2);
        r.send(corrected);
        CHECK(!r.d.running());
        r.send(r.command(1));
        CHECK(!r.d.running());
        const auto newer = r.command(3);
        r.send(newer);
        ack(r.net.acks.back(), newer, true, "accepted");
        r.finish();
        CHECK(r.d.resultCount() == 1);
    }
}

void unsupported() {
    const std::array<ProductCommand, 4> kinds = {ProductCommand::Clean, ProductCommand::SetTargetTemp,
        ProductCommand::ResetError, ProductCommand::CheckFirmwareUpdate};
    const std::array<const char*, 4> names = {"clean", "set_target_temp", "reset_error", "check_firmware_update"};
    for (size_t i = 0; i < kinds.size(); ++i) {
        Rig r;
        r.enable();
        const auto c = r.command(2, kinds[i]);
        r.send(c);
        ack(r.net.acks.back(), c, false, "unsupported_command", names[i]);
        r.send(c);
        ack(r.net.acks.back(), c, false, "unsupported_command", names[i]);
        r.send(r.command(1));
        r.send(r.command(2));
        CHECK(!r.d.running() && !r.d.resultCount());
        r.send(r.command(3));
        CHECK(r.d.running());
    }
}

void frozen_event() {
    Rig r;
    r.enable();
    auto c = r.command(42);
    const auto original = c;
    r.send(c);
    CHECK(r.d.running() && r.d.status().request);
    c.request.waterMl = 300;
    c.request.temperatureC = 50;
    c.request.profileVersion = 8;
    text(c.request.babyId, "mutated-request");
    r.ctx = context();
    text(r.ctx.babyId, "updated-cache");
    r.ctx.profileVersion = 8;
    r.d.setContext(&r.ctx, false, false);
    CHECK(sameProductRequest(*r.d.status().request, original.request));
    CHECK(r.d.status().context && !std::strcmp(r.d.status().context->babyId, original.request.babyId));
    CHECK(r.d.status().context->profileVersion == original.request.profileVersion);
    DynamicJsonDocument activeStatus(8192);
    readJson(statusJson(r), activeStatus);
    CHECK(std::string(activeStatus["feeding_context_baby_id"].as<const char*>()) == original.request.babyId);
    CHECK(activeStatus["feeding_context_profile_version"].as<unsigned>() == original.request.profileVersion);
    CHECK(activeStatus["target_temp"].as<unsigned>() == original.request.temperatureC);
    r.poll(109);
    CHECK(r.d.running() && r.net.events.empty());
    r.poll(117);
    CHECK(!r.d.running() && r.d.resultCount() == 1 && r.net.events.size() == 1);
    const auto frozen = r.net.events.back();
    CHECK(sameProductRequest(frozen.terminal.request, original.request));
    CHECK(frozen.terminal.completed && frozen.terminal.uptimeMs == 117);
    uint8_t originalDigest[kProductDigestSize], terminalDigest[kProductDigestSize];
    const auto digestCalls = fake_product_crypto::calls;
    CHECK(requestDigest(original.request, originalDigest));
    CHECK(requestDigest(frozen.terminal.request, terminalDigest));
    CHECK(!std::memcmp(originalDigest, terminalDigest, sizeof(originalDigest)));
    CHECK(fake_product_crypto::calls == digestCalls + 2); // Exercise each system SHA API variant.
    DynamicJsonDocument doc(8192);
    readJson(frozen.payload, doc);
    CHECK(std::string(doc["execution_mode"].as<const char*>()) == "brain_simulation");
    CHECK(std::string(doc["event"].as<const char*>()) == "feeding_completed");
    CHECK(std::string(doc["command_seq"].as<const char*>()) == "42");
    CHECK(std::string(doc["command_id"].as<const char*>()) == original.request.commandId);
    CHECK(std::string(doc["source"].as<const char*>()) == "cloud_command");
    CHECK(std::string(doc["device_id"].as<const char*>()) == device);
    CHECK(std::string(doc["baby_id"].as<const char*>()) == original.request.babyId);
    CHECK(doc["feeding_context_profile_version"].as<unsigned>() == 7);
    CHECK(doc["water_ml"].as<unsigned>() == 120 && doc["temp"].as<unsigned>() == 40);
    CHECK(doc["powder_g_per_100ml"].as<float>() == original.request.powderGPer100Ml);
    CHECK(doc["target_powder_g"].as<float>() == 15.7f);
    CHECK(doc["uptime_ms"].as<unsigned>() == 117);
    CHECK(doc.containsKey("dispensed_water_ml") && doc["dispensed_water_ml"].isNull());
    CHECK(std::string(doc["water_delivery_basis"].as<const char*>()) == "estimated_turns");
    r.poll(366);
    CHECK(r.net.events.size() == 1);
    r.poll(367);
    CHECK(r.net.events.size() == 2 && r.net.events.back().payload == frozen.payload);
    CHECK(r.d.resultCount() == 1); // Queueing is not a stored receipt.
}

void stop_failure() {
    Rig r;
    r.enable();
    const auto c = r.command();
    r.send(c);
    nowMs = 110; // Stop at the deadline, before poll, must still record failure.
    const auto s = r.stop();
    r.send(s);
    ack(r.net.acks.back(), s, true, "accepted");
    CHECK(!r.d.running() && !r.d.status().complete && r.d.resultCount() == 1);
    r.poll(nowMs);
    const auto event = r.net.events.back();
    CHECK(!event.terminal.completed && event.terminal.uptimeMs == 110);
    CHECK(!std::strcmp(event.terminal.reason, "stopped"));
    CHECK(!std::strcmp(event.terminal.errorCode, "E_STOPPED"));
    CHECK(sameProductRequest(event.terminal.request, c.request));
    DynamicJsonDocument doc(8192);
    readJson(event.payload, doc);
    CHECK(std::string(doc["event"].as<const char*>()) == "feeding_failed");
    r.send(s);
    ack(r.net.acks.back(), s, true, "accepted");
    CHECK(r.d.resultCount() == 1);
    const auto idle = r.stop(3);
    r.send(idle);
    ack(r.net.acks.back(), idle, true, "already_idle");
    r.poll(360);
    CHECK(r.net.events.back().payload == event.payload && r.d.resultCount() == 1);
    CHECK(!r.d.status().complete && r.d.status().canStart);
}

void freshness() {
    for (unsigned fault = 0; fault < 5; ++fault) {
        Rig r;
        r.enable();
        auto c = r.command(2);
        const auto gen = r.net.generation;
        if (fault == 0) text(c.session, "ffffffffffffffffffffffffffffffff");
        if (fault == 1) c.ttlMs = 4999;
        if (fault == 2) c.ttlMs = 0;
        if (fault == 3) r.net.connected = false;
        r.d.command(c, fault == 4 ? gen - 1 : gen, 0);
        CHECK(!r.d.running() && r.net.acks.empty() && !r.d.resultCount());
        r.net.connected = true;
        const auto valid = r.command(2);
        r.send(valid); // Unauthenticated transport did not consume a decision.
        ack(r.net.acks.back(), valid, true, "accepted");
        CHECK(r.d.running());
    }
    for (unsigned fault = 0; fault < 3; ++fault) {
        Rig r;
        r.enable();
        auto c = r.command(2);
        if (fault == 0) nowMs = c.sampledAtMs + 5001;
        if (fault == 1) c.sampledAtMs = nowMs + 1;
        if (fault == 2) c.sampledAtMs = 99; // Session opened at 100.
        r.d.command(c, r.net.generation, c.sampledAtMs); // Callback time is not freshness.
        ack(r.net.acks.back(), c, false, "request_expired");
        CHECK(!r.d.running());
        c.sampledAtMs = nowMs;
        r.send(c);
        ack(r.net.acks.back(), c, false, "request_expired");
        r.send(r.command(1));
        CHECK(!r.d.running());
        r.send(r.command(3));
        CHECK(r.d.running());
    }
    Rig boundary;
    boundary.enable();
    const auto c = boundary.command();
    nowMs += 5000;
    boundary.send(c);
    ack(boundary.net.acks.back(), c, true, "accepted");
    CHECK(boundary.d.running());
}

void freshness_wrap() {
    nowMs = UINT32_MAX - 100;
    Rig r;
    r.enable();
    const auto c = r.command();
    nowMs = 4899; // Exactly 5000 ms since sample across wrap.
    r.send(c);
    CHECK(r.d.running());
    r.finish();
    CHECK(r.net.events.back().terminal.uptimeMs == 4909);
}

void admission_recheck() {
    for (unsigned fault = 0; fault < 3; ++fault) {
        Rig r;
        r.enable();
        auto c = r.command(2);
        const auto original = c;
        r.net.beforeCheck = [&](size_t index) {
            if (index != 1) return;
            if (fault == 0) nowMs = c.sampledAtMs + 5001;
            if (fault == 1) r.net.resetCommandSession();
            if (fault == 2) r.net.connected = false;
        };
        r.send(c);
        CHECK(r.net.checks.size() == 2 && !r.d.running() && !r.d.resultCount());
        ack(r.net.acks.back(), c, false, "request_expired");
        r.net.beforeCheck = {};
        if (fault == 1) r.net.reconnect(nowMs);
        r.net.connected = true;
        text(c.session, r.net.token);
        c.sampledAtMs = nowMs;
        r.poll(nowMs);
        r.send(c);
        CHECK(!r.d.running());
        ack(r.net.acks.back(), original, false, "request_expired");
        r.send(r.command(3));
        CHECK(r.d.running());
    }
}

void stop_freshness() {
    for (unsigned fault = 0; fault < 5; ++fault) {
        Rig r;
        r.enable();
        r.send(r.command());
        auto s = r.stop();
        if (fault == 0) text(s.session, "ffffffffffffffffffffffffffffffff");
        if (fault == 1) s.ttlMs = 4999;
        if (fault == 2) nowMs += 5001;
        if (fault == 3) {
            const auto first = r.net.checks.size();
            r.net.beforeCheck = [first, s](size_t index) {
                if (index == first + 1) nowMs = s.sampledAtMs + 5001;
            };
        }
        const size_t count = r.net.acks.size();
        r.d.stop(s, fault == 4 ? r.net.generation - 1 : r.net.generation, 0);
        CHECK(r.d.running() && !r.d.resultCount());
        if (fault == 2 || fault == 3) {
            ack(r.net.acks.back(), s, false, "request_expired");
            r.net.beforeCheck = {};
            s.sampledAtMs = nowMs;
            r.send(s);
            ack(r.net.acks.back(), s, false, "request_expired");
            CHECK(r.d.running());
        } else CHECK(r.net.acks.size() == count);
        const auto newer = r.stop(3);
        r.send(newer);
        CHECK(!r.d.running() && r.d.resultCount() == 1);
        ack(r.net.acks.back(), newer, true, "accepted");
    }
}

void ack_retry() {
    Rig r;
    r.enable();
    r.net.ackAvailable = false;
    const auto c = r.command();
    r.send(c);
    CHECK(r.d.running() && !r.net.acks.back().queued);
    r.send(c);
    ack(r.net.acks.back(), c, true, "accepted");
    r.net.resetCommandSession();
    r.finish();
    CHECK(r.d.resultCount() == 1 && !r.net.acks.back().queued);
    r.net.reconnect(nowMs);
    CHECK(std::strcmp(r.net.token, c.session));
    r.net.ackAvailable = true;
    r.poll(nowMs);
    ack(r.net.acks.back(), c, true, "accepted");
    CHECK(r.net.acks.back().queued && r.d.status().canStart);
    const auto count = r.net.acks.size();
    r.poll(nowMs + 1);
    CHECK(r.net.acks.size() == count && r.d.resultCount() == 1 && !r.d.running());
    auto retry = c;
    text(retry.session, r.net.token);
    retry.sampledAtMs = nowMs;
    r.send(retry);
    ack(r.net.acks.back(), c, true, "accepted");
    CHECK(!r.d.running() && r.d.resultCount() == 1);
}

void ack_pending() {
    Rig r;
    r.enable();
    r.net.ackAvailable = false;
    const auto a = r.command(1), b = r.command(2), c = r.command(3);
    r.send(a);
    r.send(b);
    ack(r.net.acks.back(), b, false, "busy");
    r.finish();
    CHECK(!r.d.running() && r.d.resultCount() == 1 && !r.d.status().canStart);
    r.send(b);
    ack(r.net.acks.back(), b, false, "busy");
    r.send(c);
    ack(r.net.acks.back(), c, false, "busy");
    r.net.ackAvailable = true;
    r.poll(nowMs);
    ack(r.net.acks.back(), a, true, "accepted");
    CHECK(r.net.acks.back().queued && r.d.status().canStart);
    r.send(c);
    ack(r.net.acks.back(), c, false, "busy");
    r.send(b); // Older rejection may be silent, but must not become an action.
    CHECK(!r.d.running() && r.d.resultCount() == 1);
    const auto newer = r.command(4);
    r.send(newer);
    ack(r.net.acks.back(), newer, true, "accepted");
    CHECK(r.d.running() && r.d.status().request->sequence == 4);
    r.finish();
    CHECK(r.d.resultCount() == 2);
}

void rejected_ack_pending() {
    Rig r;
    r.enable();
    r.net.ackAvailable = false;
    r.d.setContext(&r.ctx, false, true);
    const auto rejected = r.command(2);
    r.send(rejected);
    ack(r.net.acks.back(), rejected, false, "context_required");
    r.d.setContext(&r.ctx, true, true);
    const auto blocked = r.command(3);
    r.send(blocked);
    ack(r.net.acks.back(), blocked, false, "busy");
    r.poll(101);
    ack(r.net.acks.back(), rejected, false, "context_required");
    CHECK(!r.d.running() && !r.d.resultCount());
    r.net.ackAvailable = true;
    r.poll(102);
    ack(r.net.acks.back(), rejected, false, "context_required");
    r.send(blocked);
    ack(r.net.acks.back(), blocked, false, "busy");
    r.send(rejected);
    r.send(r.command(1));
    CHECK(!r.d.running());
    r.send(r.command(4));
    CHECK(r.d.running());
}

void rejected_id_reuse() {
    std::string counterexamples;
    const std::array<const char*, 4> paths = {"ordinary Busy", "ACK pending", "context rejection", "expiry rejection"};
    const std::array<const char*, 4> reasons = {"busy", "busy", "context_required", "request_expired"};
    for (unsigned fault = 0; fault < paths.size(); ++fault) {
        nowMs = 100;
        Rig r;
        r.enable();
        r.net.ackAvailable = fault != 1;
        if (fault < 2) r.send(r.command(1));
        if (fault == 2) r.d.setContext(&r.ctx, false, true);
        const auto rejected = r.command(2);
        if (fault == 3) nowMs += 5001;
        r.send(rejected);
        ack(r.net.acks.back(), rejected, false, reasons[fault]);
        if (fault < 2) r.finish();
        r.d.setContext(&r.ctx, true, true);
        r.net.ackAvailable = true;
        r.poll(nowMs); // Drain the original ACK, without executing the rejected request.
        const size_t originalResults = fault < 2 ? 1 : 0;
        CHECK(!r.d.running() && r.d.resultCount() == originalResults);
        auto reused = r.command(3);
        text(reused.request.commandId, rejected.request.commandId);
        CHECK(validProductRequest(reused.request));
        r.send(reused);
        const auto actual = r.net.acks.back();
        if (actual.accepted || actual.reason != "request_conflict") {
            counterexamples += std::string(paths[fault]) +
                ": seq=2 cloud-2 rejected, seq=3 reuses cloud-2; expected Conflict, got " +
                actual.reason + (r.d.running() ? " and execution; " : "; ");
            if (r.d.running()) r.finish();
        } else {
            ack(actual, reused, false, "request_conflict");
            CHECK(!r.d.running() && r.d.resultCount() == originalResults);
        }
        r.send(reused);
        CHECK(!r.d.running()); // Even an erroneous acceptance must not execute twice.
        const auto newer = r.command(4);
        r.send(newer);
        ack(r.net.acks.back(), newer, true, "accepted");
        CHECK(r.d.running() && r.d.status().request->sequence == 4);
    }
    if (!counterexamples.empty()) throw std::runtime_error(counterexamples);
}

void busy_duplicate() {
    Rig r;
    r.enable();
    const auto a = r.command(), b = r.command(2);
    r.send(a);
    r.send(b);
    ack(r.net.acks.back(), b, false, "busy");
    r.send(a);
    ack(r.net.acks.back(), a, true, "accepted");
    auto conflict = a;
    conflict.request.waterMl = 150;
    r.send(conflict);
    CHECK(r.d.status().request->waterMl == 120);
    r.finish();
    const auto receipt = stored(r.net.events.back().terminal);
    r.d.receipt(receipt);
    CHECK(!r.d.resultCount());
    r.send(b);
    ack(r.net.acks.back(), b, false, "busy");
    r.send(a);
    CHECK(!r.d.running() && !r.d.resultCount());
    r.send(r.command(3));
    CHECK(r.d.running());
}

void stop_ack_pending() {
    Rig r;
    r.enable();
    const auto c = r.command();
    r.send(c);
    r.net.stopAckAvailable = false;
    const auto s = r.stop();
    r.send(s);
    ack(r.net.acks.back(), s, true, "accepted");
    CHECK(!r.net.acks.back().queued && !r.d.running() && r.d.resultCount() == 1);
    CHECK(r.d.status().canStart); // Informational Stop ACK does not gate the next bottle.
    const auto next = r.command(3);
    r.send(next);
    CHECK(r.d.running());
    r.poll(101);
    ack(r.net.acks.back(), s, true, "accepted");
    CHECK(r.d.status().request->sequence == 3 && r.d.resultCount() == 1);
    r.net.resetCommandSession();
    r.net.reconnect(nowMs);
    r.net.stopAckAvailable = true;
    r.poll(102);
    ack(r.net.acks.back(), s, true, "accepted");
    CHECK(r.net.acks.back().queued && r.d.running());
    const auto count = r.net.acks.size();
    r.poll(103);
    CHECK(r.net.acks.size() == count && r.d.status().request->sequence == 3);
    r.finish();
    CHECK(r.d.resultCount() == 2 && !r.d.running());
}

void full_receipts() {
    Rig r;
    r.enable();
    std::array<Event, 4> frozen;
    for (unsigned i = 0; i < 4; ++i) {
        const auto c = r.command(i + 1);
        r.send(c);
        CHECK(r.d.running() && r.d.resultCount() == i);
        r.finish();
        if (i) r.poll(nowMs + 250);
        // The publisher is round-robin; locate the newly completed identity.
        for (unsigned tries = 0; r.net.events.back().terminal.request.sequence != i + 1 && tries < 4; ++tries)
            r.poll(nowMs + 250);
        CHECK(r.net.events.back().terminal.request.sequence == i + 1);
        frozen[i] = r.net.events.back();
        CHECK(r.d.resultCount() == i + 1);
    }
    CHECK(!r.d.status().canStart);
    const auto full = r.command(5);
    r.send(full);
    ack(r.net.acks.back(), full, false, "result_queue_full");
    r.d.receipt(stored(frozen[2].terminal));
    CHECK(r.d.resultCount() == 3 && r.d.status().canStart);
    r.d.receipt(stored(frozen[2].terminal));
    CHECK(r.d.resultCount() == 3);
    for (unsigned i = 0; i < 3; ++i) {
        r.poll(nowMs + 250);
        const auto& e = r.net.events.back();
        CHECK(e.terminal.request.sequence != 3);
        CHECK(e.payload == frozen[e.terminal.request.sequence - 1].payload);
    }
    r.send(full);
    ack(r.net.acks.back(), full, false, "result_queue_full");
    CHECK(!r.d.running());
    const auto newer = r.command(6);
    r.send(newer);
    CHECK(r.d.running() && r.d.resultCount() == 3); // Fourth slot reserved for Stop.
    TerminalEvent active;
    active.request = newer.request;
    CHECK(makeProductEventId(pairing(), v4::Source::CloudCommand, 6, active.eventId));
    r.d.receipt(stored(active));
    CHECK(r.d.running() && r.d.resultCount() == 3);
    r.send(r.stop(7));
    CHECK(!r.d.running() && r.d.resultCount() == 4);
    r.d.receipt(stored(frozen[0].terminal));
    r.d.receipt(stored(frozen[3].terminal));
    r.d.receipt(stored(frozen[1].terminal));
    CHECK(r.d.resultCount() == 1);
    r.poll(nowMs + 250);
    CHECK(r.net.events.back().terminal.request.sequence == 6 && !r.net.events.back().terminal.completed);
    r.d.receipt(stored(r.net.events.back().terminal));
    CHECK(!r.d.resultCount() && r.d.status().canStart);
}

void receipts_off() {
    Rig r;
    r.enable();
    r.send(r.command());
    r.finish();
    const auto first = r.net.events.back();
    r.send(r.command(2));
    r.finish();
    r.poll(nowMs + 250);
    r.poll(nowMs + 250);
    CHECK(r.d.resultCount() == 2);
    const auto secondFrozen = r.net.events.back();
    CHECK(secondFrozen.terminal.request.sequence == 2);
    CHECK(r.d.setEnabled(false, false) == SimulationModeResult::Changed);
    CHECK(!r.net.connected);
    r.poll(nowMs + 250);
    CHECK(!r.net.events.back().queued && r.d.resultCount() == 2);
    r.net.reconnect(nowMs);
    r.net.eventAvailable = false;
    r.poll(nowMs + 250);
    CHECK(!r.net.events.back().queued && r.d.resultCount() == 2);
    r.net.eventAvailable = true;
    const size_t before = r.net.events.size();
    r.poll(nowMs + 249);
    CHECK(r.net.events.size() == before);
    r.poll(nowMs + 1);
    r.poll(nowMs + 250);
    CHECK(r.net.events.size() == before + 2 && r.d.resultCount() == 2);
    CHECK(r.net.events[before].terminal.request.sequence != r.net.events.back().terminal.request.sequence);
    const auto second = r.net.events[before].terminal.request.sequence == 2 ?
        r.net.events[before] : r.net.events.back();
    CHECK(second.payload == secondFrozen.payload);
    for (const auto& e : {r.net.events[before], r.net.events.back()})
        CHECK(e.payload == (e.terminal.request.sequence == 1 ? first.payload : second.payload));
    CloudReceipt wrong = stored(first.terminal);
    text(wrong.deviceId, "Other_device");
    r.d.receipt(wrong);
    wrong = stored(first.terminal);
    wrong.eventId[4] = wrong.eventId[4] == 'f' ? 'e' : 'f';
    r.d.receipt(wrong);
    CHECK(r.d.resultCount() == 2);
    const auto valid = stored(second.terminal);
    const std::string invalid = "{\"device_id\":\"" + std::string(device) +
        "\",\"event_id\":\"" + valid.eventId + "\",\"status\":\"queued\"}";
    CloudReceipt decoded = valid;
    CHECK(!decodeCloudReceipt(reinterpret_cast<const uint8_t*>(invalid.data()), invalid.size(), device, decoded));
    CHECK(r.d.resultCount() == 2); // Failed decode must not enter receipt API.
    r.d.receipt(valid);
    CHECK(r.d.resultCount() == 1);
    r.poll(nowMs + 250);
    CHECK(r.net.events.back().payload == first.payload);
    r.d.receipt(stored(first.terminal));
    CHECK(!r.d.resultCount() && !r.d.enabled() && !r.d.status().canStart);
}

void delayed_old_mode() {
    Rig r;
    r.enable();
    const auto delayed = r.command(50);
    const uint32_t oldGeneration = r.net.generation;
    CHECK(r.d.setEnabled(false, false) == SimulationModeResult::Changed);
    CHECK(r.net.checkFreshness(delayed.session, oldGeneration, delayed.sampledAtMs, delayed.ttlMs)
          != cloud::Freshness::Current);
    r.d.command(delayed, oldGeneration, nowMs);
    CHECK(r.net.acks.empty() && !r.d.running());
    r.net.reconnect(nowMs);
    CHECK(r.net.checkFreshness(delayed.session, oldGeneration, delayed.sampledAtMs, delayed.ttlMs)
          == cloud::Freshness::WrongSession);
    r.enable();
    r.d.command(delayed, oldGeneration, nowMs);
    CHECK(r.net.acks.empty() && !r.d.running());
    r.send(r.command(1)); // Old-mode traffic did not raise simulation highwater.
    CHECK(r.d.running());
}

void status_pulse() {
    Rig r;
    r.enable();
    for (unsigned phase = 0; phase < 3; ++phase) {
        if (phase == 1) r.send(r.command());
        if (phase == 2) r.finish();
        DynamicJsonDocument doc(8192);
        readJson(statusJson(r), doc);
        CHECK(std::string(doc["hardware_profile"].as<const char*>()) == "simulation");
        CHECK(std::string(doc["firmware_mode"].as<const char*>()) == "cloud");
        CHECK(std::string(doc["actuator_issue"].as<const char*>()) == "brain_simulation");
        CHECK(!doc["motion_connected"].as<bool>() && doc["thermal_simulated"].as<bool>());
        CHECK(doc["motion_status_stale"].as<bool>() && doc["commands_enabled"].as<bool>());
        CHECK(doc["is_preparing"].as<bool>() == (phase == 1));
        CHECK(std::string(doc["progress"].as<const char*>()) == (phase == 1 ? "mixing" : phase == 2 ? "complete" : "ready"));
        CHECK(std::string(doc["bottle_clamp_status"].as<const char*>()) == "unknown");
        CHECK(std::string(doc["water_status"].as<const char*>()) == "unknown");
        CHECK(std::string(doc["powder_status"].as<const char*>()) == "unknown");
        for (const char* key : {"measured_water_temp", "water_temp", "low_water", "water_remained",
                               "powder_remained", "bottle_present_at_load_position", "cap_hall_detected", "dispensed_water_ml"})
            CHECK(doc.containsKey(key) && doc[key].isNull());
        for (const char* key : {"is_water_ready", "is_heating", "is_cooling", "actuator_operational",
                               "actuator_config_valid", "actuator_bus_healthy", "actuator_position_referenced",
                               "bottle_presence_sensor_enabled", "bottle_state_valid"})
            CHECK(doc.containsKey(key) && !doc[key].as<bool>());
        CHECK(std::string(doc["feeding_context_baby_name"].as<const char*>()) == r.ctx.babyName);
    }
    const auto completedAt = nowMs;
    r.poll(completedAt + 1999);
    CHECK(r.d.status().complete && r.d.status().canStart && r.d.resultCount() == 1);
    r.poll(completedAt + 2000);
    CHECK(!r.d.status().complete && r.d.status().canStart);
    r.send(r.command(2));
    CHECK(r.d.running() && !r.d.status().complete && r.d.resultCount() == 1);
    r.finish();
    CHECK(r.d.status().complete && r.d.resultCount() == 2);
    r.send(r.command(3)); // Next bottle need not wait for pulse or prior stored.
    CHECK(r.d.running() && !r.d.status().complete && r.d.resultCount() == 2);
}

CommandResult typedAcceptance(v4::Source source, uint64_t seq, bool accepted = true) {
    CommandResult input, decoded;
    input.source = source;
    input.sequence = seq;
    input.accepted = accepted;
    if (source == v4::Source::LocalTouch) CHECK(makeLocalCommandId(pairing(), seq, input.commandId));
    else text(input.commandId, "real-command-" + std::to_string(seq));
    text(input.reason, accepted ? "accepted" : "busy");
    v4::Message wire;
    CHECK(encodeCommandResult(input, wire) && wire.kind == v4::Kind::CommandResult);
    CHECK(decodeCommandResult(wire, decoded)); // Acceptance, not a TERMINAL message.
    CHECK(decoded.source == source && decoded.sequence == seq && decoded.accepted == accepted);
    return decoded;
}

Status typedStatus(const Status& input) {
    Status decoded;
    v4::Message wire;
    CHECK(encodeStatus(input, wire) && wire.kind == v4::Kind::Status);
    CHECK(decodeStatus(wire, decoded));
    CHECK(!std::strcmp(decoded.cloudWatermark, input.cloudWatermark));
    CHECK(!std::strcmp(decoded.localWatermark, input.localWatermark));
    return decoded;
}

ProductRequest typedGuardRequest(v4::Source source, uint64_t seq, ProductCommand kind) {
    CommandMessage input, decoded;
    auto& request = input.request;
    request.source = source;
    request.sequence = seq;
    request.command = kind;
    text(request.deviceId, device);
    if (source == v4::Source::LocalTouch) CHECK(makeLocalCommandId(pairing(), seq, request.commandId));
    else text(request.commandId, "real-command-" + std::to_string(seq));
    if (kind == ProductCommand::Prepare) {
        const auto ctx = context();
        text(request.babyId, ctx.babyId);
        request.profileVersion = ctx.profileVersion;
        request.waterMl = ctx.waterMl;
        request.temperatureC = ctx.temperatureC;
        request.powderGPer100Ml = ctx.powderGPer100Ml;
    } else if (kind == ProductCommand::SetTargetTemp) request.temperatureC = 40;
    CHECK(validProductRequest(request));
    input.remainingTtlMs = 1000;
    v4::Message wire;
    CHECK(encodeCommand(input, wire) && decodeCommand(wire, decoded));
    CHECK(sameProductRequest(request, decoded.request));
    return decoded.request;
}

void mode_guard_command_filter() {
    const std::array<ProductCommand, 3> motion = {ProductCommand::Prepare, ProductCommand::Initialize, ProductCommand::Clean};
    const std::array<ProductCommand, 3> nonmotion = {ProductCommand::SetTargetTemp, ProductCommand::ResetError,
                                                   ProductCommand::CheckFirmwareUpdate};
    // Request callbacks are already matched/accepted by their owners. This
    // matrix tests guard semantics, not query-owner matching or NVS clearing.
    for (unsigned entry = 0; entry < 2; ++entry) {
        const auto observe = [entry](BrainSimulationModeGuard& gate, const ProductRequest& request, uint32_t at) {
            if (entry == 0)
                gate.observeAccepted(typedAcceptance(request.source, request.sequence), at, request.command);
            else gate.observeAccepted(request, at);
        };
        for (const auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch}) {
            for (const auto kind : motion) {
                if (kind == ProductCommand::Initialize && source == v4::Source::CloudCommand) continue;
                const auto acceptedMotion = typedGuardRequest(source, 10, kind);
                for (const auto nonmotionKind : nonmotion) {
                    for (const auto interferingSource : {v4::Source::CloudCommand, v4::Source::LocalTouch}) {
                        BrainSimulationModeGuard gate;
                        const auto highNonmotion = typedGuardRequest(interferingSource, 1000, nonmotionKind);
                        observe(gate, highNonmotion, 90);
                        CHECK(!gate.unresolved(nullptr, false, 0, 90)); // Nonmotion alone cannot arm.
                        auto callbackRequest = acceptedMotion;
                        observe(gate, callbackRequest, 100);
                        callbackRequest = {}; // Owner may clear its pending request immediately after callback.
                        CHECK(gate.unresolved(nullptr, false, 0, 100)); // Ignored high seq cannot consume motion highwater.

                        Status oldIdle;
                        oldIdle.stationary = true;
                        text(oldIdle.cloudWatermark, source == v4::Source::CloudCommand ? "9" : "1000");
                        text(oldIdle.localWatermark, source == v4::Source::LocalTouch ? "9" : "1000");
                        const auto old = typedStatus(oldIdle);
                        observe(gate, highNonmotion, 110);
                        observe(gate, typedGuardRequest(interferingSource, 1, nonmotionKind), 111);
                        CHECK(gate.unresolved(nullptr, false, 0, 111)); // Cannot clear the pending motion guard.
                        CHECK(gate.unresolved(&old, true, 101, 111)); // Cannot retarget it to the other source/low seq.
                        gate.observeAccepted(typedAcceptance(source, 1001, false), 112, kind);
                        CHECK(gate.unresolved(nullptr, false, 0, 112));

                        auto matchingIdle = oldIdle;
                        if (source == v4::Source::CloudCommand) text(matchingIdle.cloudWatermark, "10");
                        else text(matchingIdle.localWatermark, "10");
                        const auto matching = typedStatus(matchingIdle);
                        CHECK(!gate.unresolved(&matching, true, 101, 112)); // Original source, seq and acceptedAt survive.
                        observe(gate, highNonmotion, 200);
                        CHECK(!gate.unresolved(nullptr, false, 0, 200)); // Cannot rearm after release.
                        observe(gate, acceptedMotion, 201);
                        CHECK(!gate.unresolved(nullptr, false, 0, 201)); // Direct/recovered duplicate remains informational.
                        if (entry == 0) gate.observeAccepted(acceptedMotion, 202);
                        else gate.observeAccepted(typedAcceptance(source, 10), 202, kind);
                        CHECK(!gate.unresolved(nullptr, false, 0, 202)); // Both entry points share the same dedup evidence.

                        observe(gate, typedGuardRequest(source, 11, kind), 300);
                        CHECK(gate.unresolved(nullptr, false, 0, 300)); // Nonmotion did not pollute seen sequence.
                        CHECK(gate.unresolved(&matching, true, 301, 301));
                        if (source == v4::Source::CloudCommand) text(matchingIdle.cloudWatermark, "11");
                        else text(matchingIdle.localWatermark, "11");
                        const auto next = typedStatus(matchingIdle);
                        CHECK(!gate.unresolved(&next, true, 301, 301));
                    }
                }
            }
        }
    }
}

void mode_guard() {
    BrainSimulationModeGuard neverAccepted;
    CHECK(!neverAccepted.unresolved(nullptr, false, 0, nowMs));
    neverAccepted.observeAccepted(typedAcceptance(v4::Source::CloudCommand, 100, false), nowMs);
    CHECK(!neverAccepted.unresolved(nullptr, false, 0, nowMs));
    Rig pure;
    CHECK(pure.d.setEnabled(true, neverAccepted.unresolved(nullptr, false, 0, nowMs))
          == SimulationModeResult::Changed);
    CHECK(pure.d.setEnabled(false, neverAccepted.unresolved(nullptr, false, 0, nowMs))
          == SimulationModeResult::Changed);

    for (const auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch}) {
        for (const uint64_t seq : {uint64_t(9), uint64_t(10), uint64_t(100), uint64_t(v4::kMaxSequence)}) {
            BrainSimulationModeGuard gate;
            const auto accepted = typedAcceptance(source, seq);
            gate.observeAccepted(accepted, 100);
            Status idle;
            idle.stationary = true;
            text(idle.cloudWatermark, std::to_string(source == v4::Source::CloudCommand ? seq : v4::kMaxSequence));
            text(idle.localWatermark, std::to_string(source == v4::Source::LocalTouch ? seq : v4::kMaxSequence));
            const auto decodedIdle = typedStatus(idle);
            CHECK(gate.unresolved(nullptr, false, 0, 100));
            CHECK(gate.unresolved(nullptr, true, 101, 101));
            CHECK(gate.unresolved(&decodedIdle, false, 101, 101));
            CHECK(gate.unresolved(&decodedIdle, true, 99, 101)); // Older idle, even with matching watermark.
            CHECK(gate.unresolved(&decodedIdle, true, 100, 100)); // Acceptance-time sample is not post-accept.
            CHECK(gate.unresolved(&decodedIdle, true, 102, 101)); // Future receipt.
            CHECK(gate.unresolved(&decodedIdle, true, 101, 1601)); // Exact stale boundary.

            for (unsigned fault = 0; fault < 4; ++fault) {
                auto moving = idle;
                if (fault == 0) moving.motionBusy = true;
                if (fault == 1) moving.isPreparing = true;
                if (fault == 2) moving.stationary = false;
                if (fault == 3) text(moving.activeExecutionId, "11111111111111111111111111111111");
                text(moving.productProgress, "complete"); // A display terminal label cannot override motion evidence.
                const auto decoded = typedStatus(moving);
                CHECK(gate.unresolved(&decoded, true, 101, 101));
            }
            auto queuedBeforeAcceptance = idle;
            if (source == v4::Source::CloudCommand) text(queuedBeforeAcceptance.cloudWatermark, std::to_string(seq - 1));
            else text(queuedBeforeAcceptance.localWatermark, std::to_string(seq - 1));
            const auto queued = typedStatus(queuedBeforeAcceptance);
            CHECK(gate.unresolved(&queued, true, 101, 101)); // Fresh delivery of old idle cannot release.
            Rig blocked;
            CHECK(blocked.d.setEnabled(true, gate.unresolved(&queued, true, 101, 101))
                  == SimulationModeResult::Busy);
            CHECK(!blocked.d.enabled() && blocked.net.resets == 0);

            // A higher rejected request may advance Motion's watermark; equality is not required.
            if (seq < v4::kMaxSequence) {
                gate.observeAccepted(typedAcceptance(source, seq + 1, false), 102);
                CHECK(gate.unresolved(nullptr, false, 0, 102));
                if (source == v4::Source::CloudCommand) text(idle.cloudWatermark, std::to_string(seq + 1));
                else text(idle.localWatermark, std::to_string(seq + 1));
            }
            idle.eventPending = true; // No Cloud stored or terminal-delivery proof is needed for stationary release.
            const auto postAccept = typedStatus(idle);
            gate.observeAccepted(accepted, 110); // Duplicate acceptance must not refresh the acceptance timestamp.
            CHECK(!gate.unresolved(&postAccept, true, 101, 110));
            CHECK(!gate.unresolved(nullptr, false, 0, 200));
            gate.observeAccepted(accepted, 200);
            gate.observeAccepted(typedAcceptance(source, seq - 1), 201);
            CHECK(!gate.unresolved(nullptr, false, 0, 201)); // Same/older accepted responses cannot reblock.
            CHECK(blocked.d.setEnabled(true, gate.unresolved(nullptr, false, 0, 201))
                  == SimulationModeResult::Changed);
            blocked.net.reconnect(nowMs);
            blocked.send(blocked.command());
            CHECK(blocked.d.running()); // After release, a pure Brain test still needs no Motion.

            if (seq < v4::kMaxSequence) {
                gate.observeAccepted(typedAcceptance(source, seq + 1), 300);
                CHECK(gate.unresolved(nullptr, false, 0, 300));
                CHECK(gate.unresolved(&decodedIdle, true, 301, 301));
                CHECK(!gate.unresolved(&postAccept, true, 301, 301));
            }

            BrainSimulationModeGuard staleBoundary;
            staleBoundary.observeAccepted(accepted, 100);
            CHECK(!staleBoundary.unresolved(&decodedIdle, true, 101, 1600)); // Age 1499 is still fresh.

            BrainSimulationModeGuard wrap;
            wrap.observeAccepted(accepted, UINT32_MAX - 2);
            CHECK(wrap.unresolved(&decodedIdle, true, UINT32_MAX - 3, 2));
            CHECK(wrap.unresolved(&queued, true, 1, 2));
            CHECK(!wrap.unresolved(&postAccept, true, 1, 2));
            wrap.observeAccepted(accepted, 3);
            CHECK(!wrap.unresolved(nullptr, false, 0, 4));
        }
    }
    BrainSimulationModeGuard separateSources;
    separateSources.observeAccepted(typedAcceptance(v4::Source::CloudCommand, 10), 100);
    Status idle;
    idle.stationary = true;
    text(idle.cloudWatermark, "10");
    text(idle.localWatermark, "0");
    auto decoded = typedStatus(idle);
    CHECK(!separateSources.unresolved(&decoded, true, 101, 101));
    separateSources.observeAccepted(typedAcceptance(v4::Source::LocalTouch, 10), 102);
    CHECK(separateSources.unresolved(nullptr, false, 0, 102));
    text(idle.cloudWatermark, "100");
    text(idle.localWatermark, "9");
    decoded = typedStatus(idle);
    CHECK(separateSources.unresolved(&decoded, true, 103, 103));
    text(idle.localWatermark, "10");
    decoded = typedStatus(idle);
    CHECK(!separateSources.unresolved(&decoded, true, 103, 103));
    mode_guard_command_filter();
}

void retained_receipt_sessions() {
    // Exercise every deletion index at capacity, with independent ACK sessions.
    for (unsigned removed = 0; removed < BrainSimulation::kResultCapacity; ++removed) {
        nowMs = 100;
        Rig r;
        r.enable();
        std::array<CloudCommand, 4> originals;
        std::array<Event, 4> frozen;
        std::array<bool, 4> alive = {true, true, true, true};
        const auto capture = [&](uint64_t seq) {
            for (unsigned tries = 0; tries < 5; ++tries) {
                if (!r.net.events.empty() && r.net.events.back().terminal.request.sequence == seq)
                    return r.net.events.back();
                r.poll(nowMs + 250);
            }
            throw std::runtime_error("round-robin publisher did not visit retained seq=" + std::to_string(seq));
        };
        const auto retry = [&](const CloudCommand& original) {
            auto incoming = original;
            text(incoming.session, r.net.token);
            incoming.sampledAtMs = nowMs;
            CHECK(std::strcmp(incoming.session, original.session));
            const auto count = r.net.acks.size();
            r.send(incoming);
            CHECK(r.net.acks.size() == count + 1);
            ack(r.net.acks.back(), original, true, "accepted");
        };
        const auto retained = [&]() {
            for (unsigned i = 0; i < originals.size(); ++i) {
                if (!alive[i]) continue;
                retry(originals[i]);
                CHECK(capture(originals[i].request.sequence).payload == frozen[i].payload);
            }
        };
        for (unsigned i = 0; i < originals.size(); ++i) {
            if (i) { r.net.resetCommandSession(); r.net.reconnect(nowMs); }
            originals[i] = r.command((i + 1) * 10);
            r.send(originals[i]);
            CHECK(r.d.running() && r.d.resultCount() == i);
            if (i % 2) {
                nowMs += 3;
                r.send(r.stop(originals[i].request.sequence + 1));
                r.poll(nowMs);
            } else r.finish();
            frozen[i] = capture(originals[i].request.sequence);
            CHECK(frozen[i].terminal.completed == (i % 2 == 0));
            CHECK(r.d.resultCount() == i + 1);
        }
        r.net.resetCommandSession();
        r.net.reconnect(nowMs);
        const auto newerDecision = r.command(50, ProductCommand::Clean);
        r.send(newerDecision); // Force all four duplicates through retainedSession, not last_.
        ack(r.net.acks.back(), newerDecision, false, "unsupported_command", "clean");
        retained();
        CHECK(!r.d.running() && r.d.resultCount() == 4 && !r.d.status().canStart);

        auto wrongDevice = stored(frozen[removed].terminal);
        text(wrongDevice.deviceId, "Other_device");
        r.d.receipt(wrongDevice); // Matching event alone must not shift session metadata.
        auto unknown = frozen[removed].terminal;
        CHECK(makeProductEventId(pairing(), v4::Source::CloudCommand, 999, unknown.eventId));
        r.d.receipt(stored(unknown));
        retained();
        CHECK(r.d.resultCount() == 4);

        const auto matched = stored(frozen[removed].terminal);
        r.d.receipt(matched);
        alive[removed] = false;
        CHECK(r.d.resultCount() == 3 && r.d.status().canStart);
        retained();
        r.d.receipt(matched); // Duplicate receipt must not shift a second time.
        retained();
        CHECK(r.d.resultCount() == 3 && !r.d.running());

        const auto active = r.command(60);
        r.send(active);
        CHECK(r.d.running() && r.d.resultCount() == 3);
        r.net.resetCommandSession();
        r.net.reconnect(nowMs);
        r.send(r.command(61, ProductCommand::Clean));
        retry(active); // Active session survives reconnect and newer rejection.
        CHECK(r.d.running() && r.d.status().request->sequence == 60);
        const unsigned secondRemoved = (removed + 1) % 4;
        r.d.receipt(stored(frozen[secondRemoved].terminal));
        alive[secondRemoved] = false;
        CHECK(r.d.running() && r.d.resultCount() == 2);
        retry(active); // Compacting older results must not mutate activeSession_.
        const auto stopInNewSession = r.stop(62);
        CHECK(std::strcmp(stopInNewSession.session, active.session));
        r.send(stopInNewSession);
        ack(r.net.acks.back(), stopInNewSession, true, "accepted");
        CHECK(!r.d.running() && r.d.resultCount() == 3);
        const auto stopped = capture(60);
        CHECK(!stopped.terminal.completed && sameProductRequest(stopped.terminal.request, active.request));
        retry(active); // Terminal ACK uses acceptance session, not the Stop session.
        retained();

        CHECK(r.d.setEnabled(false, false) == SimulationModeResult::Changed);
        r.net.reconnect(nowMs);
        r.d.receipt(stored(stopped.terminal));
        CHECK(r.d.resultCount() == 2 && !r.d.enabled());
        for (unsigned i = 0; i < alive.size(); ++i)
            if (alive[i]) CHECK(capture(originals[i].request.sequence).payload == frozen[i].payload);
        r.enable();
        retained();
        for (unsigned i = 0; i < alive.size(); ++i)
            if (alive[i]) r.d.receipt(stored(frozen[i].terminal));
        CHECK(!r.d.resultCount() && !r.d.running() && r.d.status().canStart);

        const auto reusedSlot = r.command(70);
        r.send(reusedSlot);
        r.finish();
        const auto last = capture(70);
        r.net.resetCommandSession();
        r.net.reconnect(nowMs);
        r.send(r.command(71, ProductCommand::Clean));
        retry(reusedSlot); // Empty slot reuse cannot inherit a deleted result's session.
        CHECK(last.payload == capture(70).payload && r.d.resultCount() == 1 && !r.d.running());
        r.d.receipt(stored(last.terminal));
        CHECK(!r.d.resultCount());
    }
}

void retained_ack_session() {
    Rig r;
    r.enable();
    const auto original = r.command(1);
    r.send(original);
    const auto busy = r.command(2);
    r.send(busy);
    ack(r.net.acks.back(), busy, false, "busy");
    r.finish();
    r.net.resetCommandSession();
    r.net.reconnect(nowMs);
    auto retry = original;
    text(retry.session, r.net.token);
    retry.sampledAtMs = nowMs;
    r.send(retry);
    CHECK(!r.d.running() && r.d.resultCount() == 1);
    const auto& actual = r.net.acks.back();
    CHECK(actual.id == original.request.commandId && actual.sequence == 1 && actual.accepted);
    if (actual.session != original.session)
        throw std::runtime_error("retained seq=1 accepted in session " + std::string(original.session) +
            "; seq=2 rejected Busy; reconnect+retry seq=1 ACK uses retry session " + actual.session +
            " instead of frozen original ACK session");
    retained_receipt_sessions();
}

} // namespace

int main(int argc, char** argv) {
    const struct { const char* name; void (*run)(); } groups[] = {
        {"ownership", ownership}, {"modes", modes}, {"context_admission", context_admission},
        {"unsupported", unsupported}, {"frozen_event", frozen_event}, {"stop_failure", stop_failure},
        {"freshness", freshness}, {"freshness_wrap", freshness_wrap}, {"admission_recheck", admission_recheck},
        {"stop_freshness", stop_freshness}, {"ack_retry", ack_retry}, {"ack_pending", ack_pending},
        {"rejected_ack_pending", rejected_ack_pending}, {"rejected_id_reuse", rejected_id_reuse},
        {"busy_duplicate", busy_duplicate},
        {"stop_ack_pending", stop_ack_pending}, {"full_receipts", full_receipts},
        {"receipts_off", receipts_off}, {"delayed_old_mode", delayed_old_mode},
        {"status_pulse", status_pulse}, {"retained_ack_session", retained_ack_session},
        {"mode_guard", mode_guard}
    };
    if (argc > 2) { std::fprintf(stderr, "Expected at most one group name\n"); return 2; }
    unsigned passed = 0, failed = 0;
    for (const auto& group : groups) {
        if (argc == 2 && std::strcmp(argv[1], group.name)) continue;
        nowMs = 100;
        fake_product_crypto::reset();
        try { group.run(); ++passed; std::printf("PASS %s\n", group.name); }
        catch (const std::exception& error) {
            ++failed;
            std::fprintf(stderr, "FAIL %s: %s\n", group.name, error.what());
        }
    }
    if (!passed && !failed) { std::fprintf(stderr, "Unknown test group\n"); return 2; }
    std::printf("BrainSimulationDispatcher: %u groups passed, %u failed; component only, no main/MQTT/Flash/Motion\n",
                passed, failed);
    return failed ? 1 : 0;
}
