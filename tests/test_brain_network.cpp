// Real production coordinator/station/status and transport. Only SDK I/O is
// replaced; process isolation matches CloudLink's MCU-lifetime ownership.
#include "brain_network.h"
#include "brain_status.h"
#include "brain_network_console.h"
#include "brain_simulation_dispatcher.h"
#include "brain_simulation_console.h"
#include "brain_result_delivery.h"
#include "FakeBrainNvs.h"
#include "FakeCloudIo.h"
#include "WiFi.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
using fake::check;
using babytech::brain::BrainNetwork;
using babytech::brain::BrainStation;
using babytech::boardlink::Status;
constexpr char kId[] = "bt-brain-test";
constexpr char kChallenge[] = "1234567890abcdef1234567890abcdef";
const std::string kPrefix = std::string("devices/") + kId + "/";
Status readyMotion();

void stop() { throw fake::StopWorker{}; }
size_t calls(const char* method) {
    return std::count_if(WiFi.calls.begin(), WiFi.calls.end(), [&](const FakeWiFiCall& c) {
        return c.method == method;
    });
}
void seedWifi() {
    fake::nvs.seed("ssid", "host-network");
    fake::nvs.seed("pass", "wifi-test-password");
    WiFi.state = 0;
}
void seedCloud() {
    // Existing persisted cloudcfg/record wire layout, not a settings loader.
    struct Record {
        uint32_t magic = 0x42544331;
        char host[128] = "cached.test";
        char user[64] = "test-user";
        char password[128] = "host-only-test-secret";
        uint16_t port = 1884;
    } record;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
    fake::io.preferences["cloudcfg/record"] = {bytes, bytes + sizeof(record)};
}
void connectedWifi() {
    WiFi.state = WL_CONNECTED;
    WiFi.ssid = "host-network";
    WiFi.address = IPAddress(192, 168, 1, 40);
}
void begin(BrainNetwork& network, const char* id = kId) {
    seedWifi();
    seedCloud();
    connectedWifi();
    check(network.begin(id), "valid paired identity did not start");
    check(WiFi.calls.empty() && fake::nvs.calls.empty(), "begin touched Wi-Fi/raw NVS on main thread");
    check(!network.connected() && fake::io.connectCalls == 0, "begin performed MQTT I/O");
    check(fake::io.preferenceOpens == std::vector<std::pair<std::string, bool>>{{"cloudcfg", true}},
          "CloudLink did not read cloudcfg read-only");
}
void pollAt(BrainNetwork& network, const Status* motion, bool fresh, uint32_t receivedAt,
            bool commandsEnabled = false, bool canStart = false) {
    // vTaskDelay yields to a simulated UI loop, not a second worker invocation.
    const bool worker = fake::io.inWorker;
    fake::io.inWorker = false;
    network.poll(motion, fresh, millis(), receivedAt, commandsEnabled, canStart);
    fake::io.inWorker = worker;
}
void poll(BrainNetwork& network, const Status* motion = nullptr, bool fresh = false) {
    pollAt(network, motion, fresh, millis());
}
DynamicJsonDocument packet(size_t index, bool commandsEnabled = false, bool canStart = false) {
    check(index < fake::io.published.size(), "missing status packet");
    const auto& value = fake::io.published[index];
    check(value.topic == kPrefix + "status" && !value.retained, "unexpected publish route/retained flag");
    check(value.payload.size() < 2048, "status reached/exceeded 2048-byte payload cap");
    DynamicJsonDocument doc(8192);
    check(!deserializeJson(doc, value.payload), "status payload is not JSON");
    check(doc["device_id"] == kId && doc["command_protocol"] == 4, "status identity/protocol mismatch");
    check(doc["can_start"] == canStart && doc["commands_enabled"] == commandsEnabled,
          "status command/start authorization flags incorrect");
    return doc;
}
std::string token(size_t index = 0) {
    auto doc = packet(index);
    const std::string value = doc["command_session"].as<std::string>();
    check(value.size() == 32, "status session token missing");
    return value;
}
std::string probe(const std::string& session, const char* device = kId,
                  const std::string& challenge = kChallenge) {
    StaticJsonDocument<512> doc;
    doc["type"] = "command_session_probe";
    doc["device_id"] = device;
    doc["command_session"] = session;
    doc["challenge"] = challenge;
    std::string value;
    serializeJson(doc, value);
    return value;
}
void receive(const char* suffix, const std::string& value) {
    fake::io.clients.front()->deliver(kPrefix + suffix, value);
}
void noSideEffects() {
    check(fake::nvs.writes == 0 && fake::io.preferenceWriteCalls == 0, "read-only path wrote NVS");
    check(!fake::nvs.opened, "raw NVS handle leaked");
    for (const auto& opened : fake::io.preferenceOpens)
        check(opened.first == "cloudcfg" && opened.second, "unexpected Preferences namespace/write access");
    for (const auto& call : WiFi.calls)
        if (call.method == "disconnect")
            check(call.first == 0 && call.second == 0, "Wi-Fi disconnect erased credentials/disabled radio");
}
void workerOnly() {
    for (const auto& call : WiFi.calls) check(call.worker, "Wi-Fi SDK call escaped network worker");
    for (const auto& call : fake::nvs.calls) check(call.worker, "station NVS call escaped network worker");
}

void stationConfig(const std::string& kind) {
    seedWifi();
    bool accepted = false;
    if (kind == "missing") fake::nvs.exists = false;
    else if (kind == "open-error") fake::nvs.openError = ESP_FAIL;
    else if (kind == "ssid-missing") fake::nvs.values.erase("ssid");
    else if (kind == "pass-missing") fake::nvs.values.erase("pass");
    else if (kind == "ssid-error") fake::nvs.readErrors["ssid"] = ESP_FAIL;
    else if (kind == "pass-error") fake::nvs.readErrors["pass"] = ESP_FAIL;
    else if (kind == "ssid-empty") fake::nvs.seed("ssid", "");
    else if (kind == "ssid-max") { fake::nvs.seed("ssid", std::string(32, 's')); accepted = true; }
    else if (kind == "ssid-long") fake::nvs.seed("ssid", std::string(33, 's'));
    else if (kind == "pass-open") { fake::nvs.seed("pass", ""); accepted = true; }
    else if (kind == "pass-short") fake::nvs.seed("pass", "1234567");
    else if (kind == "pass-min") { fake::nvs.seed("pass", "12345678"); accepted = true; }
    else if (kind == "pass-max") { fake::nvs.seed("pass", std::string(63, 'p')); accepted = true; }
    else if (kind == "pass-hex") { fake::nvs.seed("pass", std::string(32, 'A') + std::string(32, 'f')); accepted = true; }
    else if (kind == "pass-not-hex") fake::nvs.seed("pass", std::string(63, 'a') + "g");
    else if (kind == "pass-long") fake::nvs.seed("pass", std::string(65, 'a'));
    else if (kind == "ssid-nul") fake::nvs.seed("ssid", std::string("ok\0hidden", 9));
    else if (kind == "pass-nul") fake::nvs.seed("pass", std::string("12345678\0hidden", 15));
    else if (kind == "ssid-unterminated") fake::nvs.values["ssid"].back() = 'x';
    else if (kind == "pass-unterminated") fake::nvs.values["pass"].back() = 'x';
    else if (kind == "ssid-length") fake::nvs.reportedLengths["ssid"] = 2;
    else if (kind == "pass-length") fake::nvs.reportedLengths["pass"] = 2;
    else if (kind == "length-zero") fake::nvs.reportedLengths["pass"] = 0;
    else if (kind == "length-oversize") fake::nvs.reportedLengths["ssid"] = 34;
    else throw std::runtime_error("unknown station fixture");
    const auto original = fake::nvs.values;
    BrainStation station;
    station.poll();
    check(calls("begin") == (accepted ? 1u : 0u), "station accepted/rejected wrong credential fixture");
    check(!fake::nvs.opened, "credential load did not close handle");
    const size_t reads = fake::nvs.calls.size();
    if (accepted) {
        check(calls("persistent") == 1 && calls("autoReconnect") == 1 && calls("mode") == 1,
              "station SDK initialization missing");
        check(WiFi.calls[0].method == "persistent" && !WiFi.calls[0].first &&
              WiFi.calls[1].method == "autoReconnect" && !WiFi.calls[1].first &&
              WiFi.calls[2].method == "mode" && WiFi.calls[2].first == WIFI_STA,
              "SDK persistence/autoreconnect/mode setup incorrect");
        check(WiFi.attemptedSsid == original.at("ssid").data() &&
              WiFi.attemptedPassword == original.at("pass").data(), "credentials changed/truncated");
        const auto security = std::find_if(WiFi.calls.begin(), WiFi.calls.end(),
            [](const FakeWiFiCall& call) { return call.method == "security"; });
        check(security != WiFi.calls.end() && security->first ==
              (kind == "pass-open" ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK), "wrong minimum Wi-Fi security");
    } else check(WiFi.calls.empty(), "invalid configuration touched Wi-Fi SDK");
    fake::io.now += 60000;
    station.poll();
    check(fake::nvs.calls.size() == reads, "station reloaded raw credentials automatically");
    check(fake::nvs.values == original, "credential error altered persistent evidence");
    noSideEffects();
}

void stationRetry(bool rollover) {
    seedWifi();
    fake::io.now = rollover ? UINT32_MAX - 10000u : 1000u;
    const uint32_t started = millis();
    BrainStation station;
    station.poll();
    fake::io.now = started + 14999u;
    station.poll();
    check(calls("begin") == 1 && calls("disconnect") == 0, "join ended before 15s");
    fake::io.now = started + 15000u;
    station.poll();
    check(calls("begin") == 1 && calls("disconnect") == 1, "join timeout not at 15s");
    fake::io.now = started + 29999u;
    station.poll();
    check(calls("begin") == 1, "retry occurred before 30s");
    fake::io.now = started + 30000u;
    station.poll();
    check(calls("begin") == 2 && calls("disconnect") == 1, "retry missing at 30s");
    connectedWifi();
    fake::io.now = started + 30001u;
    station.poll();
    WiFi.state = 0;
    fake::io.now = started + 60000u;
    station.poll();
    check(calls("begin") == 2, "disconnect retry ignored last connected sample");
    ++fake::io.now;
    station.poll();
    check(calls("begin") == 3, "disconnect retry missing after last connected +30s");
    noSideEffects();
}
void stationConnected(const std::string& kind) {
    seedWifi();
    connectedWifi();
    if (kind == "wrong-ssid") WiFi.ssid = "other-network";
    if (kind == "no-ip") WiFi.address = IPAddress(0);
    BrainStation station;
    station.poll();
    check(calls("begin") == (kind == "connected" ? 0u : 1u),
          "station ignored SSID/IP validity or rejoined healthy network");
    fake::io.now += 15000;
    station.poll();
    check(calls("disconnect") == (kind == "connected" ? 0u : 1u), "SSID/IP join timeout incorrect");
    noSideEffects();
}

void identity(bool maximum) {
    BrainNetwork network;
    for (const char* id : {static_cast<const char*>(nullptr), "", "bad/pair", " bad", "-bad", "bad+id"}) {
        check(!network.begin(id), "unpaired/invalid identity started network");
        poll(network);
    }
    check(!network.begin(std::string(65, 'b').c_str()), "oversized identity accepted");
    check(fake::io.tasks.empty() && fake::io.allocationCalls == 0 && WiFi.calls.empty() &&
          fake::nvs.calls.empty() && fake::io.preferenceOpens.empty(), "bad pairing caused side effects");
    const std::string id = maximum ? std::string(64, 'b') : kId;
    begin(network, id.c_str());
    check(!network.begin(kId) && fake::io.tasks.size() == 1, "begin created duplicate worker");
    fake::io.onDelay = [&](unsigned tick) {
        check(network.connected() && fake::io.connectedId == id, "MQTT did not use paired identity");
        check(fake::io.server == "cached.test" && fake::io.port == 1884 && fake::io.credentialsMatched,
              "MQTT did not load independent cloudcfg credentials");
        if (tick == 1) poll(network);
        else {
            check(fake::io.published.size() == 1 && fake::io.published[0].topic == "devices/" + id + "/status",
                  "status not routed by paired identity");
            workerOnly();
            stop();
        }
    };
    fake::runWorker();
}
void unavailable(bool noCloud) {
    BrainNetwork network;
    seedWifi();
    if (noCloud) connectedWifi();
    else seedCloud();
    check(network.begin(kId), "unconfigured transport should still start worker");
    poll(network);
    fake::io.onDelay = [&](unsigned tick) {
        poll(network);
        check(!network.connected() && fake::io.connectCalls == 0 && fake::io.published.empty(),
              "missing Wi-Fi/cloudcfg created an active MQTT session");
        if (tick == 3) { workerOnly(); stop(); }
    };
    fake::runWorker();
}

void periodic(bool rollover) {
    if (rollover) fake::io.now = UINT32_MAX - 1000u;
    BrainNetwork network;
    begin(network);
    const Status motion = readyMotion();
    uint32_t first = 0;
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) {
            first = millis();
            check(network.connected(), "session not connected");
            char diagnostic[95]{};
            network.diagnostics(diagnostic, sizeof(diagnostic));
            check(std::strstr(diagnostic, "mqtt=1 state=0 n=1") != nullptr, "connected diagnostic snapshot missing");
            check(diagnostic[std::strlen(diagnostic) - 1] == '\n', "diagnostic console reply truncated");
            poll(network, &motion, true);
        } else if (tick == 2) {
            check(fake::io.published.size() == 1, "first status not sent");
            char diagnostic[256]{};
            network.diagnostics(diagnostic, sizeof(diagnostic));
            check(std::strstr(diagnostic, "ok=1 fail=0") != nullptr, "publish diagnostic missing");
            check(packet(0)["device_uptime_ms"] == first, "status uptime not captured with session");
            fake::io.now = first + 1999;
            poll(network, &motion, true);
        } else if (tick == 3) {
            check(fake::io.published.size() == 1, "status published before 2s boundary");
            fake::io.now = first + 2000;
            poll(network, &motion, true);
            poll(network, &motion, true);
        } else {
            check(fake::io.published.size() == 2 && token() == token(1), "periodic session changed");
            workerOnly(); stop();
        }
    };
    fake::runWorker();
}

void newSession(const std::string& kind) {
    BrainNetwork network;
    begin(network);
    const uint32_t opened = millis();
    std::string previous;
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) poll(network);
        else if (tick == 2) {
            previous = token();
            if (kind == "probe-old-generation") receive("config", probe(previous));
            if (kind == "expiry") {
                fake::io.now = opened + babytech::cloud::kSessionLifetimeMs;
                poll(network);
            } else {
                fake::io.clients.front()->dropConnection();
                fake::io.now += 5000;
            }
        } else if (tick == 3) {
            check(network.connected() && fake::io.connectCalls == 2, "new session not established");
            poll(network);
        } else {
            check(fake::io.published.size() == 2 && token(1) != previous,
                  "new session did not immediately publish or old probe survived");
            check(!packet(1).containsKey("command_session_challenge"), "stale-generation probe answered");
            workerOnly(); stop();
        }
    };
    fake::runWorker();
}
void sendExpiry() {
    BrainNetwork network;
    begin(network);
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) {
            poll(network);
            fake::io.onLoop = [] { fake::io.now += babytech::cloud::kSessionLifetimeMs; };
        } else if (tick == 2) {
            check(fake::io.published.empty(), "status escaped after send-time session expiry");
            fake::io.onLoop = nullptr;
            poll(network);
        } else if (tick == 3) poll(network);
        else {
            check(fake::io.published.size() == 1 && fake::io.connectCalls == 2, "expiry recovery failed");
            packet(0); stop();
        }
    };
    fake::runWorker();
}

void probes(const std::string& kind) {
    BrainNetwork network;
    begin(network);
    const Status motion = readyMotion();
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) poll(network);
        else if (tick == 2) {
            const auto session = token();
            if (kind == "probe-reject") {
                receive("command", probe(session));
                const unsigned beforeWrongTopic = fake::io.queueSendCalls;
                poll(network);
                check(fake::io.queueSendCalls == beforeWrongTopic, "probe accepted on command topic");
                fake::io.clients.front()->deliver("devices/other/config", probe(session));
                fake::io.clients.front()->deliver(kPrefix + "nested/config", probe(session));
                poll(network);
                check(fake::io.queueSendCalls == beforeWrongTopic, "probe accepted wrong device prefix/nested topic");
                std::vector<std::string> rejected = {"{bad", "[]", "null", probe(session, "other"),
                    probe(kChallenge), probe(session, kId, ""), probe(session, kId, "short"),
                    probe(session, kId, std::string(32, '0')), probe(session, kId, std::string(32, 'A')),
                    probe(session, kId, std::string(33, 'a')), probe(session, kId, std::string("a\0b", 3))};
                for (const char* field : {"type", "device_id", "command_session", "challenge"}) {
                    DynamicJsonDocument doc(1024);
                    check(!deserializeJson(doc, probe(session)), "invalid test probe");
                    doc.remove(field);
                    std::string value; serializeJson(doc, value); rejected.push_back(value);
                    doc[field] = 123;
                    value.clear(); serializeJson(doc, value); rejected.push_back(value);
                }
                auto extra = probe(session); extra.pop_back(); extra += ",\"extra\":true}";
                rejected.push_back(extra);
                for (const auto& value : rejected) {
                    receive("config", value);
                    const unsigned attempts = fake::io.queueSendCalls;
                    poll(network);
                    check(fake::io.queueSendCalls == attempts, "malformed/mismatched probe enqueued a reply");
                }
            } else if (kind == "probe-budget") {
                for (unsigned i = 0; i < 4; ++i) receive("config", probe(session));
                const unsigned attempts = fake::io.queueSendCalls;
                poll(network);
                check(fake::io.queueSendCalls == attempts + 3, "coordinator inbound budget is not three");
                poll(network);
                check(fake::io.queueSendCalls == attempts + 4, "fourth probe was not deferred to next poll");
            } else {
                fake::io.incoming.push_back({kPrefix + "config", probe(session)});
            }
        } else if (kind == "probe" && tick == 3) poll(network, &motion, true);
        else if (kind == "probe-budget" && tick == 3) {
            check(fake::io.published.size() == 3, "worker failed two-message publish budget");
        } else {
            const size_t expected = kind == "probe-reject" ? 1 : kind == "probe-budget" ? 5 : 2;
            check(fake::io.published.size() == expected, "probe response count mismatch");
            for (size_t i = 1; i < expected; ++i) {
                auto reply = packet(i);
                check(reply["command_session"] == token() && reply["command_session_challenge"] == kChallenge,
                      "probe response changed session/device/challenge");
            }
            workerOnly(); stop();
        }
    };
    fake::runWorker();
}

void readonlyMessages() {
    BrainNetwork network;
    begin(network);
    const auto raw = fake::nvs.values;
    const auto cloud = fake::io.preferences;
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) poll(network);
        else if (tick == 2) {
            for (const char* type : {"feeding_context", "feeding_event_receipt", "wifi_config", "rawcfg"}) {
                receive("config", "{\"type\":\"" + std::string(type) + "\",\"device_id\":\"" + kId +
                        "\",\"version\":2147483647,\"ssid\":\"replacement\",\"pass\":\"replacement\"}");
                poll(network);
            }
            for (const char* action : {"prepare", "stop", "initialize", "clean", "reset", "rawcfg"}) {
                receive("command", "{\"device_id\":\"" + std::string(kId) + "\",\"command\":\"" + action +
                        "\",\"command_id\":\"host-only\",\"command_session\":\"" + token() + "\"}");
                poll(network);
            }
        } else {
            check(fake::io.published.size() == 1, "read-only messages produced action ACK/status/other output");
            check(fake::nvs.values == raw && fake::io.preferences == cloud, "raw/config/command mutated NVS");
            check(calls("begin") == 0, "raw config changed Wi-Fi connection");
            noSideEffects(); workerOnly(); stop();
        }
    };
    fake::runWorker();
}

std::string cloudCommand(const std::string& action, const std::string& session, uint32_t sampledAt,
                         const std::string& sequence = "42", const std::string& id = "cloud-test") {
    StaticJsonDocument<1536> doc;
    doc["device_id"] = kId;
    doc["command_id"] = id;
    doc["command"] = action;
    doc["command_seq"] = sequence;
    doc["command_session"] = session;
    doc["device_uptime_ms"] = sampledAt;
    doc["ttl_ms"] = 5000;
    if (action == "prepare") {
        doc["baby_id"] = "baby-test";
        doc["feeding_context_profile_version"] = 7;
        doc["water_ml"] = 180;
        doc["temp"] = 45;
        doc["powder_g_per_100ml"] = 25.5;
    } else if (action == "set_target_temp") doc["temp"] = 45;
    check(!doc.overflowed(), "command fixture overflowed");
    std::string value;
    serializeJson(doc, value);
    return value;
}

std::vector<size_t> ackPackets() {
    std::vector<size_t> indexes;
    for (size_t i = 0; i < fake::io.published.size(); ++i) {
        if (fake::io.published[i].topic == kPrefix + "ack") indexes.push_back(i);
        else packet(i); // Every status must remain read-only; no other output is allowed.
    }
    return indexes;
}
void commandAck(size_t index, const std::string& action, const std::string& session,
                const char* reason = "integration_not_ready", const std::string& sequence = "42",
                const std::string& id = "cloud-test", bool accepted = false) {
    check(index < fake::io.published.size(), "missing command ACK");
    const auto& value = fake::io.published[index];
    check(value.topic == kPrefix + "ack" && !value.retained, "ACK route/retained flag incorrect");
    DynamicJsonDocument doc(2048);
    check(!deserializeJson(doc, value.payload) && doc.is<JsonObject>() && doc.size() == 7,
          "ACK is not an exact seven-field JSON object");
    check(doc["device_id"] == kId && doc["command_id"] == id && doc["command"] == action,
          "ACK lost paired device/command identity");
    check(doc["command_seq"].is<JsonString>() && doc["command_seq"] == sequence,
          "ACK sequence is not the exact decimal string");
    check(doc["command_session"] == session && doc["accepted"].is<bool>() &&
          doc["accepted"] == accepted && doc["reason"] == reason, "ACK session/result/reason incorrect");
}

struct HandlerCapture {
    BrainNetwork* network;
    std::vector<babytech::boardlink::CloudCommand> commands;
    std::vector<babytech::boardlink::CloudStop> stops;
    std::vector<uint32_t> generations;
    std::vector<uint32_t> times;
    bool expireInsideHandler = false;
    babytech::cloud::Freshness finalFreshness = babytech::cloud::Freshness::Disconnected;

    explicit HandlerCapture(BrainNetwork& owner) : network(&owner) {}
    void checkEntry(const char* session, uint32_t generation, uint32_t sampledAt, uint16_t ttl, uint32_t now) {
        check(!fake::io.inWorker, "product handler ran on MQTT worker instead of UI caller");
        check(now == millis(), "handler did not receive poll clock");
        const auto freshness = network->checkFreshness(session, generation, sampledAt, ttl);
        check(freshness == babytech::cloud::Freshness::Current || freshness == babytech::cloud::Freshness::Expired,
              "product handler received untrusted session/generation");
        generations.push_back(generation);
        times.push_back(now);
        if (expireInsideHandler) fake::io.now = sampledAt + 5001u;
        finalFreshness = network->checkFreshness(session, generation, sampledAt, ttl);
    }
    static void command(void* context, const babytech::boardlink::CloudCommand& value,
                        uint32_t generation, uint32_t now) {
        auto& self = *static_cast<HandlerCapture*>(context);
        self.checkEntry(value.session, generation, value.sampledAtMs, value.ttlMs, now);
        self.commands.push_back(value);
    }
    static void stopCommand(void* context, const babytech::boardlink::CloudStop& value,
                            uint32_t generation, uint32_t now) {
        auto& self = *static_cast<HandlerCapture*>(context);
        self.checkEntry(value.session, generation, value.sampledAtMs, value.ttlMs, now);
        self.stops.push_back(value);
    }
    size_t count() const { return commands.size() + stops.size(); }
    void install() { network->setProductHandlers(command, stopCommand, this); }
};

using babytech::boardlink::ProductContext;

ProductContext fullContext(bool maximum = false) {
    ProductContext value;
    std::strcpy(value.deviceId, kId);
    value.profileVersion = 2147483647;
    std::strcpy(value.babyId, "full-baby-identity");
    std::strcpy(value.babyName, "Full baby name, not a display label");
    std::strcpy(value.formulaBrand, "Full formula brand, not a display label");
    if (maximum) {
        // Maximum byte lengths with complete 3-/4-byte code points.
        for (size_t i = 0; i < 96; i += 3) std::memcpy(value.babyId + i, "\xE5\xAE\x9D", 3);
        value.babyId[96] = 0;
        for (size_t i = 0; i < 320; i += 4) std::memcpy(value.babyName + i, "\xF0\x9F\x98\x80", 4);
        value.babyName[320] = 0;
        for (size_t i = 0; i < 480; i += 4) std::memcpy(value.formulaBrand + i, "\xF0\x9F\x8D\xBC", 4);
        value.formulaBrand[480] = 0;
    }
    value.waterMl = 180;
    value.temperatureC = 45;
    value.powderGPer100Ml = 25.5000019f;
    return value;
}

ProductContext tombstone() {
    ProductContext value;
    std::strcpy(value.deviceId, kId);
    value.profileVersion = 72;
    value.cleared = true;
    return value;
}

std::string contextJson(const ProductContext& value) {
    // Build existing Cloud JSON, including metadata the semantic decoder ignores.
    DynamicJsonDocument doc(4096);
    doc["type"] = "feeding_context";
    doc["device_id"] = value.deviceId;
    doc["profile_version"] = value.profileVersion;
    doc["cleared"] = value.cleared;
    doc["updated_at"] = "2026-10-07T12:00:00+08:00";
    char powder[32]{};
    if (!value.cleared) {
        doc["baby_id"] = value.babyId;
        doc["baby_name"] = value.babyName;
        doc["formula_brand"] = value.formulaBrand;
        doc["water_ml"] = value.waterMl;
        doc["temp"] = value.temperatureC;
        std::snprintf(powder, sizeof(powder), "%.9g", double(value.powderGPer100Ml));
        doc["powder_g_per_100ml"] = serialized(static_cast<const char*>(powder));
    }
    std::string json;
    serializeJson(doc, json);
    return json;
}

struct ContextCapture {
    std::vector<ProductContext> values;
    std::vector<uint32_t> generations;
    std::vector<uint32_t> times;
    const ProductContext* scratch = nullptr;
    std::vector<std::string>* order = nullptr;
    static void context(void* owner, const ProductContext& value, uint32_t generation, uint32_t now) {
        auto& self = *static_cast<ContextCapture*>(owner);
        check(!fake::io.inWorker, "context callback escaped UI loop");
        check(now == millis() && generation, "context callback lost poll clock/generation");
        check(babytech::boardlink::validProductContext(value), "context callback received invalid data");
        // Compare addresses only while the new reference is live, never retain
        // or dereference the previous callback's borrowed data.
        check(!self.scratch || self.scratch == &value, "context callback did not reuse member scratch");
        self.scratch = &value;
        self.values.push_back(value);
        self.generations.push_back(generation);
        self.times.push_back(now);
        if (self.order) self.order->push_back("context");
    }
    void install(BrainNetwork& network) { network.setContextHandler(context, this); }
    void matches(size_t index, const ProductContext& expected) const {
        check(index < values.size() && babytech::boardlink::sameProductContext(values[index], expected),
              "context callback truncated/changed full semantic fields");
    }
};

void contextDelivery(const std::string& mode) {
    BrainNetwork network;
    ContextCapture capture;
    if (mode != "readonly") capture.install(network);
    begin(network);
    const ProductContext expected = mode == "tombstone" ? tombstone() : fullContext(mode == "utf8-max");
    const auto original = fake::io.preferences;
    const size_t allocations = fake::io.allocationCalls;
    const uint32_t opened = millis();
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) {
            poll(network);
            // Deliver via the real worker's MQTT callback before the next UI poll.
            fake::io.incoming.push_back({kPrefix + "config", contextJson(expected)});
        } else if (tick == 2) {
            check(capture.values.empty(), "MQTT receive invoked context handler directly");
            if (mode == "session-expired") fake::io.now = opened + babytech::cloud::kSessionLifetimeMs;
            if (mode == "ttl-expired") fake::io.now += 5001u;
            const unsigned sends = fake::io.queueSendCalls;
            poll(network);
            check(fake::io.queueSendCalls == sends, "context enqueued a fabricated ACK/publication");
            if (mode == "readonly") {
                receive("config", contextJson(tombstone()));
                poll(network);
                check(capture.values.empty(), "missing context handler mutated state");
                capture.install(network);
                poll(network);
                check(capture.values.empty(), "registration replayed previously consumed context");
                receive("config", contextJson(expected));
                poll(network);
            }
            check(capture.values.size() == 1, "valid context not delivered exactly once");
            capture.matches(0, expected);
            poll(network);
            check(capture.values.size() == 1, "context was replayed on next poll");
            check(fake::io.allocationCalls == allocations, "context added RTOS queue/task/lock");
            check(fake::io.preferences == original && ackPackets().empty(), "context wrote NVS/fabricated ACK");
            if (mode == "offline") {
                fake::io.clients.front()->dropConnection();
                fake::io.connectOk = false;
                return;
            }
            workerOnly();
            stop();
        } else {
            check(mode == "offline" && !network.connected(), "offline context fixture did not disconnect");
            poll(network);
            check(capture.values.size() == 1 && ackPackets().empty(), "disconnect replayed/ACKed context");
            capture.matches(0, expected);
            workerOnly();
            stop();
        }
    };
    fake::runWorker();
}

void contextWireBoundary() {
    BrainNetwork network;
    ContextCapture capture;
    capture.install(network);
    begin(network);
    const auto expected = fullContext(true);
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) poll(network);
        else if (tick == 2) {
            const auto base = contextJson(expected);
            size_t delivered = 0;
            for (size_t size : {size_t(1535), size_t(1536), size_t(2047), size_t(2048)}) {
                check(base.size() < size, "context boundary fixture too large");
                receive("config", std::string(size - base.size(), ' ') + base);
                poll(network);
                if (size < 2048) ++delivered;
                check(capture.values.size() == delivered, "context wire boundary accepted/rejected wrong size");
            }
            check(capture.values.size() == 3, "context ingress lost exact 2047-byte boundary");
            for (size_t i = 0; i < 3; ++i) capture.matches(i, expected);
            check(ackPackets().empty(), "context boundary generated ACK");
            stop();
        }
    };
    fake::runWorker();
}

void contextRejected(const std::string& mode) {
    BrainNetwork network;
    ContextCapture capture;
    capture.install(network);
    begin(network);
    const auto expected = fullContext();
    const auto base = contextJson(expected);
    DynamicJsonDocument doc(4096);
    check(!deserializeJson(doc, base), "bad context fixture");
    std::string route = kPrefix + "config";
    std::string bad;
    if (mode == "device") doc["device_id"] = "other";
    else if (mode == "topic") route = "devices/other/config";
    else if (mode == "nested-topic") route = kPrefix + "nested/config";
    else if (mode == "command-topic") route = kPrefix + "command";
    else if (mode == "extra") doc["extra"] = true;
    else if (mode == "missing") doc.remove("baby_id");
    else if (mode == "type") doc["water_ml"] = "180";
    else if (mode == "range") doc["powder_g_per_100ml"] = 51;
    else if (mode == "version") doc["profile_version"] = 0;
    else if (mode == "cleared") doc["cleared"] = "true";
    else if (mode == "tombstone-fields") doc["cleared"] = true;
    else if (mode == "name-long") doc["baby_name"] = std::string(321, 'n');
    else if (mode == "brand-long") doc["formula_brand"] = std::string(481, 'b');
    else if (mode == "utf8") doc["baby_name"] = std::string("\xF0\x9F\x98", 3);
    else if (mode == "metadata") doc["updated_at"] = 123;
    else if (mode == "duplicate") bad = base.substr(0, base.size() - 1) + ",\"water_ml\":200}";
    else if (mode == "json") bad = "{bad";
    else if (mode == "trailing") bad = base + "{}";
    else if (mode == "nul") bad = base + std::string("\0hidden", 7);
    else if (mode == "escaped-nul") doc["baby_name"] = std::string("a\0b", 3);
    else throw std::runtime_error("unknown context rejection fixture");
    if (bad.empty()) serializeJson(doc, bad);
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) poll(network);
        else if (tick == 2) {
            fake::io.clients.front()->deliver(route, bad);
            poll(network);
            check(capture.values.empty() && ackPackets().empty(), "invalid context reached callback/ACK");
            receive("config", base);
            poll(network);
            receive("config", contextJson(tombstone()));
            poll(network);
            check(capture.values.size() == 2, "invalid input blocked later valid context/tombstone");
            capture.matches(0, expected);
            capture.matches(1, tombstone());
            stop();
        }
    };
    fake::runWorker();
}

void contextRegistration() {
    BrainNetwork network;
    ContextCapture first, replacement;
    HandlerCapture products(network);
    first.install(network);
    products.install();
    begin(network);
    const auto expected = fullContext();
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) poll(network);
        else if (tick == 2) {
            const auto session = token();
            receive("config", contextJson(expected));
            poll(network);
            first.matches(0, expected);
            receive("config", contextJson(tombstone()));
            network.setContextHandler(nullptr, nullptr);
            poll(network);
            check(first.values.size() == 1, "unregistered handler still called");
            receive("command", cloudCommand("clean", session, millis()));
            poll(network);
            check(products.commands.size() == 1, "context unregistration unbound product handler");
            receive("config", contextJson(tombstone()));
            replacement.install(network);
            poll(network);
            replacement.matches(0, tombstone());
            check(first.values.size() == 1, "re-registration used previous callback owner");
            network.setProductHandlers(nullptr, nullptr, nullptr);
            receive("config", contextJson(expected));
            poll(network);
            replacement.matches(1, expected);
            products.install();
            receive("command", cloudCommand("stop", session, millis()));
            receive("config", contextJson(tombstone()));
            poll(network);
            check(products.stops.size() == 1 && replacement.values.size() == 3,
                  "product re-registration changed context binding");
            replacement.matches(2, tombstone());
            first.matches(0, expected); // copied data survives scratch reuse
            check(ackPackets().empty(), "handler registration fabricated ACK");
            stop();
        }
    };
    fake::runWorker();
}

void contextGeneration(bool deferred) {
    BrainNetwork network;
    ContextCapture capture;
    capture.install(network);
    begin(network);
    const auto expected = fullContext();
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) poll(network);
        else if (tick == 2) {
            receive("config", contextJson(expected));
            poll(network);
            check(capture.values.size() == 1, "initial context missing");
            if (deferred) fake::io.deferNextQueueSend = true;
            receive("config", contextJson(tombstone()));
            fake::io.clients.front()->dropConnection();
            fake::io.now += 5000u;
        } else if (tick == 3) {
            check(network.connected() && fake::io.connectCalls == 2, "context test did not reconnect");
            if (deferred) fake::completeDeferredSends();
            poll(network);
            check(capture.values.size() == 1, "old-generation context/tombstone dispatched");
            receive("config", contextJson(tombstone()));
            poll(network);
            check(capture.values.size() == 2 && capture.generations[0] != capture.generations[1],
                  "new generation did not deliver context with original generation");
            capture.matches(0, expected);
            capture.matches(1, tombstone());
            check(ackPackets().empty(), "generation handling fabricated context ACK");
            stop();
        }
    };
    fake::runWorker();
}

void contextPriority(bool tombstoneLast) {
    BrainNetwork network;
    ContextCapture capture;
    HandlerCapture products(network);
    std::vector<std::string> order;
    capture.order = &order;
    capture.install(network);
    struct OrderedProducts {
        HandlerCapture& capture;
        std::vector<std::string>& order;
        static void command(void* owner, const babytech::boardlink::CloudCommand& value,
                            uint32_t generation, uint32_t now) {
            auto& self = *static_cast<OrderedProducts*>(owner);
            self.order.push_back("command");
            HandlerCapture::command(&self.capture, value, generation, now);
        }
        static void stopCommand(void* owner, const babytech::boardlink::CloudStop& value,
                                uint32_t generation, uint32_t now) {
            auto& self = *static_cast<OrderedProducts*>(owner);
            self.order.push_back("stop");
            HandlerCapture::stopCommand(&self.capture, value, generation, now);
        }
    } ordered{products, order};
    network.setProductHandlers(OrderedProducts::command, OrderedProducts::stopCommand, &ordered);
    begin(network);
    const auto expected = fullContext(true);
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) poll(network);
        else if (tick == 2) {
            const auto session = token();
            receive("config", contextJson(tombstoneLast ? expected : tombstone()));
            // Ordinary FIFO fills with a probe and commands. The second context
            // must replace only the retained slot, never the queued probe/Stop.
            receive("config", probe(session));
            for (unsigned i = 0; i < 3; ++i)
                receive("command", cloudCommand("clean", session, millis(), std::to_string(i + 1)));
            receive("config", contextJson(tombstoneLast ? tombstone() : expected));
            receive("command", cloudCommand("stop", session, millis()));
            poll(network);
            check(order == std::vector<std::string>{"stop", "context"} && products.commands.empty(),
                  "Stop/config/probe priority or three-message poll budget changed");
            check(capture.values.size() == 1, "retained slot delivered superseded context");
            capture.matches(0, tombstoneLast ? tombstone() : expected);
            poll(network);
            check(order == std::vector<std::string>{"stop", "context", "command", "command", "command"},
                  "context/probe displaced ordinary commands");
        } else {
            check(fake::io.published.size() == 2 && ackPackets().empty(), "context altered probe replies/generated ACK");
            check(packet(1)["command_session_challenge"] == kChallenge, "retained context overwrote probe");
            workerOnly();
            stop();
        }
    };
    fake::runWorker();
}

void productHandler(const std::string& fixture) {
    const size_t split = fixture.find('-');
    const std::string action = fixture.substr(0, split);
    const std::string mode = fixture.substr(split + 1);
    const bool isStop = action == "stop";
    const bool dispatch = mode == "current" || mode == "age5000" || mode == "deadline" || mode == "expired";
    BrainNetwork network;
    HandlerCapture capture(network);
    capture.install();
    if (mode == "missing") network.setProductHandlers(isStop ? HandlerCapture::command : nullptr,
                                                      isStop ? nullptr : HandlerCapture::stopCommand, &capture);
    if (mode == "unregister") network.setProductHandlers(nullptr, nullptr, nullptr);
    capture.expireInsideHandler = mode == "deadline";
    begin(network);
    const std::string id = std::string(125, 'x') + "\"\\\n";
    const std::string sequence = "9223372036854775807";
    std::string session, input;
    uint32_t sample = 0;
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) poll(network);
        else if (tick == 2) {
            session = token();
            sample = millis();
            input = cloudCommand(action, mode == "wrong" ? kChallenge : session, sample, sequence, id);
            fake::io.incoming.push_back({kPrefix + "command", input});
        } else if (tick == 3) {
            check(capture.count() == 0 && ackPackets().empty(), "MQTT callback invoked handler or published early ACK");
            if (mode == "age5000") fake::io.now = sample + 5000u;
            if (mode == "expired") fake::io.now = sample + 5001u;
            const auto sends = fake::io.queueSendCalls;
            poll(network);
            const bool rejected = mode == "missing" || mode == "unregister";
            check(fake::io.queueSendCalls == sends + (rejected ? 1u : 0u),
                  "handler path emitted premature ACK or lost read-only rejection");
            check(capture.count() == (dispatch ? 1u : 0u), "wrong handler dispatch count");
            if (dispatch) {
                check(capture.generations.size() == 1 && capture.generations[0] != 0,
                      "handler generation missing");
                if (isStop) {
                    const auto& stop = capture.stops.front();
                    check(stop.deviceId == std::string(kId) && stop.commandId == id &&
                          stop.sequence == babytech::v4::kMaxSequence && stop.session == session &&
                          stop.sampledAtMs == sample && stop.ttlMs == 5000, "Stop handler input lost original envelope");
                } else {
                    babytech::boardlink::CloudCommand expected;
                    check(babytech::boardlink::decodeCloudCommand(reinterpret_cast<const uint8_t*>(input.data()),
                              input.size(), kId, expected), "handler fixture was not production-decodable");
                    const auto& command = capture.commands.front();
                    check(babytech::boardlink::sameProductRequest(command.request, expected.request) &&
                          command.session == session && command.sampledAtMs == sample && command.ttlMs == 5000,
                          "ordinary handler lost original identity/frozen recipe");
                }
                check(capture.finalFreshness == (mode == "deadline" || mode == "expired" ? babytech::cloud::Freshness::Expired :
                      babytech::cloud::Freshness::Current), "runtime deadline recheck did not use production clock");
                check(network.checkFreshness(session.c_str(), capture.generations[0], sample, 4999) ==
                      babytech::cloud::Freshness::InvalidTtl, "runtime freshness wrapper ignored invalid TTL");
            }
            const auto after = fake::io.queueSendCalls;
            poll(network);
            check(capture.count() == (dispatch ? 1u : 0u) && fake::io.queueSendCalls == after,
                  "drained handler request dispatched/ACKed twice");
        } else {
            const auto acks = ackPackets();
            const bool rejected = mode == "missing" || mode == "unregister";
            check(acks.size() == (rejected ? 1u : 0u), "handler produced fabricated or duplicate ACK");
            if (rejected) commandAck(acks.front(), action, session,
                mode == "expired" ? "request_expired" : "integration_not_ready", sequence, id);
            noSideEffects(); workerOnly(); stop();
        }
    };
    fake::runWorker();
}

void commandAccepted(const std::string& action, bool maximum) {
    BrainNetwork network;
    begin(network);
    const auto raw = fake::nvs.values;
    const auto cloud = fake::io.preferences;
    const Status motion = readyMotion();
    const std::string sequence = maximum ? "9223372036854775807" : "42";
    const std::string id = maximum ? std::string(125, 'x') + "\"\\\n" : "cloud-test";
    std::string session;
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) poll(network, &motion, true);
        else if (tick == 2) {
            session = token();
            fake::io.incoming.push_back({kPrefix + "command", cloudCommand(action, session, millis(), sequence, id)});
        } else if (tick == 3) {
            check(fake::io.published.size() == 1, "MQTT callback published/dispatched before coordinator poll");
            const auto sends = fake::io.queueSendCalls;
            poll(network, &motion, true);
            check(fake::io.queueSendCalls == sends + 1, "valid command did not enqueue exactly one ACK");
        } else {
            const auto acks = ackPackets();
            check(acks.size() == 1 && fake::io.published.size() == 2, "valid command ACK missing/duplicated");
            commandAck(acks.front(), action, session, "integration_not_ready", sequence, id);
            check(fake::nvs.values == raw && fake::io.preferences == cloud, "command changed persistent state");
            check(fake::io.clients.front()->callbackChanges() == 1, "command path installed another MQTT callback");
            noSideEffects(); workerOnly(); stop();
        }
    };
    fake::runWorker();
}

void commandFreshness(const std::string& fixture) {
    const bool isStop = fixture.compare(0, 5, "stop-") == 0;
    const std::string mode = fixture.substr(isStop ? 5 : 6);
    const std::string action = isStop ? "stop" : "clean";
    if (mode.compare(0, 5, "wrap-") == 0 || mode == "uptime-zero") fake::io.now = UINT32_MAX - 1000u;
    const uint32_t openedAt = millis();
    BrainNetwork network;
    begin(network);
    std::string session;
    bool expired = false;
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) poll(network);
        else if (tick == 2) {
            session = token();
            uint32_t sampledAt = millis();
            if (mode == "age-4999") fake::io.now = sampledAt + 4999u;
            else if (mode == "age-5000" || mode == "wrap-current") fake::io.now = sampledAt + 5000u;
            else if (mode == "age-5001" || mode == "wrap-expired") {
                fake::io.now = sampledAt + 5001u; expired = true;
            } else if (mode == "future") { ++sampledAt; expired = true; }
            else if (mode == "pre-session") { sampledAt = openedAt - 1u; expired = true; }
            else if (mode == "uptime-zero") { sampledAt = 0; fake::io.now = 0; }
            else if (mode != "queued-expired") throw std::runtime_error("unknown freshness fixture");
            receive("command", cloudCommand(action, session, sampledAt));
            if (mode == "queued-expired") { fake::io.now = sampledAt + 5001u; expired = true; }
            poll(network);
        } else {
            const auto acks = ackPackets();
            check(acks.size() == 1, "freshness decision lost/duplicated its ACK");
            commandAck(acks.front(), action, session, expired ? "request_expired" : "integration_not_ready");
            noSideEffects(); workerOnly(); stop();
        }
    };
    fake::runWorker();
}

std::vector<std::string> invalidCommands(const std::string& action, const std::string& session) {
    const std::string valid = cloudCommand(action, session, millis());
    std::vector<std::string> rejected = {"", "{bad", "[]", "null", valid + " trailing"};
    const auto mutate = [&](const char* field, const auto& value) {
        DynamicJsonDocument doc(4096);
        check(!deserializeJson(doc, valid), "invalid base command fixture");
        doc[field] = value;
        std::string output; serializeJson(doc, output); rejected.push_back(output);
    };
    DynamicJsonDocument base(4096);
    check(!deserializeJson(base, valid), "invalid base command fixture");
    for (JsonPairConst field : base.as<JsonObjectConst>()) {
        DynamicJsonDocument doc(4096);
        check(!deserializeJson(doc, valid), "invalid base command fixture");
        doc.remove(field.key().c_str());
        std::string output; serializeJson(doc, output); rejected.push_back(output);
        mutate(field.key().c_str(), nullptr);
        mutate(field.key().c_str(), true);
        // Duplicate even identical values: ArduinoJson alone would silently coalesce them.
        std::string duplicate = valid;
        duplicate.pop_back();
        duplicate += ",\"" + std::string(field.key().c_str()) + "\":";
        serializeJson(field.value(), duplicate);
        duplicate += '}';
        rejected.push_back(duplicate);
    }
    for (const char* field : {"device_id", "command_id", "command", "command_seq", "command_session"})
        mutate(field, 42);
    for (const char* field : {"device_uptime_ms", "ttl_ms"}) mutate(field, "5000");
    mutate("extra", 1);
    mutate("ratio", 1.5);
    mutate("command", "initialize");
    mutate("command", "unknown");
    mutate("device_id", "unpaired-device");
    mutate("command_id", "");
    mutate("command_id", std::string(129, 'x'));
    mutate("command_id", std::string("ok\0hidden", 9));
    for (const char* seq : {"", "0", "01", "-1", "+1", "1.0", "1e3", "9223372036854775808", "18446744073709551615"})
        mutate("command_seq", seq);
    for (const std::string& bad : {std::string(), std::string(32, '0'), std::string(32, 'A'),
                                  std::string(31, 'a'), std::string(33, 'a'), std::string(32, 'g')})
        mutate("command_session", bad);
    for (int ttl : {0, 4999, 5001, -1}) mutate("ttl_ms", ttl);
    mutate("device_uptime_ms", -1);
    mutate("device_uptime_ms", uint64_t(UINT32_MAX) + 1);
    mutate("device_uptime_ms", 1.5);
    // Legacy commands have no v4 envelope, including legacy emergency Stop.
    rejected.push_back("{\"command\":\"" + action + "\",\"command_id\":\"legacy\",\"device_id\":\"" + kId + "\"}");
    if (action == "prepare") {
        mutate("baby_id", "");
        mutate("feeding_context_profile_version", 0);
        mutate("feeding_context_profile_version", uint64_t(INT32_MAX) + 1);
        for (const char* field : {"feeding_context_profile_version", "water_ml", "temp", "powder_g_per_100ml"})
            mutate(field, "45");
        mutate("water_ml", 29); mutate("water_ml", 501); mutate("water_ml", 180.5);
        mutate("powder_g_per_100ml", 0.5); mutate("powder_g_per_100ml", 50.1);
    }
    if (action == "prepare" || action == "set_target_temp") {
        mutate("temp", 34); mutate("temp", 61); mutate("temp", 45.5);
    } else {
        mutate("temp", 45); // In particular, Stop accepts exactly its seven envelope keys.
        mutate("baby_id", "baby-test");
    }
    return rejected;
}

void commandRejected(const std::string& action, bool handlers = false) {
    BrainNetwork network;
    HandlerCapture capture(network);
    if (handlers) capture.install();
    begin(network);
    std::string session;
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) poll(network);
        else if (tick == 2) {
            session = token();
            auto rejected = invalidCommands(action, session);
            // Valid syntax but a different session is silent even if its sample is expired.
            rejected.push_back(cloudCommand(action, kChallenge, millis()));
            rejected.push_back(cloudCommand(action, kChallenge, millis() - 5001u));
            for (size_t i = 0; i < rejected.size(); ++i) {
                receive("command", rejected[i]);
                const auto sends = fake::io.queueSendCalls;
                poll(network);
                if (fake::io.queueSendCalls != sends)
                    throw std::runtime_error("rejected " + action + " fixture " + std::to_string(i) + " enqueued output: " + rejected[i]);
            }
            // A positive control ensures rejection is not explained by a disabled receiver.
            receive("command", cloudCommand(action, session, millis()));
            poll(network);
        } else {
            const auto acks = ackPackets();
            check(acks.size() == (handlers ? 0u : 1u) && fake::io.published.size() == (handlers ? 1u : 2u),
                  "invalid commands leaked ACK/output");
            check(capture.count() == (handlers ? 1u : 0u), "invalid commands reached product handler");
            if (!handlers) commandAck(acks.front(), action, session);
            noSideEffects(); workerOnly(); stop();
        }
    };
    fake::runWorker();
}

void commandTopics() {
    BrainNetwork network;
    begin(network);
    std::string session;
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) poll(network);
        else if (tick == 2) {
            session = token();
            for (const char* action : {"clean", "stop"}) {
                const auto value = cloudCommand(action, session, millis());
                for (const auto& topic : {"devices/unpaired-device/command", "devices/bt-brain-test-extra/command",
                                          "devices/bt-brain-test/nested/command", "devices/bt-brain-test/command/extra",
                                          "devices/bt-brain-test/commands", "devices/bt-brain-test/config"}) {
                    fake::io.clients.front()->deliver(topic, value);
                    const auto sends = fake::io.queueSendCalls;
                    poll(network);
                    check(fake::io.queueSendCalls == sends, "command accepted on an unrelated topic");
                }
            }
            receive("command", cloudCommand("clean", session, millis()));
            poll(network);
        } else {
            const auto acks = ackPackets();
            check(acks.size() == 1 && fake::io.published.size() == 2, "wrong topic produced output");
            commandAck(acks.front(), "clean", session);
            noSideEffects(); workerOnly(); stop();
        }
    };
    fake::runWorker();
}

void commandGeneration(const std::string& fixture) {
    const bool isStop = fixture.compare(0, 5, "stop-") == 0;
    const std::string mode = fixture.substr(isStop ? 5 : 6);
    const std::string action = isStop ? "stop" : "clean";
    const bool outbound = mode == "ack-deferred";
    BrainNetwork network;
    begin(network);
    std::string previous, current;
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) poll(network);
        else if (tick == 2) {
            previous = token();
            if (mode == "inbound-deferred") fake::io.deferNextQueueSend = true;
            receive("command", cloudCommand(action, previous, millis()));
            if (outbound) {
                fake::io.deferNextQueueSend = true;
                poll(network);
            }
            fake::io.clients.front()->dropConnection();
            fake::io.now += 5000u;
        } else if (tick == 3) {
            check(network.connected() && fake::io.connectCalls == 2, "command test did not reconnect");
            if (outbound || mode == "inbound-deferred") fake::completeDeferredSends();
            poll(network);
        } else if (tick == 4) {
            check(ackPackets().empty() && fake::io.published.size() == 2, "previous-generation command/ACK survived reconnect");
            current = token(1);
            check(current != previous, "reconnect reused command session");
            receive("command", cloudCommand(action, previous, millis()));
            const auto sends = fake::io.queueSendCalls;
            poll(network);
            check(fake::io.queueSendCalls == sends, "old session accepted with new inbound generation");
            receive("command", cloudCommand(action, current, millis()));
            poll(network);
        } else {
            const auto acks = ackPackets();
            check(acks.size() == 1 && fake::io.published.size() == 3, "new session failed to recover command ACKs");
            commandAck(acks.front(), action, current);
            noSideEffects(); workerOnly(); stop();
        }
    };
    fake::runWorker();
}

void handlerGeneration(const std::string& fixture) {
    const bool isStop = fixture.compare(0, 5, "stop-") == 0;
    const std::string action = isStop ? "stop" : "clean";
    const bool deferred = fixture.substr(isStop ? 5 : 6) == "deferred";
    BrainNetwork network;
    HandlerCapture capture(network);
    capture.install();
    begin(network);
    std::string previous, current;
    uint32_t sample = 0;
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) poll(network);
        else if (tick == 2) {
            previous = token();
            sample = millis();
            receive("command", cloudCommand(action, previous, sample));
            poll(network);
            check(capture.count() == 1, "initial handler generation not observed");
            if (deferred) fake::io.deferNextQueueSend = true;
            receive("command", cloudCommand(action, previous, sample, "43", "old-queued"));
            fake::io.clients.front()->dropConnection();
            fake::io.now += 5000u;
        } else if (tick == 3) {
            check(network.connected() && fake::io.connectCalls == 2, "handler generation test did not reconnect");
            if (deferred) fake::completeDeferredSends();
            poll(network);
            check(capture.count() == 1, "old-generation queued command reached handler");
        } else if (tick == 4) {
            current = token(1);
            check(current != previous && ackPackets().empty(), "old-generation command leaked ACK/reused session");
            check(network.checkFreshness(previous.c_str(), capture.generations[0], sample, 5000) ==
                  babytech::cloud::Freshness::WrongSession, "runtime old-generation freshness incorrectly Current");
            receive("command", cloudCommand(action, previous, millis()));
            poll(network);
            check(capture.count() == 1, "old session with current inbound generation dispatched");
            receive("command", cloudCommand(action, current, millis(), "44", "new-current"));
            poll(network);
            check(capture.count() == 2 && capture.generations[0] != capture.generations[1],
                  "new-generation handler did not recover/pass original generation");
            poll(network);
        } else {
            check(capture.count() == 2 && ackPackets().empty(), "handler generation produced duplicate dispatch/early ACK");
            noSideEffects(); workerOnly(); stop();
        }
    };
    fake::runWorker();
}

void runtimeAck(const std::string& mode) {
    BrainNetwork network;
    HandlerCapture capture(network);
    capture.install();
    check(!network.publishAck("original", "clean", 42, kChallenge, true, "accepted"),
          "unstarted runtime ACK accepted");
    begin(network);
    std::string session;
    const std::string id = std::string(125, 'x') + "\"\\\n";
    const char* reason = mode == "rejected" ? "busy" : mode == "boolean" ? "ack_timeout" : "accepted";
    const bool accepted = mode != "rejected";
    const bool reconnect = mode == "reconnect" || mode == "offline" || mode == "deferred";
    bool retry = false;
    const auto publish = [&] {
        const bool worker = fake::io.inWorker;
        fake::io.inWorker = false;
        const bool result = network.publishAck(id.c_str(), "clean", babytech::v4::kMaxSequence,
                                               session.c_str(), accepted, reason);
        fake::io.inWorker = worker;
        return result;
    };
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) poll(network);
        else if (tick == 2) {
            session = token();
            receive("command", cloudCommand("clean", session, millis(), "9223372036854775807", id));
            poll(network);
            check(capture.count() == 1 && ackPackets().empty(), "runtime result preceded real handler receipt");
            if (reconnect) {
                if (mode == "deferred") {
                    fake::io.deferNextQueueSend = true;
                    check(publish(), "deferred original ACK did not enqueue");
                }
                if (mode == "offline") {
                    WiFi.state = 0;
                    return;
                }
                fake::io.clients.front()->dropConnection();
                fake::io.now += 5000u;
            } else if (mode == "full") {
                for (unsigned i = 0; i < 8; ++i) { receive("config", probe(session)); poll(network); }
                check(!publish(), "full real outbound queue accepted original result ACK");
                retry = true;
            } else {
                fake::io.now += 5001u;
                check(network.checkFreshness(session.c_str(), capture.generations[0],
                      capture.commands[0].sampledAtMs, 5000) == babytech::cloud::Freshness::Expired,
                      "runtime result test did not exceed original deadline");
                check(publish(), "determined result rejected by new-action TTL");
            }
        } else if (tick == 3 && mode == "offline") {
            check(!network.connected() && !publish(), "offline ACK not reported false to retaining caller");
            check(ackPackets().empty(), "offline runtime ACK leaked");
            connectedWifi();
        } else if (tick == (mode == "offline" ? 4u : 3u) && reconnect) {
            check(network.connected() && fake::io.connectCalls == 2, "runtime result test did not reconnect");
            if (mode == "deferred") fake::completeDeferredSends();
            poll(network);
            check(network.checkFreshness(session.c_str(), capture.generations[0],
                  capture.commands[0].sampledAtMs, 5000) == babytech::cloud::Freshness::WrongSession,
                  "old runtime result still qualifies as new action");
            check(publish(), "original result cannot publish on current transport generation");
        } else if (tick == 3 && mode == "full") {
            check(retry && ackPackets().empty() && publish(), "retained ACK could not retry after queue drain");
        } else if ((mode != "full" && tick >= (mode == "offline" || mode == "deferred" ? 5u : reconnect ? 4u : 3u)) ||
                   (mode == "full" && tick == 8)) {
            const auto acks = ackPackets();
            check(acks.size() == 1, "runtime original result ACK lost/duplicated");
            commandAck(acks.front(), "clean", session, reason, "9223372036854775807", id, accepted);
            if (reconnect) check(token(1) != session, "reconnect ACK changed old session into new one");
            check(capture.count() == 1, "retained original result replayed action");
            noSideEffects(); workerOnly(); stop();
        }
    };
    fake::runWorker();
}

void ackValidation() {
    BrainNetwork network;
    begin(network);
    std::string session;
    const std::string maxReason(64, 'R');
    const std::string maxId(128, '\x01');
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) poll(network);
        else if (tick == 2) {
            session = token();
            const auto invalid = [&](const char* id, const char* command, uint64_t seq, const char* original,
                                      const char* reason) {
                const auto sends = fake::io.queueSendCalls;
                check(!network.publishAck(id, command, seq, original, false, reason) &&
                      fake::io.queueSendCalls == sends, "invalid runtime ACK enqueued/truncated fields");
            };
            std::array<char, 129> unterminatedId; unterminatedId.fill('x');
            std::array<char, 65> unterminatedReason; unterminatedReason.fill('r');
            for (const char* id : {static_cast<const char*>(nullptr), "", static_cast<const char*>(unterminatedId.data()), "\xc0\xaf"})
                invalid(id, "clean", 42, session.c_str(), "busy");
            for (const char* name : {static_cast<const char*>(nullptr), "", "initialize", "reset", "Clean", "stop ",
                                     "check_firmware_update_extra"})
                invalid("id", name, 42, session.c_str(), "busy");
            for (uint64_t sequence : {uint64_t(0), babytech::v4::kMaxSequence + 1, UINT64_MAX})
                invalid("id", "clean", sequence, session.c_str(), "busy");
            for (const char* original : {static_cast<const char*>(nullptr), "", "abc", "00000000000000000000000000000000",
                                         "ABCDEF1234567890abcdef1234567890", "gggggggggggggggggggggggggggggggg",
                                         "1234567890abcdef1234567890abcdefx"})
                invalid("id", "clean", 42, original, "busy");
            for (const char* reason : {static_cast<const char*>(nullptr), "", "bad reason", "bad\nreason",
                                      "bad-reason", "\xc0\xaf", static_cast<const char*>(unterminatedReason.data())})
                invalid("id", "clean", 42, session.c_str(), reason);
            for (const char* action : {"prepare", "clean", "set_target_temp", "reset_error", "check_firmware_update", "stop"})
                check(network.publishAck(maxId.c_str(), action, babytech::v4::kMaxSequence,
                      session.c_str(), false, maxReason.c_str()), "maximum valid original ACK fields rejected");
        } else if (tick == 5) {
            const auto acks = ackPackets();
            check(acks.size() == 6, "ACK validation lost valid messages/leaked invalid messages");
            const char* actions[] = {"prepare", "clean", "set_target_temp", "reset_error", "check_firmware_update", "stop"};
            for (size_t i = 0; i < 6; ++i) {
                commandAck(acks[i], actions[i], session, maxReason.c_str(), "9223372036854775807", maxId);
                for (unsigned char c : fake::io.published[acks[i]].payload)
                    check(c >= 0x20, "ACK escaped identity emitted raw JSON control byte");
            }
            noSideEffects(); workerOnly(); stop();
        }
    };
    fake::runWorker();
}

void commandDisconnected(const std::string& action) {
    BrainNetwork network;
    begin(network);
    std::string previous, current;
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) poll(network);
        else if (tick == 2) {
            previous = token();
            receive("command", cloudCommand(action, previous, millis()));
            WiFi.state = 0;
        } else if (tick == 3) {
            check(!network.connected(), "disconnected fixture still has an active session");
            const auto sends = fake::io.queueSendCalls;
            poll(network);
            check(fake::io.queueSendCalls == sends && ackPackets().empty(), "disconnected command produced output");
            connectedWifi();
        } else if (tick == 4) poll(network);
        else if (tick == 5) {
            check(ackPackets().empty() && fake::io.published.size() == 2, "disconnected command replayed after recovery");
            current = token(1);
            check(current != previous, "disconnection did not replace session");
            receive("command", cloudCommand(action, current, millis()));
            poll(network);
        } else {
            const auto acks = ackPackets();
            check(acks.size() == 1, "disconnected receiver did not recover");
            commandAck(acks.front(), action, current);
            noSideEffects(); workerOnly(); stop();
        }
    };
    fake::runWorker();
}

void commandBudget(bool priorityStop) {
    BrainNetwork network;
    begin(network);
    std::string session;
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) poll(network);
        else if (tick == 2) {
            session = token();
            for (unsigned i = 1; i <= 4; ++i)
                receive("command", cloudCommand("clean", session, millis(), std::to_string(i), "queued-" + std::to_string(i)));
            if (priorityStop) receive("command", cloudCommand("stop", session, millis(), "5", "priority-stop"));
            const auto sends = fake::io.queueSendCalls;
            poll(network);
            check(fake::io.queueSendCalls == sends + 3, "command poll budget is not three");
        } else if (tick == 3) {
            const auto acks = ackPackets();
            check(acks.size() == 2, "worker did not publish two command ACKs");
            commandAck(acks[0], priorityStop ? "stop" : "clean", session, "integration_not_ready",
                       priorityStop ? "5" : "1", priorityStop ? "priority-stop" : "queued-1");
            const auto sends = fake::io.queueSendCalls;
            poll(network);
            check(fake::io.queueSendCalls == sends + (priorityStop ? 2u : 1u), "remaining commands not deferred to next poll");
            const auto drained = fake::io.queueSendCalls;
            poll(network);
            check(fake::io.queueSendCalls == drained, "drained commands produced duplicate ACKs");
        } else if (priorityStop && tick == 4) check(ackPackets().size() == 4, "worker publish budget changed");
        else {
            const auto acks = ackPackets();
            check(acks.size() == (priorityStop ? 5u : 4u), "command budget lost/duplicated ACKs");
            for (unsigned i = 1; i <= 4; ++i)
                commandAck(acks[(priorityStop ? 1u : 0u) + i - 1], "clean", session,
                           "integration_not_ready", std::to_string(i), "queued-" + std::to_string(i));
            noSideEffects(); workerOnly(); stop();
        }
    };
    fake::runWorker();
}

void commandSizeBoundary(const std::string& action) {
    BrainNetwork network;
    begin(network);
    std::string session;
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) poll(network);
        else if (tick == 2) {
            session = token();
            for (size_t size : {size_t(1535), size_t(1536), size_t(2047), size_t(2048)}) {
                const auto id = "size-" + std::to_string(size);
                const auto base = cloudCommand(action, session, millis(), "42", id);
                check(base.size() < size, "boundary fixture exceeds target size");
                receive("command", std::string(size - base.size(), ' ') + base);
                const auto sends = fake::io.queueSendCalls;
                poll(network);
                check(fake::io.queueSendCalls == sends + (size < 2048 ? 1u : 0u),
                      "v4 ingress does not enforce its exact byte boundary");
            }
        } else if (tick == 3) check(ackPackets().size() == 2, "worker publish budget changed");
        else {
            const auto acks = ackPackets();
            check(acks.size() == 3, "boundary commands lost/duplicated an ACK");
            commandAck(acks[0], action, session, "integration_not_ready", "42", "size-1535");
            commandAck(acks[1], action, session, "integration_not_ready", "42", "size-1536");
            commandAck(acks[2], action, session, "integration_not_ready", "42", "size-2047");
            noSideEffects(); workerOnly(); stop();
        }
    };
    fake::runWorker();
}

void cloudFixtures(const char* inputPath, const char* outputPath) {
    check(std::string(inputPath) != outputPath, "fixture input and ACK output must be different files");
    std::ifstream input(inputPath, std::ios::binary);
    check(input.is_open(), "cannot open Cloud fixture JSONL");
    std::vector<std::string> fixtures;
    std::string line;
    while (std::getline(input, line)) {
        check(!line.empty(), "empty Cloud fixture line");
        fixtures.push_back(line);
    }
    check(!input.bad() && input.eof(), "failed to read Cloud fixture JSONL");
    check(!fixtures.empty() && fixtures.size() <= 997, "fixture count exceeds captured-worker schedule or is empty");
    BrainNetwork network;
    begin(network);
    const auto raw = fake::nvs.values;
    const auto cloud = fake::io.preferences;
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) poll(network);
        else if (tick <= fixtures.size() + 1) {
            // Preserve the real Cloud bytes, including identity/session/sample/TTL.
            receive("command", fixtures[tick - 2]);
            poll(network);
        } else stop(); // The worker has now flushed the final fixture's ACK.
    };
    fake::runWorker();
    const auto acks = ackPackets();
    check(acks.size() == fixtures.size(), "real Brain ACK count does not match Cloud fixture count");
    for (size_t i = 0; i < fixtures.size(); ++i) {
        DynamicJsonDocument doc(4096);
        check(!deserializeJson(doc, fixtures[i]) && doc.is<JsonObject>(), "Cloud fixture is not a JSON object");
        check(doc["device_id"] == kId && doc["command"].is<JsonString>() &&
              doc["command_id"].is<JsonString>() && doc["command_seq"].is<JsonString>() &&
              doc["command_session"].is<JsonString>(), "Cloud fixture lacks string command identity");
        commandAck(acks[i], doc["command"].as<std::string>(), doc["command_session"].as<std::string>(),
                   "integration_not_ready", doc["command_seq"].as<std::string>(), doc["command_id"].as<std::string>());
    }
    check(fake::nvs.values == raw && fake::io.preferences == cloud, "Cloud fixtures mutated NVS");
    check(fake::io.clients.front()->callbackChanges() == 1, "fixture path installed another MQTT callback");
    noSideEffects(); workerOnly();
    // Write only captured, verified production ACKs; never synthesize a reply.
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    check(output.is_open(), "cannot open ACK output JSONL");
    for (const size_t index : acks) output << fake::io.published[index].payload << '\n';
    output.close();
    check(!output.fail(), "failed to write ACK output JSONL");
    std::cout << "PASS " << acks.size() << " external Cloud fixtures (real Brain ACKs)\n";
}

void failureThrottle(bool rollover) {
    if (rollover) fake::io.now = UINT32_MAX - 100u;
    BrainNetwork network;
    begin(network);
    const Status motion = readyMotion();
    uint32_t failedAt = 0;
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) {
            failedAt = millis();
            // Exercise a real production enqueue rejection: freshness expires
            // at exactly 1500ms. No private access or artificial queue failure.
            pollAt(network, &motion, true, millis() - 1500u);
        } else if (tick == 2) {
            check(fake::io.published.empty(), "expired sample was accepted");
            fake::io.now = failedAt + 249;
            poll(network, &motion, true);
        } else if (tick == 3) {
            check(fake::io.published.empty(), "failed status retried before 250ms");
            fake::io.now = failedAt + 250;
            pollAt(network, &motion, true, millis() - 1500u);
            poll(network, &motion, true);
        } else if (tick == 4) {
            check(fake::io.published.empty(), "failed retry at 250ms did not restart throttle");
            fake::io.now = failedAt + 500;
            poll(network, &motion, true);
        } else {
            check(fake::io.published.size() == 1, "enqueue recovery lost/duplicated status packets");
            check(packet(0)["device_uptime_ms"] == failedAt + 500, "recovered status is not fresh");
            workerOnly(); stop();
        }
    };
    fake::runWorker();
}
void queueFull() {
    BrainNetwork network;
    begin(network);
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) poll(network);
        else if (tick == 2) {
            const auto session = token();
            for (unsigned i = 0; i < 8; ++i) { receive("config", probe(session)); poll(network); }
            check(fake::io.queueSendFailures == 0, "probe queue filled before eight messages");
            receive("config", probe(session));
            poll(network);
            check(fake::io.queueSendFailures == 1, "ninth probe did not hit real bounded queue");
            fake::io.now += 2000;
            poll(network);
            check(fake::io.queueSendFailures == 1, "full probe FIFO blocked independent periodic status");
        } else if (tick == 3) {
            check(fake::io.published.size() == 3 && !packet(1).containsKey("command_session_challenge"),
                  "periodic status did not get a publish opportunity beside full probe FIFO");
        } else if (tick == 7) {
            check(fake::io.published.size() == 10, "full probe queue corrupted accepted messages");
            for (size_t i = 2; i < 10; ++i)
                check(packet(i)["command_session_challenge"] == kChallenge, "queued probe corrupted");
            workerOnly(); stop();
        }
    };
    fake::runWorker();
}
void publishFailure() {
    BrainNetwork network;
    begin(network);
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) { poll(network); fake::io.publishOk = false; }
        else if (tick == 2) {
            check(!network.connected() && fake::io.published.size() == 1, "MQTT failure did not invalidate session");
            poll(network);
            fake::io.publishOk = true;
            fake::io.now += 5000;
        } else if (tick == 3) poll(network);
        else {
            check(fake::io.published.size() == 2 && token() != token(1), "publish failure did not recover on new session");
            stop();
        }
    };
    fake::runWorker();
}

Status readyMotion() {
    Status value;
    std::strcpy(value.productProgress, "ready");
    std::strcpy(value.babyId, "full-identity-not-display-label");
    std::strcpy(value.snapshot.babyName.data(), "Display label");
    value.snapshot.startEnabled = true;
    value.snapshot.temperatureC = 42;
    value.contextVersion = 72;
    value.lowWaterValid = value.powderValid = true;
    value.powderGrams = 500;
    value.actuatorOperational = value.actuatorConfigValid = true;
    value.actuatorBusHealthy = value.actuatorPositionReferenced = true;
    value.executionAuthorized = value.feedingContextConfigured = true;
    return value;
}

babytech::v4::Pairing terminalPairing() {
    babytech::v4::Pairing pairing;
    pairing.role = babytech::v4::Role::Brain;
    std::strcpy(pairing.deviceId, kId);
    std::strcpy(pairing.epoch, "0123456789abcdef0123456789abcdef");
    std::strcpy(pairing.localPhysicalId, "aabbccddeeff");
    std::strcpy(pairing.peerPhysicalId, "112233445566");
    return pairing;
}

void terminalBytes(babytech::v4::Message& message, const std::string& bytes) {
    check(bytes.size() <= babytech::v4::kMaxMessage, "terminal fixture exceeds buffer");
    std::memcpy(message.payload, bytes.data(), bytes.size());
    message.length = uint16_t(bytes.size());
}

std::unique_ptr<babytech::v4::Message> terminalMessage(const babytech::v4::Pairing& pairing,
                                                     bool local = false, bool completed = true,
                                                     bool simulation = false) {
    using namespace babytech::boardlink;
    auto event = std::make_unique<TerminalEvent>();
    auto& request = event->request;
    request.source = local ? babytech::v4::Source::LocalTouch : babytech::v4::Source::CloudCommand;
    request.command = ProductCommand::Prepare;
    request.sequence = babytech::v4::kMaxSequence;
    std::strcpy(request.deviceId, pairing.deviceId);
    if (local) check(makeLocalCommandId(pairing, request.sequence, request.commandId), "local ID fixture failed");
    else std::strcpy(request.commandId, "cloud-result");
    std::strcpy(request.babyId, "historical-baby");
    request.profileVersion = 7;
    request.waterMl = 120;
    request.temperatureC = 42;
    request.powderGPer100Ml = 25;
    event->targetPowderG = productTargetPowderG(request);
    event->completed = completed;
    event->uptimeMs = 1;
    if (!completed) {
        std::strcpy(event->reason, "reboot_during_feed");
        std::strcpy(event->errorCode, "E_REBOOT_DURING_FEED");
    }
    check(makeProductEventId(pairing, request.source, request.sequence, event->eventId), "event ID fixture failed");
    auto message = std::make_unique<babytech::v4::Message>();
    check(simulation ? encodeBrainSimulationEvent(pairing, *event, *message) :
                       encodeTerminalEvent(pairing, *event, *message), "terminal encoder fixture failed");
    std::string bytes(reinterpret_cast<const char*>(message->payload), message->length);
    // Legal noncanonical bytes expose any accidental decode/re-encode or trim.
    const auto at = bytes.find("historical-baby");
    check(at != std::string::npos, "terminal baby fixture missing");
    bytes.replace(at, std::strlen("historical-baby"), "historical\\u002dbaby");
    terminalBytes(*message, " \n\t" + bytes + "\r\n ");
    message->senderBoot = 17;
    message->receiverBoot = 29;
    message->messageId = 41;
    return message;
}

std::vector<size_t> terminalPackets() {
    std::vector<size_t> packets;
    for (size_t i = 0; i < fake::io.published.size(); ++i)
        if (fake::io.published[i].topic == kPrefix + "event") packets.push_back(i);
    return packets;
}

void terminalTransport(const std::string& mode) {
    BrainNetwork network;
    const auto pairing = terminalPairing();
    auto message = terminalMessage(pairing, mode.compare(0, 5, "local") == 0,
                                   mode.find("failed") == std::string::npos);
    if (mode == "wire-boundary") {
        std::string bytes(reinterpret_cast<const char*>(message->payload), message->length);
        bytes.append(babytech::v4::kMaxMessage - bytes.size(), ' ');
        terminalBytes(*message, bytes);
    }
    const auto original = std::make_unique<babytech::v4::Message>(*message);
    const std::string bytes(reinterpret_cast<const char*>(message->payload), message->length);
    check(!network.publishTerminalEvent(pairing, *message), "unstarted terminal accepted");
    begin(network);
    check(!network.publishTerminalEvent(pairing, *message), "terminal accepted before first cloud session");
    const Status motion = readyMotion();
    const bool reconnect = mode == "current-generation" || mode == "reconnect-queued" ||
        mode == "reconnect-deferred" || mode == "reconnect-before-send" || mode == "history";
    const auto publish = [&] {
        const bool worker = fake::io.inWorker;
        fake::io.inWorker = false;
        const bool result = network.publishTerminalEvent(pairing, *message);
        fake::io.inWorker = worker;
        check(std::memcmp(message.get(), original.get(), sizeof(*original)) == 0, "terminal input was mutated");
        return result;
    };
    std::string previous;
    bool rotatedBeforeSend = false;
    fake::io.onDelay = [&](unsigned tick) {
        check(tick < 12, "terminal transport did not converge");
        if (tick == 1) poll(network);
        else if (tick == 2) {
            previous = token();
            if (mode == "offline") { WiFi.state = 0; return; }
            if (mode == "full") {
                for (unsigned i = 0; i < 8; ++i) check(publish(), "terminal queue filled before eight events");
                check(terminalPackets().empty(), "enqueue performed synchronous MQTT I/O");
                check(!publish() && fake::io.queueSendFailures == 1, "full terminal queue did not return false");
                return;
            }
            if (mode == "publish-failure") fake::io.publishOk = false;
            if (mode == "reconnect-deferred") fake::io.deferNextQueueSend = true;
            if (mode != "current-generation" && mode != "history") {
                if (mode == "stale-motion") pollAt(network, &motion, true, millis() - 1500u);
                fake::io.now += 5001u;
                check(publish(), "valid historical terminal blocked by Motion/context/readiness/TTL");
                check(terminalPackets().empty(), "terminal success claimed actual delivery");
                if (!reconnect && mode != "publish-failure") poll(network);
            }
            if (reconnect) {
                if (mode == "reconnect-before-send") {
                    fake::io.onLoop = [&] {
                        if (!rotatedBeforeSend) {
                            rotatedBeforeSend = true;
                            network.resetCommandSession();
                        }
                    };
                } else {
                    network.resetCommandSession();
                    check(!publish(), "terminal accepted in invalidated current generation");
                }
            }
        } else if (tick == 3 && mode == "offline") {
            check(!network.connected() && !publish() && terminalPackets().empty(), "offline terminal not retained by caller");
            connectedWifi();
        } else if (tick == 3 && mode == "publish-failure") {
            check(!network.connected() && terminalPackets().size() == 1 && !publish(),
                  "MQTT rejection was mistaken for durable terminal success");
            fake::io.publishOk = true;
            fake::io.now += 5000u;
        } else if ((tick == 3 && reconnect) ||
                   (tick == 4 && (mode == "offline" || mode == "publish-failure"))) {
            check(network.connected() && fake::io.connectCalls == 2, "terminal retry did not reconnect");
            if (mode == "reconnect-deferred") fake::completeDeferredSends();
            check(terminalPackets().size() == (mode == "publish-failure" ? 1u : 0u),
                  "old-generation terminal crossed reconnect before retry");
            // publish before poll: use CloudLink's actual generation, not the
            // coordinator's still-old status generation or event uptime.
            check(publish(), "retained terminal cannot enqueue in new current generation");
            poll(network);
        } else if (tick == 3 && mode == "full") {
            check(publish(), "terminal could not retry after queue drain");
        } else if ((mode == "full" && tick == 7) ||
                   (mode != "full" && tick >= (mode == "offline" || mode == "publish-failure" ||
                                               mode == "reconnect-deferred" ? 5u : reconnect ? 4u : 3u))) {
            const auto packets = terminalPackets();
            check(packets.size() == (mode == "full" ? 9u : mode == "publish-failure" ? 2u : 1u),
                  "terminal lost/duplicated or crossed generations");
            for (const auto index : packets) {
                const auto& packet = fake::io.published[index];
                check(!packet.retained && packet.payload == bytes, "terminal route/retained/raw bytes changed");
                auto captured = std::make_unique<babytech::v4::Message>();
                captured->kind = babytech::v4::Kind::Terminal;
                terminalBytes(*captured, packet.payload);
                auto decoded = std::make_unique<babytech::boardlink::TerminalEvent>();
                check(babytech::boardlink::decodeTerminalEvent(*captured, pairing, *decoded),
                      "MQTT captured bytes do not round-trip production terminal decoder");
                check(decoded->uptimeMs == 1 && decoded->request.profileVersion == 7 &&
                      std::string(decoded->request.babyId) == "historical-baby", "historical snapshot changed");
            }
            if (reconnect || mode == "offline" || mode == "publish-failure") {
                auto latest = fake::io.published.size();
                while (latest && fake::io.published[latest - 1].topic != kPrefix + "status") --latest;
                check(latest && token(latest - 1) != previous, "terminal retry reused old cloud session");
            }
            workerOnly(); stop();
        }
    };
    fake::runWorker();
}

void terminalRejected(const std::string& mode) {
    BrainNetwork network;
    begin(network);
    auto pairing = terminalPairing();
    auto message = terminalMessage(pairing, mode == "local-command", true, mode == "simulation-marker");
    std::string bytes(reinterpret_cast<const char*>(message->payload), message->length);
    const auto replace = [&](const std::string& from, const std::string& to) {
        const auto at = bytes.find(from);
        check(at != std::string::npos, "terminal rejection fixture field missing");
        bytes.replace(at, from.size(), to);
    };
    if (mode == "wrong-kind") message->kind = babytech::v4::Kind::CloudReceipt;
    else if (mode == "unknown-kind") message->kind = babytech::v4::Kind(255);
    else if (mode == "motion-role") pairing.role = babytech::v4::Role::Motion;
    else if (mode == "invalid-role") pairing.role = babytech::v4::Role(0);
    else if (mode == "invalid-pairing") std::memcpy(pairing.peerPhysicalId, pairing.localPhysicalId, sizeof(pairing.peerPhysicalId));
    else if (mode == "pair-unterminated") std::memset(pairing.deviceId, 'a', sizeof(pairing.deviceId));
    else if (mode == "pair-device") std::strcpy(pairing.deviceId, "other-device");
    else if (mode == "event-device") replace(kId, "other-device");
    else if (mode == "other-current-device") {
        std::strcpy(pairing.deviceId, "other-device");
        message = terminalMessage(pairing);
        bytes.assign(reinterpret_cast<const char*>(message->payload), message->length);
    }
    else if (mode == "wrong-epoch") pairing.epoch[0] = '9';
    else if (mode == "invalid-physical") std::memset(pairing.localPhysicalId, '0', sizeof(pairing.localPhysicalId) - 1);
    else if (mode == "local-command") replace("local-" + std::string(pairing.epoch), "local-wrong");
    else if (mode == "empty") bytes.clear();
    else if (mode == "invalid-json") bytes = "{";
    else if (mode == "extra") replace("{", "{\"unexpected\":true,");
    else if (mode == "duplicate") replace("{", "{\"device_id\":\"bt-brain-test\",");
    else if (mode == "missing") replace("\"water_ml\":120,", "");
    else if (mode == "range") replace("\"water_ml\":120", "\"water_ml\":0");
    else if (mode == "type") replace("\"water_ml\":120", "\"water_ml\":\"120\"");
    else if (mode == "sequence") replace("\"command_seq\":\"9223372036854775807\"", "\"command_seq\":\"42\"");
    else if (mode == "powder") replace("\"target_powder_g\":30", "\"target_powder_g\":31");
    else if (mode == "trailing") bytes += "{}";
    else if (mode == "nul") bytes += std::string(1, '\0');
    else if (mode == "escaped-nul") replace("historical", "history\\u0000");
    else if (mode == "utf8") replace("historical", std::string("\xc0\xaf"));
    else check(mode == "oversized" || mode == "simulation-marker", "unknown terminal rejection fixture");
    terminalBytes(*message, bytes);
    if (mode == "oversized") message->length = uint16_t(babytech::v4::kMaxMessage + 1);
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) {
            check(network.connected(), "terminal rejection fixture has no cloud session");
            const auto sends = fake::io.queueSendCalls;
            check(!network.publishTerminalEvent(pairing, *message) && fake::io.queueSendCalls == sends,
                  "invalid terminal was accepted/enqueued");
        } else {
            check(fake::io.published.empty(), "invalid terminal reached MQTT");
            workerOnly(); stop();
        }
    };
    fake::runWorker();
}

void simulationPath(const std::string& mode) {
    using Owner = babytech::brain::BrainSimulationDispatcher<BrainNetwork>;
    using namespace babytech::boardlink;
    BrainNetwork network;
    begin(network);
    babytech::v4::Pairing pairing;
    pairing.role = babytech::v4::Role::Brain;
    std::strcpy(pairing.deviceId, kId);
    std::strcpy(pairing.epoch, "0123456789abcdef0123456789abcdef");
    std::strcpy(pairing.localPhysicalId, "aabbccddeeff");
    std::strcpy(pairing.peerPhysicalId, "112233445566");
    Owner owner(pairing, network, millis, 15000);
    ProductContext context;
    std::strcpy(context.deviceId, kId);
    std::strcpy(context.babyId, "baby-test");
    std::strcpy(context.babyName, "Frozen baby");
    std::strcpy(context.formulaBrand, "Test formula");
    context.profileVersion = 7;
    context.waterMl = 180;
    context.temperatureC = 45;
    context.powderGPer100Ml = 25.5f;
    owner.setContext(&context, true, true);
    // Production owners and MQTT ingress; only the final UART I/O is captured.
    // The actual main callback's branch is also checked by the wiring suite.
    struct ReceiptLink {
        const babytech::v4::Pairing& pairing;
        std::vector<CloudReceipt> forwarded;
        const babytech::v4::Pairing* verifiedPairing() const { return &pairing; }
        bool forwardCloudReceipt(const CloudReceipt& receipt, uint32_t) {
            forwarded.push_back(receipt);
            return true;
        }
    } link{pairing, {}};
    using Relay = babytech::brain::BrainResultDelivery<ReceiptLink, BrainNetwork>;
    Relay relay(link, network);
    struct Route {
        Owner& owner;
        Relay& relay;
        size_t realCalls = 0;
        size_t receiptCalls = 0;
        static void command(void* ptr, const CloudCommand& value, uint32_t generation, uint32_t now) {
            auto& self = *static_cast<Route*>(ptr);
            if (self.owner.enabled()) self.owner.command(value, generation, now);
            else ++self.realCalls;
        }
        static void stopCommand(void* ptr, const CloudStop& value, uint32_t generation, uint32_t now) {
            auto& self = *static_cast<Route*>(ptr);
            if (self.owner.enabled()) self.owner.stop(value, generation, now);
            else ++self.realCalls;
        }
        static void receipt(void* ptr, const CloudReceipt& value) {
            auto& self = *static_cast<Route*>(ptr);
            ++self.receiptCalls;
            if (self.owner.receipt(value)) return;
            self.relay.receipt(value);
        }
        static void contextReceived(void* ptr, const ProductContext&, uint32_t, uint32_t) {
            // The main callback refreshes cache usability immediately: pending
            // persistence cannot authorize a later command in the same batch.
            static_cast<Route*>(ptr)->owner.setContext(nullptr, false, true);
        }
    } route{owner, relay};
    network.setProductHandlers(Route::command, Route::stopCommand, &route);
    network.setReceiptHandler(Route::receipt, &route);
    network.setContextHandler(Route::contextReceived, &route);
    char output[96]{};
    check(babytech::brain::BrainSimulationConsole::handle("SIM ON", owner, false, output, sizeof(output)),
          "actual owner did not handle USB simulation command");
    check(owner.enabled() && std::string(output) == "[simulation] on\n", "USB mode did not enable owner");
    std::string session, original, simulatedStored, realStored, realEventId;
    const bool mixed = mode.compare(0, 9, "receipts-") == 0;
    size_t receiptsBeforeReconnect = 0;
    unsigned phase = 0, waits = 0;
    const auto ui = [&] {
        const bool worker = fake::io.inWorker;
        fake::io.inWorker = false;
        network.poll(nullptr, false, millis(), 0, false, false, owner.enabled() ? &owner.status() : nullptr);
        owner.poll(millis());
        relay.poll(millis());
        fake::io.inWorker = worker;
    };
    const auto findTopic = [&](const char* suffix) {
        std::vector<size_t> found;
        for (size_t i = 0; i < fake::io.published.size(); ++i)
            if (fake::io.published[i].topic == kPrefix + suffix) found.push_back(i);
        return found;
    };
    fake::io.onDelay = [&](unsigned) {
        check(++waits < 40, "simulation network scenario did not converge");
        if (phase == 0) { ui(); phase = 1; return; }
        if (phase == 1) {
            const auto statuses = findTopic("status");
            if (statuses.empty()) { ui(); return; }
            DynamicJsonDocument doc(8192);
            check(!deserializeJson(doc, fake::io.published[statuses.back()].payload), "simulation status malformed");
            check(doc["hardware_profile"] == "simulation" && doc["motion_connected"] == false &&
                  doc["commands_enabled"] == true && doc["can_start"] == true && doc["progress"] == "ready",
                  "detached-Motion simulation did not advertise explicit ready");
            check(doc["water_temp"].isNull() && doc["low_water"].isNull() && doc["powder_remained"].isNull() &&
                  doc["actuator_operational"] == false, "simulation forged physical measurements/readiness");
            session = doc["command_session"].as<std::string>();
            original = cloudCommand("prepare", session, millis());
            if (mode == "context-batch") {
                auto newer = context;
                newer.profileVersion++;
                receive("command", original);
                receive("config", contextJson(newer));
                ui();
                check(!owner.running() && owner.resultCount() == 0 && route.realCalls == 0,
                      "pending context allowed old-context Prepare in the same batch");
                phase = 6;
                return;
            }
            receive("command", original);
            ui();
            check(owner.running(), "actual Cloud ingress did not start Brain simulation");
            receive("command", original);
            ui();
            check(owner.running() && owner.resultCount() == 0 && route.realCalls == 0,
                  "duplicate simulation restarted/completed or fell through to real route");
            fake::io.now += 15000;
            phase = 2;
            return;
        }
        if (phase == 2) {
            if (mode == "stop") receive("command", cloudCommand("stop", session, millis(), "43", "stop-test"));
            if (mode == "offline") {
                fake::io.clients.front()->dropConnection();
                fake::io.connectOk = false;
            }
            ui();
            check(!owner.running() && owner.resultCount() == 1, "timer/Stop did not freeze one result");
            phase = mode == "offline" ? 3 : 4;
            return;
        }
        if (phase == 3) {
            check(!network.connected() && owner.resultCount() == 1, "offline completion deleted result");
            fake::io.connectOk = true;
            fake::io.now += 5000;
            ui();
            phase = 4;
            return;
        }
        if (phase == 4) {
            const auto events = findTopic("event");
            if (events.empty()) { fake::io.now += 250; ui(); return; }
            const std::string frozen = fake::io.published[events.front()].payload;
            DynamicJsonDocument doc(4096);
            check(!deserializeJson(doc, frozen), "production simulation event not JSON");
            check(doc["execution_mode"] == "brain_simulation" && doc["command_id"] == "cloud-test" &&
                  doc["command_seq"] == "42" && doc["baby_id"] == "baby-test" &&
                  doc["feeding_context_profile_version"] == 7 && doc["water_ml"] == 180 &&
                  doc["temp"] == 45 && doc["target_powder_g"].as<float>() == 45.9f,
                  "MQTT terminal changed original request identity/recipe");
            check(doc["dispensed_water_ml"].isNull(), "simulation invented dispensed water");
            check(doc["event"] == (mode == "stop" ? "feeding_failed" : "feeding_completed"),
                  "explicit Stop was recorded as success");
            if (mode == "stop") check(doc["reason"] == "stopped", "Stop reason missing");
            for (size_t index : events) check(fake::io.published[index].payload == frozen, "retry mutated frozen event");
            check(owner.resultCount() == 1 && route.realCalls == 0, "publish queue acted as stored proof/real dispatch");
            CloudReceipt receipt;
            std::strcpy(receipt.deviceId, kId);
            std::strcpy(receipt.eventId, doc["event_id"].as<const char*>());
            babytech::v4::Message message;
            check(encodeCloudReceipt(receipt, message), "receipt fixture encoding failed");
            const std::string stored(reinterpret_cast<const char*>(message.payload), message.length);
            receive("config/nested", stored);
            receive("command", stored);
            ui();
            check(owner.resultCount() == 1, "wrong topic cleared simulated result");
            check(link.forwarded.empty(), "wrong topic forwarded a Cloud receipt to Motion");
            if (mixed) {
                simulatedStored = stored;
                check(babytech::boardlink::makeProductEventId(pairing,
                      babytech::v4::Source::CloudCommand, 41, receipt.eventId), "real receipt ID fixture failed");
                realEventId = receipt.eventId;
                check(encodeCloudReceipt(receipt, message), "real receipt fixture encoding failed");
                realStored.assign(reinterpret_cast<const char*>(message.payload), message.length);
                // A real historical receipt must continue to Motion in SIM ON;
                // it must not delete or otherwise mutate the simulated result.
                if (mode == "receipts-on") {
                    receive("config", realStored);
                    ui();
                    check(owner.resultCount() == 1 && link.forwarded.size() == 1 &&
                          realEventId == link.forwarded[0].eventId,
                          "SIM ON swallowed real receipt or cleared simulated evidence");
                    receive("config", simulatedStored);
                    ui();
                    check(owner.resultCount() == 0 && link.forwarded.size() == 1,
                          "matching simulation receipt reached Motion");
                    noSideEffects(); workerOnly(); stop();
                }
                receiptsBeforeReconnect = route.receiptCalls;
                if (mode == "receipts-off") {
                    check(owner.setEnabled(false, false) == babytech::brain::SimulationModeResult::Changed,
                          "pending result blocked SIM OFF");
                } else {
                    if (mode == "receipts-deferred") fake::io.deferNextQueueSend = true;
                    receive("config", simulatedStored);
                    receive("config", realStored);
                    fake::io.clients.front()->dropConnection();
                }
                check(owner.resultCount() == 1 && link.forwarded.empty(), "unconsumed receipt deleted a result");
                waits = 0;
                phase = 7;
                fake::io.now += 5000;
                return;
            }
            if (mode == "switch") {
                // Both pre-toggle queued and post-toggle old-session bytes
                // must never be delivered to the real hardware route.
                receive("command", original);
                check(owner.setEnabled(false, false) == babytech::brain::SimulationModeResult::Changed,
                      "idle mode switch blocked by an unuploaded RAM result");
                check(!network.connected() && owner.resultCount() == 1, "mode switch lost pending result/session");
                phase = 5;
                return;
            }
            receive("config", stored);
            ui();
            check(owner.resultCount() == 0, "actual decoded matching receipt did not remove simulated result");
            workerOnly();
            stop();
        }
        if (phase == 5) {
            if (!network.connected()) { fake::io.now += 5000; ui(); return; }
            receive("command", original);
            ui();
            check(route.realCalls == 0 && owner.resultCount() == 1, "delayed simulation bytes reached real route");
            workerOnly();
            stop();
        }
        if (phase == 6) {
            const auto acks = findTopic("ack");
            if (acks.empty()) { ui(); return; }
            DynamicJsonDocument ack(2048);
            check(!deserializeJson(ack, fake::io.published[acks.back()].payload), "invalid batch ACK");
            check(ack["accepted"] == false && ack["reason"] == "context_required",
                  "context batch did not reject stale request explicitly");
            check(!owner.running() && !owner.status().canStart && owner.resultCount() == 0,
                  "pending context falsely restored simulation readiness");
            workerOnly();
            stop();
        }
        if (phase == 7) {
            if (!network.connected()) { fake::io.now += 5000; ui(); return; }
            if (mode == "receipts-deferred") fake::completeDeferredSends();
            ui();
            check(route.receiptCalls == receiptsBeforeReconnect && owner.resultCount() == 1 && link.forwarded.empty(),
                  "old MQTT generation deleted a result or reached Motion relay");
            receive("config", realStored);
            ui();
            check(owner.resultCount() == 1 && link.forwarded.size() == 1 &&
                  realEventId == link.forwarded[0].eventId,
                  "current real receipt did not recover after MQTT generation change");
            receive("config", simulatedStored);
            ui();
            check(owner.resultCount() == 0 && link.forwarded.size() == 1,
                  "current simulated receipt leaked to Motion or was lost in SIM OFF");
            check(owner.enabled() == (mode != "receipts-off"), "receipt changed simulation mode");
            check(route.realCalls == 0, "receipt/reconnect accidentally dispatched real movement");
            noSideEffects(); workerOnly(); stop();
        }
    };
    fake::runWorker();
}
void statusSafety() {
    babytech::cloud::SessionSnapshot session;
    std::strcpy(session.id, kChallenge);
    session.generation = 7;
    session.uptimeMs = 1234;
    Status motion = readyMotion();
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        StaticJsonDocument<4096> doc;
        const Status* value = scenario == 0 ? nullptr : &motion;
        const bool fresh = scenario < 3 || scenario >= 5;
        motion = readyMotion();
        if (scenario == 4 || scenario == 5) {
            std::strcpy(motion.productError, "E_CAN_FAULT");
            std::strcpy(motion.productProgress, "error");
            motion.isPreparing = true;
        }
        if (scenario == 6) motion.lowWaterValid = motion.powderValid = false;
        if (scenario == 7) std::strcpy(motion.productProgress, "complete");
        babytech::brain::writeStatus(doc.to<JsonObject>(), kId, "host-test", value, fresh, session);
        check(!doc.overflowed(), "status document overflow");
        check(doc["bottle_presence_sensor_enabled"] == false && doc["bottle_state_valid"] == false &&
              doc["bottle_clamp_status"] == "unknown" && doc["bottle_present_at_load_position"].isNull(),
              "unmeasured bottle advertised as empty/full");
        check(doc["can_start"] == false && doc["commands_enabled"] == false && doc["progress"] != "ready",
              "null/stale/read-only telemetry advertised start conditions");
        const bool usable = value && fresh;
        check(doc["motion_connected"] == usable && doc["motion_status_stale"] == !usable,
              "motion freshness projection incorrect");
        check(doc["actuator_operational"] == usable && doc["feeding_context_configured"] == usable,
              "stale/null status retained live readiness");
        if (!usable || scenario == 6) {
            check(doc["water_status"] == "unknown" && doc["powder_status"] == "unknown" &&
                  doc["low_water"].isNull() && doc["powder_remained"].isNull(),
                  "stale/null telemetry exposed resources as fresh");
            if (!usable) check(doc["water_temp"].isNull(), "offline temperature is not unknown");
        } else {
            check(doc["water_status"] == "normal" && doc["powder_remained"] == 500 && doc["target_temp"] == 42,
                  "valid observational resources were lost");
            check(doc["feeding_context_baby_id"] == motion.babyId &&
                  doc["feeding_context_baby_name"] == motion.snapshot.babyName.data(), "identity confused with display label");
        }
        if (scenario == 4 || scenario == 5)
            check(doc["error_code"] == "E_CAN_FAULT" && doc["progress"] == "error" && doc["is_preparing"] == true,
                  "link loss erased known error/in-progress evidence");
        check(doc["measured_water_temp"].isNull() && doc["is_water_ready"] == false, "simulated heat became measured readiness");
    }
}

void statusFlags() {
    babytech::cloud::SessionSnapshot session;
    std::strcpy(session.id, kChallenge);
    for (unsigned bits = 0; bits < 64; ++bits) {
        Status motion = readyMotion();
        const bool present = bits & 1;
        const bool fresh = bits & 2;
        const bool enabled = bits & 4;
        const bool start = bits & 8;
        motion.snapshot.thermalSimulated = bits & 16;
        motion.powderValid = !(bits & 32);
        // These observations must not globally disable Stop/low-frequency operations.
        motion.motionBusy = motion.eventPending = true;
        motion.executionAuthorized = false;
        const bool expectedEnabled = present && fresh && enabled;
        const bool expectedStart = expectedEnabled && start;
        StaticJsonDocument<4096> doc;
        babytech::brain::writeStatus(doc.to<JsonObject>(), kId, "host-test", present ? &motion : nullptr,
                                    fresh, session, kChallenge, enabled, start);
        check(!doc.overflowed() && doc["commands_enabled"] == expectedEnabled && doc["can_start"] == expectedStart,
              "explicit status flags were inferred from busy/storage/telemetry or bypassed stale gate");
        check(doc["bottle_clamp_status"] == "unknown" && doc["bottle_state_valid"] == false,
              "ready permission fabricated a bottle state");
        check(doc["progress"] == (expectedStart ? "ready" : "noready"), "legacy progress bypassed explicit start gate");
        check(doc["thermal_simulated"] == (!present || motion.snapshot.thermalSimulated),
              "status invented/suppressed original thermal flag");
        check(doc["water_temp"].isNull() && doc["measured_water_temp"].isNull() &&
              doc["is_water_ready"] == false && doc["is_heating"] == false && doc["is_cooling"] == false,
              "target temperature was fabricated into measured water temperature/readiness");
        if (present && fresh) check(doc["target_temp"] == 42, "valid target temperature lost");
        else check(doc["target_temp"].isNull(), "stale target temperature advertised as fresh");
        if (!present || !fresh || !motion.powderValid)
            check(doc["powder_status"] == "unknown" && doc["powder_remained"].isNull(), "unknown powder became known");
        else check(doc["powder_remained"] == 500, "real known powder not forwarded");
    }
}

void networkStatusFlags(const std::string& mode) {
    if (mode == "wrap") fake::io.now = UINT32_MAX - 9u;
    BrainNetwork network;
    begin(network);
    Status motion = readyMotion();
    motion.motionBusy = motion.eventPending = true;
    motion.powderValid = false;
    motion.snapshot.thermalSimulated = true;
    const bool enable = mode != "disabled";
    const bool start = mode != "operations";
    const bool absent = mode == "absent";
    const bool stale = mode == "stale";
    uint32_t receipt = 0;
    std::string session;
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) {
            receipt = millis();
            if (mode == "age1499") receipt -= 1499u;
            if (mode == "age1500") receipt -= 1500u;
            pollAt(network, absent ? nullptr : &motion, !stale, receipt, enable, start);
            if (mode == "send-expiry") fake::io.onLoop = [&] { fake::io.now = receipt + 1500u; };
        } else if (tick == 2) {
            if (mode == "age1500" || mode == "send-expiry") {
                check(fake::io.published.empty(), "expired Motion sample advertised commands_enabled/start");
                fake::io.onLoop = nullptr;
                fake::io.now = receipt + 2500u;
                pollAt(network, &motion, false, receipt, true, true);
            } else {
                const bool expectedEnable = enable && !absent && !stale;
                auto doc = packet(0, expectedEnable, expectedEnable && start);
                check(doc["powder_status"] == "unknown" && doc["thermal_simulated"] == true && doc["water_temp"].isNull(),
                      "enabled status fabricated resource/thermal measurement");
                session = doc["command_session"].as<std::string>();
                receive("config", probe(session));
                pollAt(network, absent ? nullptr : &motion, !stale, millis(), enable, start);
            }
        } else if (tick == 3) {
            if (mode == "age1500" || mode == "send-expiry") {
                check(fake::io.published.size() == 1, "stale conservative flags did not recover after expired sample");
                packet(0);
                noSideEffects(); workerOnly(); stop();
            }
            const bool expectedEnable = enable && !absent && !stale;
            check(packet(1, expectedEnable, expectedEnable && start)["command_session_challenge"] == kChallenge,
                  "probe did not forward explicit capability/start flags");
            fake::io.now += 2000u;
            pollAt(network, &motion, false, millis(), true, true);
        } else {
            check(fake::io.published.size() == 3, "stale projection missing/duplicated after enabled status");
            auto doc = packet(2);
            check(doc["motion_status_stale"] == true && doc["progress"] == "noready",
                  "stale Motion retained explicit commands/start permissions");
            noSideEffects(); workerOnly(); stop();
        }
    };
    fake::runWorker();
}

Status maximumMotion(char identityByte, char nameByte) {
    Status motion = readyMotion();
    std::memset(motion.babyId, identityByte, sizeof(motion.babyId) - 1);
    std::fill(motion.snapshot.babyName.begin(), motion.snapshot.babyName.end() - 1, nameByte);
    std::memset(motion.productError, 'E', sizeof(motion.productError) - 1);
    std::strcpy(motion.productProgress, "dispensing_powder");
    motion.powderGrams = INT32_MAX;
    motion.contextVersion = INT32_MAX;
    motion.snapshot.temperatureC = INT16_MIN;
    return motion;
}
void statusSize(bool controls = false) {
    Status motion = readyMotion();
    babytech::cloud::SessionSnapshot session;
    std::strcpy(session.id, kChallenge);
    session.generation = session.uptimeMs = UINT32_MAX;
    for (unsigned scenario = controls ? 4 : 0; scenario < (controls ? 5u : 4u); ++scenario) {
        StaticJsonDocument<4096> doc;
        const std::string device = scenario < 2 ? kId : std::string(64, 'b');
        if (scenario >= 2) {
            motion = maximumMotion(controls ? '\x01' : scenario == 3 ? '"' : 'b',
                                   controls ? '\x01' : scenario == 3 ? '\\' : 'n');
        }
        babytech::v4::Message wire;
        Status decoded;
        check(babytech::boardlink::encodeStatus(motion, wire) && babytech::boardlink::decodeStatus(wire, decoded),
              "capacity fixture is not accepted by production telemetry codec");
        babytech::brain::writeStatus(doc.to<JsonObject>(), device.c_str(), "host-test", &motion, true,
                                    session, scenario == 0 ? nullptr : kChallenge);
        std::array<char, 4096> full{};
        check(babytech::brain::encodeStatusJson(doc, full.data(), full.size()), "large-buffer status encoding failed");
        const std::string encoded(full.data());
        std::cout << "STATUS_BYTES " << (scenario == 0 ? "ordinary" : scenario == 1 ? "probe" :
                                       scenario == 2 ? "max-fields" : scenario == 3 ? "max-escaped-fields" : "max-controls")
                  << '=' << encoded.size() << (controls ? " (must reject at capacity 2048)\n" : " (must be <2048)\n");
        const size_t rawControls = std::count_if(encoded.begin(), encoded.end(),
            [](unsigned char c) { return c < 0x20; });
        check(!rawControls, "production encoder emitted unescaped JSON control bytes");
        DynamicJsonDocument parsed(8192);
        check(!deserializeJson(parsed, full.data()), "escaped payload is not JSON");
        check(parsed["feeding_context_baby_id"] == motion.babyId &&
              parsed["feeding_context_baby_name"] == motion.snapshot.babyName.data(), "escaped identity did not round trip");
        std::array<char, 2048> limited;
        limited.fill('!');
        const auto original = limited;
        const bool accepted = babytech::brain::encodeStatusJson(doc, limited.data(), limited.size());
        check(accepted == !controls, "encoder capacity acceptance does not match payload size");
        if (controls) {
            check(encoded.size() > 2047 && limited == original, "oversized status partially overwrote output");
        } else check(encoded.size() < 2048 && std::string(limited.data()) == encoded,
                     "bounded status was truncated/changed");
    }
}
void encoderBoundary() {
    DynamicJsonDocument doc(8192);
    doc["value"] = std::string(2035, 'x');
    // {"value":""} costs twelve bytes, including braces and quotes.
    check(measureJson(doc) == 2047, "invalid exact-boundary fixture");
    std::array<char, 2048> output;
    output.fill('!');
    check(babytech::brain::encodeStatusJson(doc, output.data(), output.size()) && std::strlen(output.data()) == 2047,
          "2047-byte JSON did not fit with terminator");
    const auto original = output;
    doc["value"] = std::string(2036, 'x');
    check(!babytech::brain::encodeStatusJson(doc, output.data(), output.size()) && output == original,
          "2048-byte JSON was accepted or partially written");
    check(!babytech::brain::encodeStatusJson(doc, output.data(), 0) && output == original &&
          !babytech::brain::encodeStatusJson(doc, nullptr, output.size()), "null/zero-capacity output accepted");
    doc.clear();
    std::string controls;
    for (char c = 1; c < 32; ++c) controls += c;
    controls += "\"\\/\xE4\xB8\xAD";
    doc["value"] = controls;
    check(babytech::brain::encodeStatusJson(doc, output.data(), output.size()), "short control/UTF-8 value rejected");
    for (const unsigned char c : std::string(output.data())) check(c >= 0x20, "unescaped control in encoded JSON");
    DynamicJsonDocument parsed(8192);
    check(!deserializeJson(parsed, output.data()) && parsed["value"].as<std::string>() == controls,
          "escaping changed controls/quotes/UTF-8 identity");
    StaticJsonDocument<16> tooSmall;
    tooSmall["value"] = std::string(100, 'x');
    check(tooSmall.overflowed(), "overflow fixture did not overflow");
    const auto beforeOverflow = output;
    check(!babytech::brain::encodeStatusJson(tooSmall, output.data(), output.size()) && output == beforeOverflow,
          "overflowed document was encoded/partially written");
}
void statusPayload() {
    BrainNetwork network;
    begin(network);
    Status motion = maximumMotion('\x01', '\x01');
    uint32_t failedAt = 0;
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) {
            failedAt = millis();
            poll(network, &motion, true);
            motion = maximumMotion('"', '\\');
            fake::io.now = failedAt + 249;
            poll(network, &motion, true);
        } else if (tick == 2) {
            check(fake::io.published.empty(), "oversized payload leaked or failure retry occurred before 250ms");
            fake::io.now = failedAt + 250;
            poll(network, &motion, true);
        } else {
            check(fake::io.published.size() == 1, "oversized status leaked or valid status missing");
            auto doc = packet(0);
            check(doc["feeding_context_baby_id"] == motion.babyId &&
                  doc["feeding_context_baby_name"] == motion.snapshot.babyName.data(), "transmitted identity truncated");
            workerOnly(); stop();
        }
    };
    fake::runWorker();
}
void motionExpiry(const std::string& kind) {
    if (kind == "motion-expiry-wrap") fake::io.now = UINT32_MAX - 500u;
    if (kind == "motion-receipt-zero") fake::io.now = UINT32_MAX - 9u;
    BrainNetwork network;
    begin(network);
    Status motion = readyMotion();
    uint32_t receivedAt = 0;
    const bool isProbe = kind == "motion-expiry-probe";
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) {
            receivedAt = millis();
            if (kind == "motion-receipt-zero") check(receivedAt == 0, "zero receipt fixture missed rollover");
            pollAt(network, &motion, true, receivedAt);
            if (!isProbe) fake::io.onLoop = [&] { fake::io.now = receivedAt + 1500; };
        } else if (tick == 2 && isProbe) {
            const std::string session = token();
            fake::io.now = receivedAt + 1499;
            receive("config", probe(session));
            pollAt(network, &motion, true, receivedAt);
            fake::io.onLoop = [&] { fake::io.now = receivedAt + 1500; };
        } else if (tick == (isProbe ? 3u : 2u)) {
            check(fake::io.published.size() == (isProbe ? 1u : 0u),
                  "expired Motion sample sent; coordinator reset/lost original receipt timestamp");
            fake::io.onLoop = nullptr;
            fake::io.now = receivedAt + 2000;
            pollAt(network, &motion, false, receivedAt);
        } else {
            check(fake::io.published.size() == (isProbe ? 2u : 1u), "conservative stale status did not recover");
            auto doc = packet(fake::io.published.size() - 1);
            check(doc["motion_connected"] == false && doc["water_status"] == "unknown",
                  "stale fallback advertised fresh readiness");
            workerOnly(); stop();
        }
    };
    fake::runWorker();
}

void configureUnpaired(const std::string& kind) {
    fake::nvs.allowWrites = true;
    BrainNetwork network;
    char output[96]{};
    constexpr char mqtt[] = "NET MQTT 6e65772e74657374 1885 746573742d75736572 686f73742d6f6e6c792d746573742d736563726574";
    const auto command = [&](const char* line, bool maintenance, const char* expected) {
        check(babytech::brain::BrainNetworkConsole::handle(line, maintenance, network, output, sizeof(output)),
              "unpaired command not handled");
        check(std::string(output) == std::string("[network] ") + expected + "\n", "wrong unpaired result");
    };
    const auto dormant = [&] {
        poll(network);
        check(!network.started() && !network.connected() && fake::io.tasks.empty() &&
              fake::io.allocationCalls == 0 && fake::liveQueues() == 0 && fake::liveSemaphores() == 0 &&
              fake::io.taskAttempts == 0, "unpaired save/poll started network resources");
        check(WiFi.calls.empty() && fake::io.connectCalls == 0 && fake::io.disconnectCalls == 0 &&
              fake::io.loopCalls == 0 && fake::io.published.empty(), "unpaired save/poll touched network");
        check(fake::io.openPreferences == 0, "unpaired configuration leaked Preferences");
    };
    command("NET STATUS", false, "brain_unpaired");
    command(mqtt, false, "maintenance_required");
    check(fake::io.preferenceWriteCalls == 0, "configuration escaped maintenance gate");
    command("NET WIFI 486f6d65 -", true, "wifi_saved");
    dormant();
    if (kind == "configure-unpaired-failure") {
        for (bool readFailure : {false, true}) {
            fake::io.failPreferencesWrite = !readFailure;
            fake::io.failPreferencesRead = readFailure;
            command(mqtt, true, "mqtt_failed");
            dormant();
            fake::io.failPreferencesWrite = fake::io.failPreferencesRead = false;
        }
    }
    command(mqtt, true, "mqtt_saved");
    const auto saved = fake::io.preferences;
    check(saved.count("cloudcfg/record") == 1, "unpaired save missing existing record");
    dormant();
    check(!network.begin("bad/id"), "invalid identity used saved credentials");
    dormant();
    if (kind == "configure-unpaired-begin-failure") {
        fake::io.failTask = true;
        check(!network.begin(kId), "task allocation failure accepted");
        check(!network.started() && fake::io.tasks.empty() && fake::liveQueues() == 0 &&
              fake::liveSemaphores() == 0 && WiFi.calls.empty(), "failed begin leaked resources/radio");
        fake::io.failTask = false;
    }
    check(network.begin(kId), "valid identity could not load unpaired configuration");
    check(fake::io.preferences == saved, "begin changed pre-saved record");
    check(fake::io.connectCalls == 0 && WiFi.calls.empty(), "begin connected outside worker");
    const size_t configNvsCalls = fake::nvs.calls.size();
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) {
            check(calls("begin") == 1 && WiFi.attemptedSsid == "Home" && WiFi.attemptedPassword.empty(),
                  "worker did not join pre-saved Wi-Fi");
            check(fake::io.connectCalls == 0, "MQTT connected before Wi-Fi join completed");
            WiFi.state = WL_CONNECTED;
            WiFi.ssid = "Home";
            WiFi.address = IPAddress(192, 168, 1, 40);
            return;
        }
        check(network.connected() && fake::io.connectedId == kId && fake::io.server == "new.test" &&
              fake::io.port == 1885 && fake::io.credentialsMatched, "worker did not use pre-saved MQTT values");
        for (const auto& call : WiFi.calls) check(call.worker, "radio escaped worker after pre-start save");
        for (size_t i = configNvsCalls; i < fake::nvs.calls.size(); ++i)
            check(fake::nvs.calls[i].worker, "station reload escaped worker after pre-start save");
        stop();
    };
    fake::runWorker();
    for (const auto& log : fake::io.serial)
        check(log.find("host-only-test-secret") == std::string::npos &&
              log.find("686f73742d6f6e6c792d746573742d736563726574") == std::string::npos,
              "unpaired configuration logged secret");
}

void configureOtherOwner() {
    BrainNetwork owner;
    begin(owner);
    BrainNetwork other;
    const auto saved = fake::io.preferences;
    const unsigned writes = fake::io.preferenceWriteCalls;
    check(!other.configureMqtt("other.test", 2883, "user", "private-secret"),
          "unstarted BrainNetwork configured active owner's record");
    check(fake::io.preferences == saved && fake::io.preferenceWriteCalls == writes &&
          !other.started() && fake::io.tasks.size() == 1, "other-owner refusal changed state");
    fake::io.onDelay = [&](unsigned) {
        check(owner.connected() && !other.connected() && fake::io.server == "cached.test" &&
              fake::io.port == 1884 && fake::io.credentialsMatched, "other-owner refusal disrupted owner");
        stop();
    };
    fake::runWorker();
}

void configureNetwork(const std::string& kind) {
    fake::nvs.allowWrites = true;
    BrainNetwork network;
    begin(network);
    char output[96]{};
    const auto command = [&](const char* line, const char* expected) {
        check(babytech::brain::BrainNetworkConsole::handle(line, true, network, output, sizeof(output)),
              "network command not handled");
        check(std::string(output) == std::string("[network] ") + expected + "\n", "wrong configuration result");
    };
    if (kind == "configure-mqtt-failure") {
        fake::io.failPreferencesWrite = true;
        command("NET MQTT 6e65772e74657374 1885 75736572 70617373", "mqtt_failed");
        check(network.started(), "storage error permanently stopped worker");
        fake::io.failPreferencesWrite = false;
    }
    const bool wifi = kind == "configure-wifi";
    if (wifi) command("NET WIFI 486f6d65 -", "wifi_saved");
    else command("NET MQTT 6e65772e74657374 1885 75736572 70617373", "mqtt_saved");
    check(WiFi.calls.empty(), "configuration invoked radio in UI thread");
    fake::io.onDelay = [&](unsigned) {
        if (wifi) {
            check(calls("begin") == 1 && WiFi.attemptedSsid == "Home", "worker did not apply Wi-Fi configuration");
        } else {
            check(fake::io.server == "new.test" && fake::io.port == 1885, "worker did not apply MQTT configuration");
            check(fake::io.connectCalls == 1, "worker failed to connect with new settings");
        }
        for (const auto& call : WiFi.calls) check(call.worker, "radio escaped worker");
        stop();
    };
    fake::runWorker();
    for (const auto& log : fake::io.serial)
        check(log.find("70617373") == std::string::npos && log.find("password") == std::string::npos,
              "configuration logged secret");
}
}

int main(int argc, char** argv) {
    const char* fixtures = nullptr;
    const char* ackOutput = nullptr;
    if (argc == 5) {
        for (int i = 1; i < argc; i += 2) {
            const std::string option = argv[i];
            if (option == "--cloud-fixtures" && !fixtures) fixtures = argv[i + 1];
            else if (option == "--ack-output" && !ackOutput) ackOutput = argv[i + 1];
            else return 2;
        }
        if (!fixtures || !ackOutput) return 2;
    } else if (argc != 2) return 2;
    const std::string name = fixtures ? "cloud-fixtures" : argv[1];
    try {
        if (fixtures) cloudFixtures(fixtures, ackOutput);
        else if (name.compare(0, 18, "configure-unpaired") == 0) configureUnpaired(name);
        else if (name == "configure-other-owner") configureOtherOwner();
        else if (name.compare(0, 10, "configure-") == 0) configureNetwork(name);
        else if (name == "station-retry" || name == "station-rollover") stationRetry(name == "station-rollover");
        else if (name == "station-connected" || name == "station-wrong-ssid" || name == "station-no-ip")
            stationConnected(name.substr(8));
        else if (name.compare(0, 8, "station-") == 0) stationConfig(name.substr(8));
        else if (name == "identity" || name == "identity-max") identity(name == "identity-max");
        else if (name == "offline" || name == "no-cloudcfg") unavailable(name == "no-cloudcfg");
        else if (name == "periodic" || name == "periodic-rollover") periodic(name == "periodic-rollover");
        else if (name == "reconnect" || name == "expiry" || name == "probe-old-generation") newSession(name);
        else if (name == "send-expiry") sendExpiry();
        else if (name == "probe" || name == "probe-reject" || name == "probe-budget") probes(name);
        else if (name == "readonly") readonlyMessages();
        else if (name == "context-wire-boundary") contextWireBoundary();
        else if (name == "context-registration") contextRegistration();
        else if (name == "context-priority" || name == "context-priority-tombstone")
            contextPriority(name == "context-priority-tombstone");
        else if (name == "context-generation-queued" || name == "context-generation-deferred")
            contextGeneration(name == "context-generation-deferred");
        else if (name.compare(0, 15, "context-reject-") == 0) contextRejected(name.substr(15));
        else if (name.compare(0, 8, "context-") == 0) contextDelivery(name.substr(8));
        else if (name.compare(0, 19, "handler-generation-") == 0) handlerGeneration(name.substr(19));
        else if (name.compare(0, 15, "handler-reject-") == 0) commandRejected(name.substr(15), true);
        else if (name.compare(0, 8, "handler-") == 0) productHandler(name.substr(8));
        else if (name == "ack-validation") ackValidation();
        else if (name.compare(0, 12, "ack-runtime-") == 0) runtimeAck(name.substr(12));
        else if (name == "command-topics") commandTopics();
        else if (name.compare(0, 13, "command-size-") == 0) commandSizeBoundary(name.substr(13));
        else if (name == "command-budget" || name == "command-stop-priority") commandBudget(name == "command-stop-priority");
        else if (name.compare(0, 14, "command-fresh-") == 0) commandFreshness(name.substr(14));
        else if (name.compare(0, 19, "command-generation-") == 0) commandGeneration(name.substr(19));
        else if (name.compare(0, 21, "command-disconnected-") == 0) commandDisconnected(name.substr(21));
        else if (name.compare(0, 15, "command-reject-") == 0) commandRejected(name.substr(15));
        else if (name.compare(0, 15, "command-maxseq-") == 0) commandAccepted(name.substr(15), true);
        else if (name.compare(0, 8, "command-") == 0) commandAccepted(name.substr(8), false);
        else if (name == "queue-full") queueFull();
        else if (name == "failure-throttle" || name == "failure-throttle-rollover") failureThrottle(name == "failure-throttle-rollover");
        else if (name == "publish-failure") publishFailure();
        else if (name.compare(0, 16, "terminal-reject-") == 0) terminalRejected(name.substr(16));
        else if (name.compare(0, 9, "terminal-") == 0) terminalTransport(name.substr(9));
        else if (name == "status-safety") statusSafety();
        else if (name.compare(0, 11, "simulation-") == 0) simulationPath(name.substr(11));
        else if (name == "status-flags") statusFlags();
        else if (name.compare(0, 13, "status-flags-") == 0) networkStatusFlags(name.substr(13));
        else if (name == "status-size") statusSize();
        else if (name == "status-control-characters") statusSize(true);
        else if (name == "encoder-boundary") encoderBoundary();
        else if (name == "status-payload") statusPayload();
        else if (name.compare(0, 7, "motion-") == 0) motionExpiry(name);
        else throw std::runtime_error("unknown test case");
        if (name.compare(0, 10, "configure-") != 0) noSideEffects();
        check(fake::io.clients.empty(), "MQTT client lifetime leak");
        fake::cleanupLifetimeResources();
        std::cout << "PASS brain-network " << name << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL brain-network " << name << ": " << error.what() << '\n';
        return 1;
    }
}
