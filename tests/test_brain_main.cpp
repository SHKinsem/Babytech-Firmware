#include "FakeMainIo.h"
#include "FakeBrainNvs.h"
#include "FakeCommissioning.h"
#include "FakeCloudIo.h"
#include "WiFi.h"
#include "BrainStateStore.h"
#include "BoardPairingRecord.h"
#include "MotionResultDelivery.h"
#include <ArduinoJson.h>
#include <array>
#include <cstdio>
#include <iostream>
#include <stdexcept>

// Bind the production entry point to the USB SDK replacement, without copying
// its loop, callbacks or ownership decisions into the test.
#define Serial fake_main::usb
#include "../main-controller/src/main.cpp"
#undef Serial

namespace {
using fake::check;
using namespace babytech::boardlink;
namespace v4 = babytech::v4;
namespace nvs = fake_brain;
constexpr char device[] = "bt-main-test";
const std::string prefix = std::string("devices/") + device + "/";

ProductContext initialContext() {
    ProductContext value;
    std::strcpy(value.deviceId, device); value.profileVersion = 1;
    std::strcpy(value.babyId, "baby-original"); std::strcpy(value.babyName, "Original baby");
    std::strcpy(value.formulaBrand, "Test formula");
    value.waterMl = 180; value.temperatureC = 45; value.powderGPer100Ml = 25;
    check(validProductContext(value), "invalid fixture context"); return value;
}
void seed(bool withContext = true) {
    nvs::reset(); fake_commissioning::reset();
    fake_commissioning::mac = {{0x11, 0x22, 0x33, 0x44, 0x55, 0x66}};
    v4::Pairing pair;
    pair.role = v4::Role::Brain;
    std::strcpy(pair.deviceId, device); std::strcpy(pair.epoch, "0123456789abcdef0123456789abcdef");
    std::strcpy(pair.localPhysicalId, "112233445566"); std::strcpy(pair.peerPhysicalId, "aabbccddeeff");
    std::array<uint8_t, kPairingRecordMaxSize> bytes{};
    const size_t size = encodePairingRecord(pair, bytes.data(), bytes.size()); check(size, "pair encode failed");
    nvs::io.disk["productpair"]["record"] = {{bytes.begin(), bytes.begin() + size}, nvs::Type::Blob};
    BrainStateStore state; const auto context = initialContext();
    check(state.installInitial(pair, withContext ? &context : nullptr) == BrainWrite::Stored, "fixture install failed");
    for (const auto& entry : {std::make_pair("ssid", "host-network"), std::make_pair("pass", "wifi-test-password")}) {
        const std::string value = entry.second;
        nvs::io.disk["wifi-cfg"][entry.first] = {{value.begin(), value.end()}, nvs::Type::String};
        nvs::io.disk["wifi-cfg"][entry.first].bytes.push_back(0);
    }
    struct Record {
        uint32_t magic = 0x42544331;
        char host[128] = "cached.test", user[64] = "test-user", password[128] = "host-only-test-secret";
        uint16_t port = 1884;
    } record;
    const auto* start = reinterpret_cast<const uint8_t*>(&record);
    fake::io.preferences["cloudcfg/record"] = {start, start + sizeof(record)};
    WiFi.state = WL_CONNECTED; WiFi.ssid = "host-network"; WiFi.address = IPAddress(192, 168, 1, 40);
}
DynamicJsonDocument json(const std::string& value) {
    DynamicJsonDocument doc(8192);
    check(!deserializeJson(doc, value), "invalid output JSON"); return doc;
}
std::string encode(const JsonDocument& doc) {
    std::string value; serializeJson(doc, value); return value;
}
bool latestSimulationStatus(std::string& session, uint32_t& uptime) {
    for (auto it = fake::io.published.rbegin(); it != fake::io.published.rend(); ++it) {
        if (it->topic != prefix + "status") continue;
        auto doc = json(it->payload);
        if (doc["hardware_profile"] != "simulation" || !doc["can_start"].as<bool>()) continue;
        check(doc["motion_connected"] == false && doc["command_protocol"] == 4 &&
              doc["device_id"] == device, "simulation status identity/route mismatch");
        check(doc["water_temp"].isNull() && doc["remaining_powder_g"].isNull(), "fabricated physical samples");
        session = doc["command_session"].as<std::string>(); uptime = doc["device_uptime_ms"].as<uint32_t>();
        check(session.size() == 32, "missing live session"); return true;
    }
    return false;
}
std::string prepare(const std::string& session, uint32_t uptime, uint64_t sequence = 1) {
    StaticJsonDocument<1024> doc;
    doc["command"] = "prepare"; doc["command_id"] = sequence == 1 ? "main-prepare" : "main-old-context";
    doc["device_id"] = device; doc["command_seq"] = std::to_string(sequence); doc["command_session"] = session;
    doc["device_uptime_ms"] = uptime; doc["ttl_ms"] = 5000;
    doc["baby_id"] = "baby-original"; doc["feeding_context_profile_version"] = 1;
    doc["water_ml"] = 180; doc["temp"] = 45; doc["powder_g_per_100ml"] = 25;
    return encode(doc);
}
void incoming(const char* suffix, const std::string& value) { fake::io.incoming.push_back({prefix + suffix, value, false}); }
unsigned acknowledgments(const char* id, bool accepted, const char* reason,
                         const std::string& originalSession, uint64_t sequence = 1) {
    unsigned count = 0;
    for (const auto& packet : fake::io.published) {
        if (packet.topic != prefix + "ack") continue;
        auto doc = json(packet.payload);
        if (doc["command_id"] != id) continue;
        check(doc.size() == 7 && doc["accepted"].is<bool>() && doc["accepted"] == accepted &&
              doc["reason"] == reason && doc["device_id"] == device && doc["command"] == "prepare" &&
              doc["command_seq"].is<const char*>() &&
              doc["command_seq"] == std::to_string(sequence) && doc["command_session"] == originalSession,
              "wrong production ACK"); ++count;
    }
    return count;
}
std::string event(bool stopped, uint32_t expectedUptime = 0) {
    std::string identity;
    for (const auto& packet : fake::io.published) {
        if (packet.topic != prefix + "event") continue;
        auto doc = json(packet.payload);
        check(doc["execution_mode"] == "brain_simulation" && doc["device_id"] == device &&
              doc["command_id"] == "main-prepare" && doc["command_seq"] == "1" &&
              doc["baby_id"] == "baby-original" && doc["feeding_context_profile_version"] == 1 &&
              doc["water_ml"] == 180 && doc["temp"] == 45 && doc["target_powder_g"] == 45,
              "terminal identity/recipe changed");
        check(doc["event"] == (stopped ? "feeding_failed" : "feeding_completed"), "wrong terminal outcome");
        if (stopped) check(doc["uptime_ms"] == expectedUptime, "Stop result timestamp is not the exact deadline");
        const auto id = doc["event_id"].as<std::string>();
        check(!id.empty() && (identity.empty() || identity == id), "duplicate created a new terminal identity");
        identity = id;
    }
    return identity;
}
bool unsafeTxPrefix(const std::vector<uint8_t>& bytes) {
    // Inspect emission, including unfinished or deliberately invalidated CRCs;
    // a receiver's byte timeout must not hide a motion frame's transmitted head.
    for (size_t i = 0; i + 6 <= bytes.size(); ++i)
        if (!std::memcmp(bytes.data() + i, "BTM4", 4) && bytes[i + 4] == 4 &&
            (bytes[i + 5] == uint8_t(v4::Kind::Command) || bytes[i + 5] == uint8_t(v4::Kind::Stop))) return true;
    return false;
}

// A scripted wire peer, not Motion main or a replacement ControllerLink. All
// replies use production codecs and the boot/request IDs observed in real TX.
class ReadinessPeer {
public:
    Status status;
    bool sendStatus = false, automaticContext = false;
    ContextStatus contextReply = ContextStatus::Stored;
    unsigned hellos = 0, contexts = 0, commands = 0, stops = 0, queries = 0;
    ProductContext context;
    CommandMessage command;

    ReadinessPeer() {
        status.snapshot.stage = babytech::display::DisplayStage::Ready;
        status.snapshot.startEnabled = true;
        status.snapshot.waterMl = 180; status.snapshot.temperatureC = 45;
        std::strcpy(status.snapshot.babyName.data(), "Original baby");
        std::strcpy(status.snapshot.formulaBrand.data(), "Test formula");
        std::strcpy(status.productProgress, "ready");
        status.stationary = true; status.contextVersion = 1;
        status.feedingContextConfigured = true;
        std::strcpy(status.babyId, "baby-original");
        status.lowWaterValid = status.powderValid = true; status.powderGrams = 200;
        status.actuatorOperational = status.actuatorConfigValid = status.actuatorBusHealthy = true;
        status.actuatorPositionReferenced = status.executionAuthorized = true;
    }

    void tick() {
        observe();
        if (!brainBoot_) return;
        if (uint32_t(millis() - heartbeatAt_) >= 100) {
            v4::Message message; message.kind = v4::Kind::Heartbeat;
            send(message); heartbeatAt_ = millis();
        }
        if (sendStatus && uint32_t(millis() - statusAt_) >= 200) {
            v4::Message message;
            status.sampleUptimeMs = ++sample_;
            check(encodeStatus(status, message), "real STATUS fixture encode failed");
            send(message); statusAt_ = millis();
        }
    }
    void observe() {
        for (; cursor_ < fake_main::uartTx.size(); ++cursor_) {
            v4::Frame frame;
            if (!parser_.push(fake_main::uartTx[cursor_], millis(), frame)) continue;
            if (frame.kind == v4::Kind::Heartbeat || frame.kind == v4::Kind::LinkAck ||
                frame.kind == v4::Kind::LinkReject) continue;
            if (frame.kind == v4::Kind::Stop) {
                v4::StopRequest stop;
                check(frame.senderBoot == brainBoot_ && frame.receiverBoot == motionBoot_ &&
                      v4::decodeStop(frame.payload, frame.length, stop) && stop.scope == v4::StopScope::Idle &&
                      stop.source == (stops ? v4::Source::CloudCommand : v4::Source::LocalTouch) &&
                      stop.sequence == (stops ? 2U : 0U), "unexpected production idle Stop");
                ++stops; continue; // Deliberately lose its receipt.
            }
            v4::Message message;
            const auto assembled = assembler_.accept(frame, millis(), message);
            check(assembled == v4::AssemblyResult::Incomplete || assembled == v4::AssemblyResult::Complete,
                  "peer could not reassemble production TX");
            if (assembled != v4::AssemblyResult::Complete) continue;
            if (message.kind == v4::Kind::Hello) {
                v4::Hello hello;
                check(decodeHello(message, hello) && hello.role == v4::Role::Brain &&
                      !std::strcmp(hello.deviceId, device) && !std::strcmp(hello.physicalId, "112233445566"),
                      "unexpected production HELLO");
                brainBoot_ = message.senderBoot;
                hello.role = v4::Role::Motion; hello.replyTo = message.messageId;
                std::strcpy(hello.physicalId, "aabbccddeeff");
                v4::Message reply;
                check(encodeHello(hello, reply, v4::Kind::HelloAck), "peer HELLO_ACK encode failed");
                send(reply); ++hellos;
            } else if (message.kind == v4::Kind::Context) {
                check(message.receiverBoot == motionBoot_ && message.senderBoot == brainBoot_ &&
                      decodeContextMessage(message, device, context), "invalid production CONTEXT");
                contextId_ = message.messageId; ++contexts;
                if (automaticContext) replyContext();
            } else if (message.kind == v4::Kind::Command) {
                check(decodeCommand(message, command), "invalid production COMMAND"); ++commands;
            } else if (message.kind == v4::Kind::ResultQuery) {
                ResultQuery query;
                check(decodeResultQuery(message, query), "invalid production RESULT_QUERY"); ++queries;
            }
        }
    }
    void replyContext(bool wrongDigest = false) {
        check(contextId_ != 0, "no real context request to acknowledge");
        ContextResult result;
        result.replyTo = contextId_; std::strcpy(result.deviceId, context.deviceId);
        result.profileVersion = context.profileVersion; result.cleared = context.cleared;
        check(contextDigest(context, result.digest), "peer context digest failed");
        if (wrongDigest) result.digest[0] ^= 1;
        result.status = contextReply;
        v4::Message message;
        check(encodeContextResult(result, message), "peer CONTEXT_RESULT encode failed"); send(message);
    }
    void replyCommand(bool accepted = false) {
        check(commands != 0, "no real command to answer");
        CommandResult result;
        result.source = command.request.source; result.sequence = command.request.sequence;
        result.accepted = accepted;
        std::strcpy(result.commandId, command.request.commandId);
        std::strcpy(result.reason, accepted ? "accepted" : "not_ready");
        v4::Message message;
        check(encodeCommandResult(result, message), "peer COMMAND_RESULT encode failed"); send(message);
    }
    void changedStatus() { statusAt_ = millis() - 200; }

private:
    void send(v4::Message& message) {
        message.senderBoot = motionBoot_; message.receiverBoot = brainBoot_; message.messageId = nextId_++;
        size_t offset = 0;
        do {
            v4::Frame frame;
            check(v4::fragment(message, offset, frame), "peer fragment failed");
            std::array<uint8_t, v4::kMaxFrame> bytes{};
            const size_t size = v4::encode(frame, bytes.data(), bytes.size());
            check(size != 0, "peer frame encode failed");
            fake_main::uartRx.insert(fake_main::uartRx.end(), bytes.begin(), bytes.begin() + size);
            offset += frame.length;
        } while (offset < message.length);
    }
    static constexpr uint64_t motionBoot_ = 0x123456789abcdef0;
    uint64_t brainBoot_ = 0;
    uint32_t nextId_ = 1, contextId_ = 0, heartbeatAt_ = 0, statusAt_ = 0, sample_ = 0;
    size_t cursor_ = 0;
    v4::Parser parser_;
    v4::Assembler assembler_;
};

void runRealReadiness(bool exhausted) {
    check(!cloudCanStart(), "Cloud can start before setup");
    seed();
    if (exhausted) {
        auto& bytes = nvs::io.disk.at("brainstate").at("record").bytes;
        BrainState fixture;
        check(decodeBrainState(bytes.data(), bytes.size(), fixture), "cannot decode initial BrainState fixture");
        fixture.localSequence = v4::kMaxSequence;
        std::array<uint8_t, kBrainStateMaxSize> encoded{};
        const size_t size = encodeBrainState(fixture, encoded.data(), encoded.size());
        check(size != 0, "max-sequence BrainState fixture invalid");
        bytes.assign(encoded.begin(), encoded.begin() + size);
    }
    setup();
    check(!cloudCanStart() && productState.ready() && fake::io.tasks.size() == 1,
          "real startup fabricated readiness or failed to load fixture");
    ReadinessPeer peer;
    struct Step { std::string label; std::function<bool()> run; };
    std::deque<Step> steps;
    const auto action = [&](const char* label, std::function<void()> run) {
        steps.push_back({label, [run] { run(); return true; }});
    };
    const auto wait = [&](const char* label, std::function<bool()> run) { steps.push_back({label, run}); };
    std::string session, challenge;
    unsigned probes = 0, falseReplies = 0, trueReplies = 0;
    const auto captureSession = [&] {
        for (auto it = fake::io.published.rbegin(); it != fake::io.published.rend(); ++it) {
            if (it->topic != prefix + "status") continue;
            auto doc = json(it->payload);
            session = doc["command_session"].as<std::string>();
            check(session.size() == 32 && doc["hardware_profile"] == "real", "missing real command session");
            return true;
        }
        return false;
    };
    // Expected answers are explicit scenario outcomes, never a copied predicate.
    const auto probe = [&](const char* label, bool expected, std::function<void()> before = {}) {
        action(label, [&, before] {
            check(captureSession(), "probe has no published session");
            if (before) before();
            char value[33]; std::snprintf(value, sizeof(value), "%032x", ++probes); challenge = value;
            StaticJsonDocument<256> request;
            request["type"] = "command_session_probe"; request["device_id"] = device;
            request["command_session"] = session; request["challenge"] = challenge;
            incoming("config", encode(request));
        });
        wait(label, [&, expected, label] {
            for (const auto& packet : fake::io.published) {
                if (packet.topic != prefix + "status") continue;
                auto doc = json(packet.payload);
                if (doc["command_session_challenge"] != challenge) continue;
                if (!doc["can_start"].is<bool>() || doc["can_start"] != expected ||
                    doc["progress"] != (expected ? "ready" : "noready"))
                    throw std::runtime_error(std::string(label) + ": wrong readiness " + packet.payload);
                check(!packet.retained && doc["command_session"] == session &&
                      doc["device_id"] == device && doc["hardware_profile"] == "real" &&
                      doc["command_protocol"] == 4, "wrong real probe status identity");
                if (expected) ++trueReplies;
                else ++falseReplies;
                return true;
            }
            return false;
        });
    };
    wait("initial public status", [&] {
        if (!captureSession()) return false;
        auto doc = json(fake::io.published.back().payload);
        check(doc["can_start"] == false, "initial public status enabled Cloud"); return true;
    });
    probe("no initial STATUS/proof", false);
    action("enable Motion telemetry", [&] { peer.sendStatus = true; });
    wait("actual context transmitted", [&] { return peer.contexts != 0; });
    probe("STATUS alone is not context proof", false);
    action("wrong context digest", [&] { peer.replyContext(true); });
    probe("wrong digest is not Stored proof", false);
    action("exact Stored reply", [&] { peer.replyContext(); });
    wait("exact Stored proof consumed", [&] { return contextSync.canPrepare(); });
    probe("real ready", true);
    action("local screen readiness", [&] {
        check(fake_main::shownConnected && fake_main::shown.startEnabled == !exhausted,
              "screen local readiness does not respect its own sequence budget");
        if (exhausted) check(productState.state().localSequence == v4::kMaxSequence,
                             "max-sequence fixture not loaded");
        fake::io.loopOk = fake::io.connectOk = false;
    });
    wait("Cloud disconnected", [&] { return !network.connected(); });
    action("OFFLINE screen readiness", [&] {
        check(fake_main::shownConnected && !fake_main::shown.cloudConnected &&
              fake_main::shown.startEnabled == !exhausted && cloudCanStart(),
              "Cloud outage blocked valid local readiness or Cloud mechanical predicate");
        if (!exhausted) fake_main::intent = babytech::display::DisplayIntent::StartFeeding;
    });
    if (!exhausted) {
        wait("OFFLINE local UART Prepare", [&] { return peer.commands == 1; });
        action("local pending excludes Cloud", [&] {
            check(peer.command.request.source == v4::Source::LocalTouch &&
                  peer.command.request.command == ProductCommand::Prepare &&
                  peer.command.request.sequence == 1 && productState.state().pending && !cloudCanStart(),
                  "offline click did not use real durable local dispatch");
            peer.replyCommand();
        });
        wait("local definitive reply clears pending", [&] {
            return !productState.state().pending && !localDispatcher.busy();
        });
    }
    action("Cloud reconnect", [&] { fake::io.loopOk = fake::io.connectOk = true; });
    wait("new Cloud session status", [&] {
        const auto previous = session;
        return network.connected() && captureSession() && session != previous;
    });
    probe("ready after reconnect", true);

    const auto statusCase = [&](const char* label, std::function<void()> change) {
        action(label, [&, change] { change(); peer.changedStatus(); });
        wait("changed STATUS received", [&, changeSample = uint32_t(0)]() mutable {
            if (!changeSample) { changeSample = millis(); return false; }
            if (controllerLink.lastTelemetryReceivedAtMs() < changeSample) return false;
            changeSample = 0; return true;
        });
        probe(label, false);
        action("restore idle STATUS", [&] { peer.status = ReadinessPeer().status; peer.changedStatus(); });
        wait("restored STATUS received", [&] { return cloudCanStart(); });
        probe("restored readiness", true);
    };
    statusCase("Motion start disabled", [&] { peer.status.snapshot.startEnabled = false; });
    statusCase("Motion busy", [&] { peer.status.motionBusy = true; });
    statusCase("Motion not stationary", [&] { peer.status.stationary = false; });
    statusCase("Motion active execution", [&] {
        peer.status.executionOwner = ExecutionOwner::Workbench;
        std::strcpy(peer.status.activeExecutionId, "11111111111111111111111111111111");
    });
    statusCase("Motion context version mismatch", [&] { peer.status.contextVersion = 2; });
    statusCase("Motion baby mismatch", [&] { std::strcpy(peer.status.babyId, "baby-other"); });
    action("age STATUS while heartbeats continue", [&] { peer.sendStatus = false; });
    wait("STATUS reaches stale boundary", [&] {
        return uint32_t(millis() - controllerLink.lastTelemetryReceivedAtMs()) >= 1500 && fake_main::uartRx.empty();
    });
    probe("stale STATUS", false);
    action("restore fresh STATUS", [&] { peer.sendStatus = true; peer.changedStatus(); });
    wait("fresh STATUS restores readiness", [&] { return cloudCanStart(); });
    probe("ready after stale", true);
    action("USB maintenance begin", [&] { fake_main::input("MAINT BEGIN"); });
    wait("maintenance entered", [&] { return commissioningSession.active(); });
    probe("maintenance", false);
    action("USB maintenance end", [&] { fake_main::input("MAINT END"); });
    wait("maintenance ended", [&] { return !commissioningSession.active(); });
    probe("ready after maintenance", true);

    // Config/probe has priority over the ordinary command queue. Exercise a
    // command actually consumed by main, not fictitious FIFO ordering.
    action("Cloud Prepare input", [&] { incoming("command", prepare(session, millis())); });
    wait("Cloud Prepare reached actual UART", [&] { return peer.commands == (exhausted ? 1U : 2U); });
    probe("ordinary command outstanding", false);
    action("Cloud reply", [&] {
        check(peer.command.request.source == v4::Source::CloudCommand && cloudDispatcher.ordinaryBusy(),
              "test never exercised ordinary Cloud owner");
        peer.replyCommand();
        fake::io.loopOk = fake::io.connectOk = false;
    });
    wait("offline definitive reply releases ordinary owner", [&] {
        return !network.connected() && !cloudDispatcher.ordinaryBusy();
    });
    action("pending command ACK still excludes readiness", [&] {
        check(cloudDispatcher.resultPending() && !cloudCanStart(), "pending command ACK reported ready");
        fake::io.loopOk = fake::io.connectOk = true;
    });
    wait("command ACK accepted after reconnect", [&] {
        const auto previous = session;
        return network.connected() && !cloudDispatcher.resultPending() && captureSession() && session != previous;
    });
    probe("ready after Cloud reply", true);
    probe("same-batch update then probe", false, [&] {
        ProductContext updated = initialContext(); updated.profileVersion = 2;
        std::array<uint8_t, v4::kMaxMessage> bytes{};
        const size_t size = encodeProductContext(updated, bytes.data(), bytes.size());
        check(size != 0, "context update fixture encode failed");
        incoming("config", std::string(reinterpret_cast<char*>(bytes.data()), size));
    });
    wait("updated context on wire", [&] { return peer.context.profileVersion == 2; });
    action("Motion has version but no proof", [&] { peer.status.contextVersion = 2; peer.changedStatus(); });
    probe("updated context awaits exact proof", false);
    action("updated exact Unchanged reply", [&] { peer.contextReply = ContextStatus::Unchanged; peer.replyContext(); });
    wait("updated context proved", [&] { return cloudCanStart(); });
    probe("ready after Unchanged", true);
    probe("same-batch tombstone then probe", false, [&] {
        incoming("config", std::string("{\"type\":\"feeding_context\",\"device_id\":\"") + device +
                 "\",\"profile_version\":3,\"cleared\":true}");
    });
    wait("tombstone on wire", [&] { return peer.context.profileVersion == 3 && peer.context.cleared; });
    action("prove persisted tombstone", [&] { peer.replyContext(); });
    probe("persisted tombstone cannot start", false);
    action("restore active context", [&] {
        peer.automaticContext = true; peer.status.contextVersion = 4; peer.changedStatus();
        ProductContext updated = initialContext(); updated.profileVersion = 4;
        std::array<uint8_t, v4::kMaxMessage> bytes{};
        const size_t size = encodeProductContext(updated, bytes.data(), bytes.size());
        check(size != 0, "restore context fixture encode failed");
        incoming("config", std::string(reinterpret_cast<char*>(bytes.data()), size));
    });
    wait("restored context proved", [&] { return cloudCanStart(); });
    probe("ready after context restoration", true);
    action("historical outbox telemetry", [&] {
        peer.status.eventPending = true; std::strcpy(peer.status.pendingEventId, "historical-event"); peer.changedStatus();
    });
    wait("historical outbox STATUS received", [&] { return controllerLink.lastTelemetry()->eventPending; });
    probe("historical outbox is not mechanical busy", true);
    action("second Cloud Prepare input", [&] {
        auto request = json(prepare(session, millis(), 3));
        request["command_id"] = "readiness-accepted"; request["feeding_context_profile_version"] = 4;
        incoming("command", encode(request));
    });
    wait("second Cloud Prepare on UART", [&] { return peer.commands == (exhausted ? 2U : 3U); });
    action("accept command without new Motion STATUS", [&] { peer.sendStatus = false; peer.replyCommand(true); });
    wait("accepted owner releases ordinary slot", [&] { return !cloudDispatcher.ordinaryBusy(); });
    probe("accepted command old idle not ready", false);
    action("later idle still has old watermark", [&] { peer.sendStatus = true; peer.changedStatus(); });
    wait("post-acceptance old-watermark STATUS", [&] {
        return controllerLink.lastTelemetry()->sampleUptimeMs == peer.status.sampleUptimeMs;
    });
    probe("later idle old watermark not ready", false);
    action("accepted watermark but not stationary", [&] {
        std::strcpy(peer.status.cloudWatermark, "3"); peer.status.stationary = false; peer.changedStatus();
    });
    wait("nonstationary accepted-watermark STATUS", [&] {
        return !controllerLink.lastTelemetry()->stationary &&
            !std::strcmp(controllerLink.lastTelemetry()->cloudWatermark, "3");
    });
    probe("accepted watermark not stationary", false);
    action("accepted watermark fresh stationary", [&] { peer.status.stationary = true; peer.changedStatus(); });
    wait("accepted movement proof released", [&] { return cloudCanStart(); });
    probe("accepted watermark stationary restores ready", true);
    action("touchscreen local Stop", [&] { fake_main::stopClick = true; });
    wait("local Stop on actual UART", [&] {
        return peer.stops == 1 && controllerLink.stopSendState() == StopSendState::Pending;
    });
    probe("local Stop in flight", false);
    wait("local Stop transport deadline", [&] { return controllerLink.stopSendState() != StopSendState::Pending; });
    probe("local Stop no mechanical owner", true);
    probe("same-batch Stop then probe", false, [&] {
        auto request = json(prepare(session, millis(), 2)); request["command"] = "stop";
        request["command_id"] = "readiness-stop";
        request.remove("baby_id"); request.remove("feeding_context_profile_version");
        request.remove("water_ml"); request.remove("temp"); request.remove("powder_g_per_100ml");
        incoming("command", encode(request));
    });
    wait("lost Stop receipt becomes informational lookup", [&] {
        return peer.stops == 2 && peer.queries != 0 && cloudDispatcher.busy() && !cloudDispatcher.stopInFlight();
    });
    probe("informational Stop lookup does not own mechanics", true);

    unsigned phasePasses = 0, completedSteps = 0;
    const auto scheduledSteps = steps.size();
    fake::io.onDelay = [&](unsigned) {
        const bool worker = fake::io.inWorker; fake::io.inWorker = false;
        peer.tick(); loop(); peer.observe();
        // Fake SDK writes only seven bytes. Drain the real adapter at the same
        // logical instant so worker sleeps do not masquerade as UART latency.
        // This intentionally proves no baud rate, FreeRTOS or Motion timing.
        for (unsigned pump = 0; pump < 160; ++pump) controllerLink.poll(uint32_t(millis()));
        peer.observe();
        if (steps.empty()) throw fake::StopWorker{};
        if (steps.front().run()) { steps.pop_front(); phasePasses = 0; ++completedSteps; }
        else if (++phasePasses >= 150)
            throw std::runtime_error("real-readiness stalled: " + steps.front().label + " USB=" + fake_main::usb.output);
        fake::io.now += 5;
        fake::io.inWorker = worker;
    };
    fake::runWorker();
    check(steps.empty() && completedSteps == scheduledSteps && peer.hellos && peer.contexts == 4 &&
          probes == 38 && trueReplies == 17 && falseReplies == 21 &&
          peer.commands == (exhausted ? 2U : 3U) && peer.stops == 2,
          "real readiness schedule did not finish its wire/public-status assertions");
    check(!nvs::count(nvs::Op::Erase) && !nvs::count(nvs::Op::Init) && nvs::io.handles.empty(),
          "real readiness unsafe NVS operation/leak");
    for (const auto& call : WiFi.calls) check(call.worker, "readiness Wi-Fi escaped network worker");
    check(!fake::io.preferenceWriteCalls && !simulating(), "real readiness rewrote credentials or enabled simulation");
    std::printf("READINESS %s: steps=%u probes=%u true=%u false=%u HELLO=%u CONTEXT=%u COMMAND=%u STOP=%u QUERY=%u\n",
                exhausted ? "max-sequence" : "offline-local", completedSteps, probes, trueReplies, falseReplies,
                peer.hellos, peer.contexts, peer.commands, peer.stops, peer.queries);
    fake::cleanupLifetimeResources();
}
DynamicJsonDocument bridgeInput(const std::string& line) {
    check(line.size() <= 65536, "bridge input exceeds test budget");
    auto input = json(line);
    check(input.is<JsonObject>(), "bridge step must be an object");
    if (input.containsKey("quit")) check(input["quit"].is<bool>(), "invalid bridge quit flag");
    if (input.containsKey("advance_ms"))
        check(input["advance_ms"].is<uint32_t>() && input["advance_ms"].as<uint32_t>() <= 1000,
              "bridge tick must be an unsigned integer within test budget");
    if (input.containsKey("connected")) check(input["connected"].is<bool>(), "invalid bridge connection flag");
    if (input.containsKey("usb")) check(input["usb"].is<const char*>(), "invalid bridge USB input");
    if (input.containsKey("incoming")) check(input["incoming"].is<JsonArray>(), "bridge incoming must be an array");
    for (auto packet : input["incoming"].as<JsonArray>()) {
        check(packet.is<JsonObject>() && packet["topic"].is<const char*>() &&
              packet["payload"].is<const char*>() && packet["retained"].is<bool>(),
              "invalid bridge packet field types");
        const auto topic = packet["topic"].as<std::string>();
        check(topic == prefix + "command" || topic == prefix + "config", "unexpected bridge input topic");
    }
    return input;
}
void checkBridgeInputs() {
    const auto packet = std::string("{\"topic\":\"") + prefix + "config\",\"payload\":\"{}\",\"retained\":false}";
    auto valid = bridgeInput("{\"advance_ms\":1000,\"incoming\":[" + packet + "],\"connected\":true,\"usb\":\"SIM ON\"}");
    check(valid["incoming"][0]["payload"] == "{}", "bridge altered valid payload bytes");
    for (const auto& value : {"[]", "null", "{\"quit\":1}", "{\"advance_ms\":1.5}",
              "{\"advance_ms\":true}", "{\"advance_ms\":-1}", "{\"advance_ms\":1001}",
              "{\"incoming\":{}}", "{\"incoming\":[null]}", "{\"connected\":1}", "{\"usb\":42}",
              "{\"incoming\":[{\"topic\":\"devices/bt-main-test/config\",\"payload\":42,\"retained\":false}]}",
              "{\"incoming\":[{\"topic\":\"devices/bt-main-test/config\",\"payload\":\"{}\",\"retained\":1}]}"}) {
        bool rejected = false;
        try { bridgeInput(value); } catch (const std::exception&) { rejected = true; }
        check(rejected, "bridge accepted malformed orchestration input");
    }
}
void runBridge() {
    // Pairing is a synthetic installation fixture; business context must arrive
    // unmodified from the real Cloud broker path, not from a copied API record.
    seed(false);
    setup();
    check(productState.ready() && fake::io.tasks.size() == 1, "bridge startup failed");
    size_t usbCursor = 0;
    fake::io.onDelay = [&](unsigned) {
        const bool worker = fake::io.inWorker;
        fake::io.inWorker = false;
        check(!unsafeTxPrefix(fake_main::uartTx), "broker simulation emitted real UART motion/control");
        check(!productState.state().pending && !productState.state().localSequence,
              "broker simulation used durable local identity");
        DynamicJsonDocument report(32768);
        report["now_ms"] = millis(); report["connected"] = network.connected();
        report["simulation"] = simulation && simulation->enabled();
        report["running"] = simulation && simulation->running();
        report["result_count"] = simulation ? simulation->resultCount() : 0;
        report["profile_version"] = productState.state().context.profileVersion;
        report["baby_id"] = productState.state().context.babyId;
        report["context_cleared"] = productState.state().context.cleared;
        report["pending"] = productState.state().pending;
        report["local_sequence"] = productState.state().localSequence;
        report["uart_motion_frames"] = 0;
        auto subscriptions = report.createNestedArray("subscriptions");
        for (const auto& topic : fake::io.subscriptions) subscriptions.add(topic);
        report["usb_output"] = fake_main::usb.output.substr(usbCursor);
        usbCursor = fake_main::usb.output.size();
        auto packets = report.createNestedArray("published");
        for (const auto& packet : fake::io.published) {
            auto value = packets.createNestedObject();
            value["topic"] = packet.topic; value["payload"] = packet.payload;
            value["retained"] = packet.retained;
        }
        check(!report.overflowed(), "bridge output overflow");
        std::cout << "BRAIN_BRIDGE=" << encode(report) << std::endl;
        fake::io.published.clear();
        fake::io.subscriptions.clear();
        std::string line;
        if (!std::getline(std::cin, line)) throw fake::StopWorker{};
        auto input = bridgeInput(line);
        if (input["quit"].as<bool>()) throw fake::StopWorker{};
        const auto advance = input["advance_ms"] | uint32_t(0);
        fake::io.now += advance;
        if (input.containsKey("connected")) {
            fake::io.connectOk = fake::io.loopOk = input["connected"].as<bool>();
        }
        if (input.containsKey("usb")) fake_main::input(input["usb"].as<const char*>());
        for (auto packet : input["incoming"].as<JsonArray>()) {
            const auto topic = packet["topic"].as<std::string>();
            fake::io.incoming.push_back({topic, packet["payload"].as<std::string>(),
                                         packet["retained"].as<bool>()});
        }
        loop();
        fake::io.inWorker = worker;
    };
    fake::runWorker();
    check(!nvs::count(nvs::Op::Erase) && !nvs::count(nvs::Op::Init) && nvs::io.handles.empty(),
          "bridge unsafe NVS operation/leak");
    for (const auto& call : WiFi.calls) check(call.worker, "bridge Wi-Fi escaped worker");
    check(!fake::io.preferenceWriteCalls, "bridge changed credentials");
    fake::cleanupLifetimeResources();
}
void run(const std::string& name) {
    v4::Frame control; control.kind = v4::Kind::Stop; control.senderBoot = 1; control.receiverBoot = 2;
    control.messageId = 1; control.length = control.total = 1;
    std::array<uint8_t, v4::kMaxFrame> probe{};
    check(v4::encode(control, probe.data(), probe.size()), "observer control fixture invalid");
    check(unsafeTxPrefix({probe.begin(), probe.begin() + 6}), "observer missed an unfinished control header");
    control.kind = v4::Kind::Command;
    check(v4::encode(control, probe.data(), probe.size()), "observer command fixture invalid");
    check(unsafeTxPrefix({probe.begin(), probe.begin() + 6}), "observer missed an unfinished command header");
    seed(); fake_main::panelReady = name != "panel-failure";
    const auto originalBrain = nvs::io.disk.at("brainstate");
    const auto initialWrites = nvs::count(nvs::Op::Set);
    setup();
    check(fake_main::usb.txTimeout == 1 && fake::io.tasks.size() == 1, "setup missing bounded USB/network task");
    check(productState.ready() && !productState.state().pending, "startup lost cached business state");
    fake_main::input("SIM ON");
    unsigned phase = 0;
    uint32_t acceptedAt = 0;
    bool offlineInjected = false;
    bool deadlineArmed = false, deadlineStopInjected = false;
    std::string session, command, terminal;
    uint32_t uptime = 0;
    unsigned commandFrames = 0, stopFrames = 0, helloFrames = 0;
    size_t uartCursor = 0;
    v4::Parser observer;
    fake::io.onDelay = [&](unsigned passes) {
        const bool worker = fake::io.inWorker; fake::io.inWorker = false;
        if (phase == 2 && name == "simulation-stop" && deadlineArmed) {
            check(millis() == acceptedAt + 15000, "touch Stop missed exact deadline");
            fake_main::stopClick = true;
            deadlineStopInjected = true;
        }
        loop();
        check(!unsafeTxPrefix(fake_main::uartTx), "Brain emitted an unsafe full/partial UART header");
        for (; uartCursor < fake_main::uartTx.size(); ++uartCursor) {
            v4::Frame frame;
            // This is a coalesced SDK TX byte audit, not a peer timing model.
            if (observer.push(fake_main::uartTx[uartCursor], 0, frame)) {
                commandFrames += frame.kind == v4::Kind::Command;
                stopFrames += frame.kind == v4::Kind::Stop;
                helloFrames += frame.kind == v4::Kind::Hello;
            }
        }
        check(!commandFrames && !stopFrames, "Brain simulation emitted real UART motion/control");
        check(!productState.state().pending && !productState.state().localSequence, "simulation borrowed durable local identity");
        if (phase == 2 && name == "simulation-offline" && !offlineInjected &&
            acknowledgments("main-prepare", true, "accepted", session)) {
            fake::io.loopOk = false; fake::io.connectOk = false; offlineInjected = true;
        }
        if (phase == 0 && latestSimulationStatus(session, uptime) &&
            fake_main::usb.output.find("[simulation] on") != std::string::npos) {
            command = prepare(session, uptime); incoming("command", command); phase = 1;
        } else if (phase == 1 && simulation->running()) {
            acceptedAt = millis() - 5; phase = 2;
            if (name == "simulation-context") {
                incoming("config", std::string("{\"type\":\"feeding_context\",\"device_id\":\"") + device +
                         "\",\"profile_version\":2,\"cleared\":true}");
                incoming("command", prepare(session, millis(), 2));
            }
            if (name == "simulation-offline-ack-lost") {
                fake::io.loopOk = false; fake::io.connectOk = false; offlineInjected = true;
            }
        } else if (phase == 2 && !simulation->running()) {
            check(simulation->resultCount() == 1, "completion lost/unexpected result");
            if (offlineInjected) {
                check(event(false).empty(), "offline completion pretended publish succeeded");
                fake::io.loopOk = fake::io.connectOk = true;
            }
            phase = 3;
        } else if (phase == 3 && !(terminal = event(name == "simulation-stop", acceptedAt + 15000)).empty()) {
            const auto acceptedAcks = acknowledgments("main-prepare", true, "accepted", session);
            check(name == "simulation-offline-ack-lost" ? !acceptedAcks : bool(acceptedAcks),
                  "ACK publication does not match injected connection loss");
            if (name == "simulation-context")
                check(acknowledgments("main-old-context", false, "context_required", session, 2),
                      "same-batch tombstone did not reject old context");
            if (name == "simulation-receipt") {
                fake_main::input("SIM OFF"); phase = 4;
            } else if (name == "simulation-complete") {
                incoming("command", command); phase = 5;
            } else throw fake::StopWorker{};
        } else if (phase == 4 && !simulation->enabled() && network.connected()) {
            StaticJsonDocument<512> receipt;
            receipt["type"] = "feeding_event_receipt"; receipt["device_id"] = device;
            receipt["event_id"] = terminal; receipt["status"] = "stored";
            incoming("config", encode(receipt)); phase = 6;
        } else if (phase == 5 && acknowledgments("main-prepare", true, "accepted", session) >= 2) {
            check(!simulation->running() && simulation->resultCount() == 1, "duplicate restarted/duplicated result");
            throw fake::StopWorker{};
        } else if (phase == 6 && simulation->resultCount() == 0) throw fake::StopWorker{};
        if (passes >= 500) {
            std::string details;
            for (const auto& packet : fake::io.published)
                if (packet.topic == prefix + "ack") details += packet.payload + "\n";
            throw std::runtime_error("main fixture stalled phase=" + std::to_string(phase) +
                " connected=" + std::to_string(network.connected()) + " USB=" + fake_main::usb.output +
                " ACK=" + details);
        }
        if (phase == 2 && name == "simulation-stop" &&
            uint32_t(acceptedAt + 15000 - millis()) <= 100) {
            check(millis() <= acceptedAt + 15000 - 10, "deadline scheduler moved time backwards");
            // The connected production worker yields 10 ms next; schedule the
            // exact boundary instead of rounding up to the next 105 ms pass.
            fake::io.now = acceptedAt + 15000 - 10;
            deadlineArmed = true;
        } else fake::io.now += 90;
        fake::io.inWorker = worker;
    };
    fake::runWorker();
    check(helloFrames, "TX observer did not decode a known production HELLO");
    if (name == "simulation-stop") check(deadlineStopInjected, "deadline Stop was never exercised");
    if (name == "simulation-context") {
        check(productState.state().context.cleared && productState.state().context.profileVersion == 2,
              "main never persisted the tombstone");
    } else check(nvs::io.disk.at("brainstate") == originalBrain && nvs::count(nvs::Op::Set) == initialWrites,
                 "simulation modified persistent business data");
    check(!nvs::count(nvs::Op::Erase) && !nvs::count(nvs::Op::Init) && nvs::io.handles.empty(), "unsafe NVS operation/leak");
    for (const auto& call : WiFi.calls) check(call.worker, "Wi-Fi I/O escaped the worker");
    check(!fake::io.preferenceWriteCalls, "main fixture unexpectedly rewrote credentials");
    fake::cleanupLifetimeResources();
}
}
#include "brain_main_result_fixture.h"
#include "brain_main_offline_fixture.h"
int main(int argc, char** argv) {
    try {
        if (argc >= 2 && std::string(argv[1]) == "real-auth-offline") {
            check(argc == 2 || (argc == 3 && (std::string(argv[2]) == "command-subscribe-failed" ||
                  std::string(argv[2]) == "config-subscribe-failed")), "invalid offline fixture variant");
            runColdOffline(argc == 3 ? argv[2] : "connect-rejected"); return 0;
        }
        if (argc >= 2 && std::string(argv[1]) == "real-readiness") {
            check(argc == 2 || (argc == 3 && std::string(argv[2]) == "max-sequence"), "invalid readiness fixture variant");
            runRealReadiness(argc == 3);
            std::printf("PASS Brain real-readiness %s\n", argc == 3 ? "max-sequence" : "offline-local"); return 0;
        }
        check(argc == 2, "one isolated case required");
        if (std::string(argv[1]) == "real-results" || std::string(argv[1]) == "real-results-write-failure") {
            runRealResults(std::string(argv[1]) == "real-results-write-failure");
            std::printf("PASS Brain main Motion result recovery %s\n", argv[1]); return 0;
        }
        if (std::string(argv[1]) == "bridge-input-validation") {
            checkBridgeInputs(); std::puts("PASS Brain bridge input validation"); return 0;
        }
        if (std::string(argv[1]) == "broker-bridge") { runBridge(); return 0; }
        run(argv[1]); std::printf("PASS Brain setup/loop %s\n", argv[1]); return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL Brain setup/loop: %s\n", error.what()); return 1;
    }
}
