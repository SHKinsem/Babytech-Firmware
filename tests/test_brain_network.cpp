// Real production coordinator/station/status and transport. Only SDK I/O is
// replaced; process isolation matches CloudLink's MCU-lifetime ownership.
#include "brain_network.h"
#include "brain_status.h"
#include "brain_network_console.h"
#include "FakeBrainNvs.h"
#include "FakeCloudIo.h"
#include "WiFi.h"
#include <algorithm>
#include <iostream>
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
void pollAt(BrainNetwork& network, const Status* motion, bool fresh, uint32_t receivedAt) {
    // vTaskDelay yields to a simulated UI loop, not a second worker invocation.
    const bool worker = fake::io.inWorker;
    fake::io.inWorker = false;
    network.poll(motion, fresh, millis(), receivedAt);
    fake::io.inWorker = worker;
}
void poll(BrainNetwork& network, const Status* motion = nullptr, bool fresh = false) {
    pollAt(network, motion, fresh, millis());
}
DynamicJsonDocument packet(size_t index) {
    check(index < fake::io.published.size(), "missing status packet");
    const auto& value = fake::io.published[index];
    check(value.topic == kPrefix + "status" && !value.retained, "unexpected publish route/retained flag");
    check(value.payload.size() < 2048, "status reached/exceeded 2048-byte payload cap");
    DynamicJsonDocument doc(8192);
    check(!deserializeJson(doc, value.payload), "status payload is not JSON");
    check(doc["device_id"] == kId && doc["command_protocol"] == 4, "status identity/protocol mismatch");
    check(doc["can_start"] == false && doc["commands_enabled"] == false,
          "read-only status advertised command authorization");
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
            poll(network, &motion, true);
        } else if (tick == 2) {
            check(fake::io.published.size() == 1, "first status not sent");
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
void statusSafety() {
    babytech::cloud::SessionSnapshot session;
    std::strcpy(session.id, kChallenge);
    session.generation = 7;
    session.uptimeMs = 1234;
    Status motion = readyMotion();
    for (unsigned scenario = 0; scenario < 7; ++scenario) {
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
        babytech::brain::writeStatus(doc.to<JsonObject>(), kId, "host-test", value, fresh, session);
        check(!doc.overflowed(), "status document overflow");
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

void configureNetwork(const std::string& kind) {
    fake::nvs.allowWrites = true;
    BrainNetwork network;
    if (kind != "configure-unpaired") begin(network);
    char output[96]{};
    const auto command = [&](const char* line, const char* expected) {
        check(babytech::brain::BrainNetworkConsole::handle(line, true, network, output, sizeof(output)),
              "network command not handled");
        check(std::string(output) == std::string("[network] ") + expected + "\n", "wrong configuration result");
    };
    if (kind == "configure-unpaired") {
        command("NET WIFI 486f6d65 -", "wifi_saved");
        command("NET MQTT 686f7374 1883 75736572 70617373", "pairing_required");
        check(fake::io.preferenceWriteCalls == 0 && fake::io.tasks.empty(), "unpaired MQTT side effect");
        check(WiFi.calls.empty(), "unpaired configuration started radio");
        return;
    }
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
    if (argc != 2) return 2;
    const std::string name = argv[1];
    try {
        if (name.compare(0, 10, "configure-") == 0) configureNetwork(name);
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
        else if (name == "queue-full") queueFull();
        else if (name == "failure-throttle" || name == "failure-throttle-rollover") failureThrottle(name == "failure-throttle-rollover");
        else if (name == "publish-failure") publishFailure();
        else if (name == "status-safety") statusSafety();
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
