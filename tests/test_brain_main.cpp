#include "FakeMainIo.h"
#include "FakeBrainNvs.h"
#include "FakeCommissioning.h"
#include "FakeCloudIo.h"
#include "WiFi.h"
#include "BrainStateStore.h"
#include "BoardPairingRecord.h"
#include <ArduinoJson.h>
#include <array>
#include <cstdio>
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
void seed() {
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
    check(state.installInitial(pair, &context) == BrainWrite::Stored, "fixture install failed");
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
int main(int argc, char** argv) {
    try {
        check(argc == 2, "one isolated case required");
        run(argv[1]); std::printf("PASS Brain setup/loop %s\n", argv[1]); return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL Brain setup/loop: %s\n", error.what()); return 1;
    }
}
