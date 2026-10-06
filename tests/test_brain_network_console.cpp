#include "brain_network_console.h"
#include "MaintenanceConsole.h"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using babytech::brain::BrainNetworkConsole;
using babytech::boardlink::MaintenanceLineReader;
using Result = MaintenanceLineReader::Result;

namespace {
size_t cases = 0;
size_t failures = 0;
size_t checks = 0;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) throw std::runtime_error( \
        std::string("line ") + std::to_string(__LINE__) + ": " #condition); \
} while (false)

void test(const std::string& name, const std::function<void()>& run) {
    ++cases;
    try {
        run();
        std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
}

struct Call {
    std::string method;
    std::string address;
    std::string user;
    std::string password;
    uint16_t port;
};

// No parser, credential validation, readiness transitions or storage in this fake.
// Results are injected; only the production template decides which calls occur.
struct FakeNetwork {
    bool ready = true;
    bool wifiResult = true;
    bool mqttResult = true;
    std::vector<Call> calls;

    bool started() {
        calls.push_back({"started", "", "", "", 0});
        return ready;
    }
    bool configureWifi(const char* ssid, const char* password) {
        calls.push_back({"wifi", ssid, "", password, 0});
        return wifiResult;
    }
    bool configureMqtt(const char* host, uint16_t port,
                       const char* user, const char* password) {
        calls.push_back({"mqtt", host, user, password, port});
        return mqttResult;
    }
};

std::string hex(const std::string& value) {
    const char* digits = "0123456789abcdef";
    std::string encoded;
    for (char byte : value) {
        const unsigned char c = static_cast<unsigned char>(byte);
        encoded += digits[c >> 4];
        encoded += digits[c & 15];
    }
    return encoded;
}

std::string wifi(const std::string& ssid, const std::string& password) {
    return "NET WIFI " + hex(ssid) + " " + (password.empty() ? "-" : hex(password));
}

std::string mqtt(const std::string& host, const std::string& port,
                 const std::string& user, const std::string& password) {
    return "NET MQTT " + hex(host) + " " + port + " " + hex(user) + " " + hex(password);
}

void response(const char* command, bool maintenance, FakeNetwork& network,
              const char* expected) {
    char output[128];
    std::memset(output, '!', sizeof(output));
    CHECK(BrainNetworkConsole::handle(command, maintenance, network, output, sizeof(output)));
    // Exact output also rejects plaintext/hex credential echoes on every path.
    CHECK(std::string(output) == std::string("[network] ") + expected + "\n");
}

void response(const std::string& command, bool maintenance, FakeNetwork& network,
              const char* expected) {
    response(command.c_str(), maintenance, network, expected);
}

void wifiCall(const FakeNetwork& network, size_t index, const std::string& ssid,
              const std::string& password) {
    CHECK(network.calls.at(index).method == "wifi");
    CHECK(network.calls.at(index).address == ssid);
    CHECK(network.calls.at(index).password == password);
}

void mqttCall(const FakeNetwork& network, size_t index, const std::string& host,
              uint16_t port, const std::string& user, const std::string& password) {
    CHECK(network.calls.at(index).method == "mqtt");
    CHECK(network.calls.at(index).address == host);
    CHECK(network.calls.at(index).port == port);
    CHECK(network.calls.at(index).user == user);
    CHECK(network.calls.at(index).password == password);
}

void statusAndGates() {
    for (bool maintenance : {false, true}) {
        for (bool ready : {false, true}) {
            test("status/maintenance=" + std::to_string(maintenance) +
                 "/ready=" + std::to_string(ready), [=]() {
                FakeNetwork network;
                network.ready = ready;
                response("NET STATUS", maintenance, network,
                         ready ? "brain_ready" : "brain_unpaired");
                CHECK(network.calls.size() == 1);
                CHECK(network.calls[0].method == "started");
            });
        }
    }
    for (bool ready : {false, true}) {
        test("maintenance-gate/ready=" + std::to_string(ready), [=]() {
            FakeNetwork network;
            network.ready = ready;
            for (const auto& command : {wifi("ssid", "secret password"),
                    mqtt("host", "1883", "user", "mqtt secret"),
                    std::string("NET WIFI zz secret"), std::string("NET MQTT broken"),
                    std::string("NET UNKNOWN secret")}) {
                response(command, false, network, "maintenance_required");
                CHECK(network.calls.empty());
            }
        });
    }
    test("unpaired/wifi-allowed-mqtt-blocked-then-ready", []() {
        FakeNetwork network;
        network.ready = false;
        response(wifi("ssid", ""), true, network, "wifi_saved");
        response(mqtt("host", "1883", "user", "secret"), true, network, "pairing_required");
        CHECK(network.calls.size() == 2);
        wifiCall(network, 0, "ssid", "");
        CHECK(network.calls[1].method == "started");
        network.ready = true;
        response(mqtt("host", "1883", "user", "secret"), true, network, "mqtt_saved");
        CHECK(network.calls.size() == 4);
        CHECK(network.calls[2].method == "started");
        mqttCall(network, 3, "host", 1883, "user", "secret");
    });
}

void validConfigurations() {
    const std::string utf8 = "\xe5\xa5\xb6\xe7\xb2\x89";
    const std::vector<std::pair<std::string, std::string>> wifiValues = {
        {"ssid", ""}, {" home wifi ", " pass word "}, {utf8, utf8 + " secret"},
        {std::string(32, 's'), std::string(63, 'p')},
        // The console buffer is 127 bytes; Wi-Fi policy belongs to configureWifi.
        {"ssid", std::string(127, 'p')}, {" ", " "}
    };
    for (size_t i = 0; i < wifiValues.size(); ++i) {
        test("wifi/valid-" + std::to_string(i), [=]() {
            FakeNetwork network;
            response(wifi(wifiValues[i].first, wifiValues[i].second), true, network, "wifi_saved");
            CHECK(network.calls.size() == 1);
            wifiCall(network, 0, wifiValues[i].first, wifiValues[i].second);
        });
    }
    struct MqttValue { std::string host, user, password; uint16_t port; };
    const std::vector<MqttValue> mqttValues = {
        {"host", "user", "password", 1}, {"host", "user", "password", 65535},
        {" host name ", " user name ", " pass word ", 1883},
        {utf8 + ".local", utf8, utf8 + " secret", 1883},
        {std::string(127, 'h'), std::string(63, 'u'), std::string(127, 'p'), 65535}
    };
    for (size_t i = 0; i < mqttValues.size(); ++i) {
        test("mqtt/valid-" + std::to_string(i), [=]() {
            FakeNetwork network;
            const auto& value = mqttValues[i];
            response(mqtt(value.host, std::to_string(value.port), value.user, value.password),
                     true, network, "mqtt_saved");
            CHECK(network.calls.size() == 2);
            CHECK(network.calls[0].method == "started");
            mqttCall(network, 1, value.host, value.port, value.user, value.password);
        });
    }
}

void malformedConfigurations() {
    std::vector<std::pair<std::string, std::string>> invalid = {
        {"wifi/missing-all", "NET WIFI "},
        {"wifi/missing-password", "NET WIFI 73"},
        {"wifi/missing-password-after-space", "NET WIFI 73 "},
        {"wifi/missing-ssid", "NET WIFI  70"},
        {"wifi/extra", "NET WIFI 73 70 78"},
        {"wifi/extra-open", "NET WIFI 73 - 78"},
        {"wifi/ssid-long", wifi(std::string(33, 's'), "pass")},
        {"wifi/password-long", wifi("ssid", std::string(128, 'p'))},
        {"wifi/empty-ssid-marker", "NET WIFI - 70"},
        {"mqtt/missing-all", "NET MQTT "},
        {"mqtt/missing-port", "NET MQTT 68"},
        {"mqtt/missing-user", "NET MQTT 68 1883"},
        {"mqtt/missing-user-after-space", "NET MQTT 68 1883 "},
        {"mqtt/missing-password", "NET MQTT 68 1883 75"},
        {"mqtt/missing-password-after-space", "NET MQTT 68 1883 75 "},
        {"mqtt/missing-host", "NET MQTT  1883 75 70"},
        {"mqtt/empty-user", "NET MQTT 68 1883  70"},
        {"mqtt/extra", "NET MQTT 68 1883 75 70 78"},
        {"mqtt/host-long", mqtt(std::string(128, 'h'), "1883", "u", "p")},
        {"mqtt/user-long", mqtt("h", "1883", std::string(64, 'u'), "p")},
        {"mqtt/password-long", mqtt("h", "1883", "u", std::string(128, 'p'))},
        {"mqtt/empty-password-marker", "NET MQTT 68 1883 75 -"},
        {"mqtt/empty-user-marker", "NET MQTT 68 1883 - 70"}
    };
    const std::vector<std::pair<std::string, std::string>> badHex = {
        {"nonhex-high", "g0"}, {"nonhex-low", "0g"},
        {"uppercase", "AF"}, {"nul", "00"}, {"embedded-nul", "730070"},
        {"odd-one", "7"}, {"odd-three", "737"}
    };
    for (const auto& value : badHex) {
        invalid.push_back({"wifi/ssid-" + value.first, "NET WIFI " + value.second + " 70"});
        invalid.push_back({"wifi/password-" + value.first, "NET WIFI 73 " + value.second});
        invalid.push_back({"mqtt/host-" + value.first, "NET MQTT " + value.second + " 1883 75 70"});
        invalid.push_back({"mqtt/user-" + value.first, "NET MQTT 68 1883 " + value.second + " 70"});
        invalid.push_back({"mqtt/password-" + value.first, "NET MQTT 68 1883 75 " + value.second});
    }
    for (const char* port : {"0", "00000", "65536", "99999", "100000", "4294967296",
                            "", "-1", "+1", "1x", "1.0"}) {
        invalid.push_back({std::string("mqtt/port-") + port, mqtt("h", port, "u", "p")});
    }
    for (const auto& value : invalid) {
        test("invalid/" + value.first, [=]() {
            FakeNetwork network;
            response(value.second, true, network, "invalid_config");
            CHECK(network.calls.empty());
            // A failed parse must leave no stale fields or permanent lockout.
            if (value.second.compare(0, 9, "NET WIFI ") == 0) {
                response(wifi("retry", ""), true, network, "wifi_saved");
                CHECK(network.calls.size() == 1);
                wifiCall(network, 0, "retry", "");
            } else {
                response(mqtt("retry", "1883", "new user", "new secret"), true, network, "mqtt_saved");
                CHECK(network.calls.size() == 2);
                CHECK(network.calls[0].method == "started");
                mqttCall(network, 1, "retry", 1883, "new user", "new secret");
            }
        });
    }
}

void failuresAndUnknown() {
    test("wifi/configure-failure-then-retry", []() {
        FakeNetwork network;
        network.wifiResult = false;
        response(wifi("first", "private password"), true, network, "wifi_failed");
        network.wifiResult = true;
        response(wifi("second", ""), true, network, "wifi_saved");
        CHECK(network.calls.size() == 2);
        wifiCall(network, 0, "first", "private password");
        wifiCall(network, 1, "second", "");
    });
    test("mqtt/configure-failure-then-retry", []() {
        FakeNetwork network;
        network.mqttResult = false;
        response(mqtt("first", "1883", "old user", "old secret"), true, network, "mqtt_failed");
        network.mqttResult = true;
        response(mqtt("second", "65535", "new user", "new secret"), true, network, "mqtt_saved");
        CHECK(network.calls.size() == 4);
        CHECK(network.calls[0].method == "started");
        CHECK(network.calls[2].method == "started");
        mqttCall(network, 1, "first", 1883, "old user", "old secret");
        mqttCall(network, 3, "second", 65535, "new user", "new secret");
    });
    test("unknown/no-secret-echo", []() {
        FakeNetwork network;
        for (const auto& command : {std::string("NET UNKNOWN private-password"),
                std::string("NET UNKNOWN ") + hex("private-password"),
                std::string("NET STATUS private-password"), std::string("NET WIFI"),
                std::string("NET MQTT"), std::string("NET ")}) {
            response(command, true, network, "unknown_command");
            CHECK(network.calls.empty());
        }
    });
    test("unhandled/input-and-output-untouched", []() {
        FakeNetwork network;
        for (const char* command : {static_cast<const char*>(nullptr), "", "NET", "net STATUS",
                                   "NETX STATUS", " NET STATUS", "OTHER private-password"}) {
            char output[] = "unchanged";
            CHECK(!BrainNetworkConsole::handle(command, true, network, output, sizeof(output)));
            CHECK(std::string(output) == "unchanged");
            CHECK(network.calls.empty());
        }
    });
    test("reply/bounded-output", []() {
        for (size_t capacity : {size_t(0), size_t(1), size_t(5)}) {
            FakeNetwork network;
            char output[8];
            std::memset(output, '!', sizeof(output));
            CHECK(BrainNetworkConsole::handle("NET STATUS", false, network, output + 1, capacity));
            CHECK(output[0] == '!');
            for (size_t i = capacity + 1; i < sizeof(output); ++i) CHECK(output[i] == '!');
            if (capacity) CHECK(output[capacity] == '\0');
        }
    });
}

void pending(MaintenanceLineReader& reader, const std::string& text, uint32_t now) {
    for (char byte : text) CHECK(reader.feed(static_cast<uint8_t>(byte), now) == Result::None);
}

void cleared(const MaintenanceLineReader& reader) {
    // Inspect the entire public buffer, not just the first NUL.
    for (size_t i = 0; i < MaintenanceLineReader::kCapacity; ++i) CHECK(reader.line()[i] == '\0');
}

void readerRetry(MaintenanceLineReader& reader, FakeNetwork& network, uint32_t now) {
    pending(reader, wifi("retry", ""), now);
    CHECK(reader.feed('\n', now) == Result::Line);
    response(reader.line(), true, network, "wifi_saved");
    CHECK(network.calls.size() == 1);
    wifiCall(network, 0, "retry", "");
    reader.clear();
    cleared(reader);
}

void readerBoundaries() {
    static_assert(MaintenanceLineReader::kCapacity == 768, "USB line capacity contract");
    static_assert(MaintenanceLineReader::kPollBytes == 64, "USB poll budget contract");
    static_assert(MaintenanceLineReader::kTimeoutMs == 2000, "USB timeout contract");
    for (char delimiter : std::string("\r\n")) {
        test("reader/767-accepted/delimiter=" + std::to_string(delimiter), [=]() {
            MaintenanceLineReader reader;
            const std::string line = "NET UNKNOWN " + std::string(755, 's');
            CHECK(line.size() == 767);
            pending(reader, line, 0);
            CHECK(reader.feed(static_cast<uint8_t>(delimiter), 0) == Result::Line);
            CHECK(std::string(reader.line()) == line);
            CHECK(reader.line()[767] == '\0');
            FakeNetwork network;
            response(reader.line(), true, network, "unknown_command");
            CHECK(network.calls.empty());
            reader.clear();
            cleared(reader);
            CHECK(reader.feed('\n', 0) == Result::None);
            readerRetry(reader, network, 10000);
        });
        for (size_t length : {size_t(768), size_t(769), size_t(4096)}) {
            test("reader/overflow=" + std::to_string(length) +
                 "/delimiter=" + std::to_string(delimiter), [=]() {
                MaintenanceLineReader reader;
                FakeNetwork network;
                const std::string command = wifi("private ssid", "private password");
                pending(reader, command + std::string(length - command.size(), 's'), 0);
                // Neither the original valid prefix nor a valid suffix may be dispatched.
                pending(reader, wifi("injected", ""), 0);
                CHECK(reader.feed(static_cast<uint8_t>(delimiter), 0) == Result::Rejected);
                cleared(reader);
                CHECK(network.calls.empty());
                CHECK(reader.feed('\n', 0) == Result::None);
                readerRetry(reader, network, 1);
            });
        }
    }
    test("reader/sensitive-clear-and-shorter-command", []() {
        MaintenanceLineReader reader;
        FakeNetwork network;
        pending(reader, wifi(std::string(32, 's'), std::string(127, 'p')), 0);
        CHECK(reader.feed('\n', 0) == Result::Line);
        response(reader.line(), true, network, "wifi_saved");
        CHECK(network.calls.size() == 1);
        wifiCall(network, 0, std::string(32, 's'), std::string(127, 'p'));
        reader.clear();
        cleared(reader);
        pending(reader, "NET STATUS", 1);
        CHECK(reader.feed('\n', 1) == Result::Line);
        CHECK(std::string(reader.line()) == "NET STATUS");
        for (size_t i = 10; i < MaintenanceLineReader::kCapacity; ++i) CHECK(reader.line()[i] == '\0');
        response(reader.line(), false, network, "brain_ready");
        CHECK(network.calls.size() == 2);
        CHECK(network.calls[1].method == "started");
        reader.clear();
        cleared(reader);
    });
}

void readerFragmentsAndTimeouts() {
    const std::string host(127, 'h'), user(63, 'u'), password(127, 'p');
    const std::string command = mqtt(host, "65535", user, password);
    for (size_t chunk : {size_t(1), size_t(7), size_t(63), size_t(64)}) {
        test("reader/max-mqtt/chunk=" + std::to_string(chunk), [=]() {
            CHECK(command.size() == 651);
            MaintenanceLineReader reader;
            FakeNetwork network;
            uint32_t now = std::numeric_limits<uint32_t>::max() - 1000;
            for (size_t offset = 0; offset < command.size(); offset += chunk) {
                pending(reader, command.substr(offset, chunk), now);
                now += MaintenanceLineReader::kTimeoutMs - 1;
            }
            CHECK(reader.feed('\n', now) == Result::Line);
            CHECK(std::string(reader.line()) == command);
            response(reader.line(), true, network, "mqtt_saved");
            CHECK(network.calls.size() == 2);
            CHECK(network.calls[0].method == "started");
            mqttCall(network, 1, host, 65535, user, password);
            reader.clear();
            cleared(reader);
        });
    }
    for (uint32_t start : {uint32_t(0), std::numeric_limits<uint32_t>::max() - 1000}) {
        for (uint32_t gap : {uint32_t(1999), uint32_t(2000), uint32_t(2001)}) {
            for (bool split : {false, true}) {
                test("reader/timeout/start=" + std::to_string(start) + "/gap=" +
                     std::to_string(gap) + "/split=" + std::to_string(split), [=]() {
                    MaintenanceLineReader reader;
                    FakeNetwork network;
                    const std::string line = wifi("private ssid", "private password");
                    const size_t at = split ? size_t(12) : line.size();
                    pending(reader, line.substr(0, at), start);
                    pending(reader, line.substr(at), start + gap);
                    if (gap < MaintenanceLineReader::kTimeoutMs) {
                        CHECK(reader.feed('\n', start + gap) == Result::Line);
                        response(reader.line(), true, network, "wifi_saved");
                        CHECK(network.calls.size() == 1);
                        wifiCall(network, 0, "private ssid", "private password");
                        reader.clear();
                        cleared(reader);
                    } else {
                        CHECK(reader.feed('\n', start + gap) == Result::Rejected);
                        cleared(reader);
                        CHECK(network.calls.empty());
                        readerRetry(reader, network, start + gap);
                    }
                });
            }
        }
    }
    test("reader/timeout-drops-valid-suffix-until-delimiter", []() {
        MaintenanceLineReader reader;
        FakeNetwork network;
        pending(reader, "NET WIFI 736563726574", 0);
        pending(reader, wifi("injected", ""), 2000);
        pending(reader, "NET STATUS", 5000);
        CHECK(reader.feed('\n', 5000) == Result::Rejected);
        cleared(reader);
        CHECK(network.calls.empty());
        readerRetry(reader, network, 5001);
    });
    for (uint8_t byte : {uint8_t(0), uint8_t('\t'), uint8_t(0x7f), uint8_t(0xff)}) {
        test("reader/invalid-byte=" + std::to_string(byte), [=]() {
            MaintenanceLineReader reader;
            FakeNetwork network;
            pending(reader, "NET WIFI 736563726574", 0);
            CHECK(reader.feed(byte, 0) == Result::None);
            pending(reader, wifi("injected", ""), 0);
            CHECK(reader.feed('\n', 0) == Result::Rejected);
            cleared(reader);
            CHECK(network.calls.empty());
            readerRetry(reader, network, 1);
        });
    }
}
}  // namespace

int main() {
    statusAndGates();
    validConfigurations();
    malformedConfigurations();
    failuresAndUnknown();
    readerBoundaries();
    readerFragmentsAndTimeouts();
    std::cout << "Brain network console: " << cases << " cases, " << checks
              << " checks, " << failures << " failures\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
