#include "ProductBoardMessages.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace babytech;
using boardlink::CloudCommand;
using boardlink::CommandMessage;
using boardlink::ProductCommand;
using boardlink::ProductRequest;
using v4::Message;

namespace {
size_t rejected = 0, roundTrips = 0, floatSamples = 0;
std::string phase;
constexpr const char* kDevice = "Babytech_unit-01";
constexpr const char* kSession = "1234567890abcdef1234567890abcdef";
constexpr const char* kNames[] = {"", "initialize", "prepare", "clean",
                                 "set_target_temp", "reset_error", "check_firmware_update"};
constexpr const char* kRecipe[] = {"baby_id", "feeding_context_profile_version", "water_ml",
                                  "temp", "powder_g_per_100ml"};

#define CHECK(condition) do { if (!(condition)) \
    throw std::runtime_error(phase + ": line " + std::to_string(__LINE__) + ": " #condition); \
} while (false)

template <typename T> std::array<unsigned char, sizeof(T)> bytes(const T& value) {
    std::array<unsigned char, sizeof(T)> result{};
    std::memcpy(result.data(), &value, sizeof(value));
    return result;
}

template <size_t N> void set(char (&target)[N], const std::string& text) {
    CHECK(text.size() < N);
    std::memset(target, 0, N);
    std::memcpy(target, text.data(), text.size());
}

uint32_t floatBits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float fromBits(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::string decimal(float value) {
    char text[48];
    const int count = std::snprintf(text, sizeof(text), "%.9g", static_cast<double>(value));
    CHECK(count > 0 && size_t(count) < sizeof(text));
    return text;
}

// Lexical fixtures deliberately retain invalid numbers and duplicate/escaped keys.
// Do not feed them through a normal JSON serializer, which would normalize them.
std::string quote(const std::string& value) {
    std::string result = "\"";
    for (unsigned char c : value) {
        if (c < 0x20) {
            char escaped[7];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", unsigned(c));
            result += escaped;
        } else {
            if (c == '"' || c == '\\') result += '\\';
            result += char(c);
        }
    }
    return result + '"';
}

using Fields = std::vector<std::pair<std::string, std::string>>;

std::string json(const Fields& fields) {
    std::string result = "{";
    for (const auto& field : fields) {
        if (result.size() > 1) result += ',';
        result += quote(field.first) + ':' + field.second;
    }
    return result + '}';
}

Fields changed(Fields fields, const std::string& key, const std::string& token) {
    for (auto& field : fields) {
        if (field.first == key) { field.second = token; return fields; }
    }
    fields.emplace_back(key, token);
    return fields;
}

Fields removed(Fields fields, const std::string& key) {
    fields.erase(std::remove_if(fields.begin(), fields.end(), [&](const auto& field) {
        return field.first == key;
    }), fields.end());
    return fields;
}

CommandMessage command(ProductCommand kind = ProductCommand::Prepare,
                       v4::Source source = v4::Source::CloudCommand) {
    CommandMessage result;
    result.remainingTtlMs = 5000;
    auto& r = result.request;
    r.source = source;
    r.command = kind;
    r.sequence = UINT64_C(9007199254740993);
    set(r.deviceId, kDevice);
    set(r.commandId, "command-01");
    if (kind == ProductCommand::Prepare) {
        set(r.babyId, "baby-01");
        r.profileVersion = 123;
        r.waterMl = 180;
        r.temperatureC = 45;
        r.powderGPer100Ml = fromBits(UINT32_C(0x41555555));
    } else if (kind == ProductCommand::SetTargetTemp) r.temperatureC = 45;
    return result;
}

Fields fields(const CommandMessage& value, bool cloud) {
    const auto& r = value.request;
    Fields result{{"device_id", quote(r.deviceId)}, {"command_id", quote(r.commandId)},
                  {"command", quote(kNames[unsigned(r.command)])},
                  {cloud ? "command_seq" : "seq", quote(std::to_string(r.sequence))},
                  {"ttl_ms", std::to_string(cloud ? 5000 : value.remainingTtlMs)}};
    if (cloud) {
        result.emplace_back("command_session", quote(kSession));
        result.emplace_back("device_uptime_ms", "4294967295");
    } else result.emplace_back("source", quote(r.source == v4::Source::CloudCommand
                                               ? "cloud_command" : "local_touch"));
    if (r.command == ProductCommand::Prepare) {
        result.emplace_back("baby_id", quote(r.babyId));
        result.emplace_back("feeding_context_profile_version", std::to_string(r.profileVersion));
        result.emplace_back("water_ml", std::to_string(r.waterMl));
        result.emplace_back("powder_g_per_100ml", decimal(r.powderGPer100Ml));
    }
    if (r.command == ProductCommand::Prepare || r.command == ProductCommand::SetTargetTemp)
        result.emplace_back("temp", std::to_string(r.temperatureC));
    return result;
}

Message raw(const std::string& text) {
    Message result;
    CHECK(text.size() <= sizeof(result.payload));
    result.kind = v4::Kind::Command;
    result.length = uint16_t(text.size());
    std::memcpy(result.payload, text.data(), text.size());
    return result;
}

std::string payload(const Message& message) {
    CHECK(message.length <= sizeof(message.payload));
    return {reinterpret_cast<const char*>(message.payload), message.length};
}

void same(const ProductRequest& actual, const ProductRequest& expected) {
    CHECK(actual.source == expected.source && actual.command == expected.command);
    CHECK(actual.sequence == expected.sequence);
    CHECK(std::string(actual.deviceId) == expected.deviceId);
    CHECK(std::string(actual.commandId) == expected.commandId);
    CHECK(std::string(actual.babyId) == expected.babyId);
    CHECK(actual.profileVersion == expected.profileVersion && actual.waterMl == expected.waterMl);
    CHECK(actual.temperatureC == expected.temperatureC);
    CHECK(floatBits(actual.powderGPer100Ml) == floatBits(expected.powderGPer100Ml));
}

void reject(const std::string& text, bool cloud, const char* expected = kDevice) {
    if (cloud) {
        CloudCommand output;
        output.request = command().request;
        set(output.session, "abcdef1234567890abcdef1234567890");
        output.sampledAtMs = 123;
        output.ttlMs = 321;
        const auto before = bytes(output);
        CHECK(!boardlink::decodeCloudCommand(reinterpret_cast<const uint8_t*>(text.data()),
                                             text.size(), expected, output));
        CHECK(bytes(output) == before);
    } else {
        CommandMessage output = command();
        output.remainingTtlMs = 321;
        const auto before = bytes(output);
        const Message wire = raw(text);
        const auto wireBefore = bytes(wire);
        CHECK(!boardlink::decodeCommand(wire, output));
        CHECK(bytes(output) == before && bytes(wire) == wireBefore);
    }
    ++rejected;
}

void rejectMessage(const Message& wire) {
    CommandMessage output = command();
    const auto before = bytes(output);
    const auto wireBefore = bytes(wire);
    CHECK(!boardlink::decodeCommand(wire, output));
    CHECK(bytes(output) == before && bytes(wire) == wireBefore);
    ++rejected;
}

void rejectEncode(const CommandMessage& input) {
    Message output = raw(json(fields(command(), false)));
    output.senderBoot = 123;
    output.receiverBoot = 456;
    output.messageId = 789;
    std::memset(output.payload + output.length, 0xa5, sizeof(output.payload) - output.length);
    const auto before = bytes(output);
    const auto inputBefore = bytes(input);
    CHECK(!boardlink::encodeCommand(input, output));
    CHECK(bytes(output) == before && bytes(input) == inputBefore);
    ++rejected;
}

Message roundtrip(const CommandMessage& input) {
    Message wire;
    wire.kind = v4::Kind::Stop;
    wire.senderBoot = 111;
    wire.receiverBoot = 222;
    wire.messageId = 333;
    std::memset(wire.payload, 0xa5, sizeof(wire.payload));
    const auto before = bytes(input);
    CHECK(boardlink::encodeCommand(input, wire));
    CHECK(bytes(input) == before);
    CHECK(wire.kind == v4::Kind::Command && !wire.senderBoot && !wire.receiverBoot && !wire.messageId);
    CHECK(wire.length > 0 && wire.length <= v4::kMaxMessage);
    for (size_t i = wire.length; i < sizeof(wire.payload); ++i) CHECK(wire.payload[i] == 0);
    StaticJsonDocument<8192> doc;
    CHECK(!deserializeJson(doc, static_cast<const uint8_t*>(wire.payload), wire.length));
    CHECK(doc.size() == fields(input, false).size());
    CHECK(doc["seq"].is<JsonString>() && doc["seq"].as<std::string>() == std::to_string(input.request.sequence));
    CHECK(doc["command"].as<std::string>() == kNames[unsigned(input.request.command)]);
    CHECK(doc["source"].as<std::string>() == (input.request.source == v4::Source::CloudCommand
                                            ? "cloud_command" : "local_touch"));
    CHECK(doc["ttl_ms"].as<uint16_t>() == input.remainingTtlMs);
    CommandMessage output = command();
    CHECK(boardlink::decodeCommand(wire, output));
    same(output.request, input.request);
    CHECK(output.remainingTtlMs == input.remainingTtlMs);
    Message again;
    CHECK(boardlink::encodeCommand(output, again) && payload(again) == payload(wire));
    // Independently authored JSON also has to decode to the same full request.
    const Message fixture = raw(json(fields(input, false)));
    CHECK(boardlink::decodeCommand(fixture, output));
    same(output.request, input.request);
    CHECK(output.remainingTtlMs == input.remainingTtlMs);
    ++roundTrips;
    return wire;
}

CloudCommand cloudRoundtrip(const std::string& text) {
    StaticJsonDocument<8192> doc;
    CHECK(!deserializeJson(doc, text) && doc["device_id"].is<JsonString>());
    const std::string device = doc["device_id"].as<std::string>();
    CloudCommand output;
    const auto inputBefore = text;
    CHECK(boardlink::decodeCloudCommand(reinterpret_cast<const uint8_t*>(text.data()),
                                         text.size(), device.c_str(), output));
    CHECK(text == inputBefore);
    CHECK(output.request.source == v4::Source::CloudCommand);
    CHECK(std::string(output.request.deviceId) == device);
    CHECK(std::string(output.request.commandId) == doc["command_id"].as<std::string>());
    const std::string name = doc["command"].as<std::string>();
    unsigned kind = 0;
    for (unsigned i = 1; i < std::size(kNames); ++i) if (name == kNames[i]) kind = i;
    CHECK(kind > 1 && unsigned(output.request.command) == kind);
    CHECK(doc["command_seq"].is<JsonString>());
    CHECK(output.request.sequence == std::stoull(doc["command_seq"].as<std::string>()));
    CHECK(std::string(output.session) == doc["command_session"].as<std::string>());
    CHECK(output.sampledAtMs == doc["device_uptime_ms"].as<uint32_t>() && output.ttlMs == 5000);
    if (output.request.command == ProductCommand::Prepare) {
        CHECK(std::string(output.request.babyId) == doc["baby_id"].as<std::string>());
        CHECK(output.request.profileVersion == doc["feeding_context_profile_version"].as<uint32_t>());
        CHECK(output.request.waterMl == doc["water_ml"].as<uint16_t>());
        const float expected = static_cast<float>(doc["powder_g_per_100ml"].as<double>());
        CHECK(floatBits(output.request.powderGPer100Ml) == floatBits(expected));
    } else {
        CHECK(!output.request.babyId[0] && !output.request.profileVersion && !output.request.waterMl);
        CHECK(floatBits(output.request.powderGPer100Ml) == 0);
    }
    if (output.request.command == ProductCommand::Prepare || output.request.command == ProductCommand::SetTargetTemp)
        CHECK(output.request.temperatureC == doc["temp"].as<uint8_t>());
    else CHECK(output.request.temperatureC == 0);
    CHECK(doc.size() == (kind == unsigned(ProductCommand::Prepare) ? 12u :
                        kind == unsigned(ProductCommand::SetTargetTemp) ? 8u : 7u));
    CommandMessage uart;
    uart.request = output.request;
    uart.remainingTtlMs = output.ttlMs;
    roundtrip(uart);
    return output;
}

void combinations() {
    phase = "all commands/sources/sequence/TTL combinations";
    for (unsigned kind = 1; kind < std::size(kNames); ++kind) {
        for (auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch}) {
            for (uint64_t seq : {UINT64_C(1), UINT64_C(9007199254740993), v4::kMaxSequence}) {
                for (uint16_t ttl : {uint16_t(1), uint16_t(5000)}) {
                    auto input = command(ProductCommand(kind), source);
                    input.request.sequence = seq;
                    input.remainingTtlMs = ttl;
                    if (kind == 1 && source == v4::Source::CloudCommand) {
                        rejectEncode(input);
                        reject(json(fields(input, false)), false);
                    } else roundtrip(input);
                }
                if (source == v4::Source::CloudCommand) {
                    const auto input = command(ProductCommand(kind), source);
                    auto fixture = fields(input, true);
                    fixture = changed(fixture, "command_seq", quote(std::to_string(seq)));
                    if (kind == 1) reject(json(fixture), true);
                    else {
                        auto expected = input.request;
                        expected.sequence = seq;
                        same(cloudRoundtrip(json(fixture)).request, expected);
                        fixture = changed(fixture, "device_uptime_ms", "0");
                        CHECK(cloudRoundtrip(json(fixture)).sampledAtMs == 0);
                    }
                }
            }
        }
    }
    for (bool cloud : {false, true}) {
        const auto fixture = fields(command(), cloud);
        for (const char* name : {"", "stop", "interrupt", "Prepare", "target_temp", "unknown"})
            reject(json(changed(fixture, "command", quote(name))), cloud);
        for (const char* token : {"0", "1", "255"})
            reject(json(changed(fixture, "command", token)), cloud);
    }
    for (const char* source : {"", "cloud", "local", "Cloud_Command", "motion", "0", "1"})
        reject(json(changed(fields(command(), false), "source", quote(source))), false);
}

void fieldValidation() {
    phase = "each field missing/type/unknown/duplicate";
    for (bool cloud : {false, true}) {
        for (unsigned kind = 1; kind < std::size(kNames); ++kind) {
            if (cloud && kind == 1) continue;
            const auto fixture = fields(command(ProductCommand(kind), cloud ? v4::Source::CloudCommand
                                                                            : v4::Source::LocalTouch), cloud);
            for (const auto& field : fixture) {
                phase = std::string(cloud ? "Cloud " : "UART ") + kNames[kind] + " field " + field.first;
                reject(json(removed(fixture, field.first)), cloud);
                for (const char* token : {"null", "true", "false", "[]", "{}"})
                    reject(json(changed(fixture, field.first, token)), cloud);
                const bool string = field.second[0] == '"';
                for (const char* token : (string ? std::initializer_list<const char*>{"0", "1", "1.0"}
                                                : std::initializer_list<const char*>{"\"1\"", "\"5000\"", "\"\""}))
                    reject(json(changed(fixture, field.first, token)), cloud);
                reject(json(changed(removed(fixture, field.first), "unknown_field", field.second)), cloud);
                auto duplicate = fixture;
                duplicate.emplace_back(field);
                reject(json(duplicate), cloud);
                duplicate = fixture;
                duplicate.insert(duplicate.begin(), {field.first, "null"});
                reject(json(duplicate), cloud);
                std::string escaped = quote(field.first);
                char escape[7];
                std::snprintf(escape, sizeof(escape), "\\u%04x", unsigned(static_cast<unsigned char>(field.first[0])));
                escaped.replace(1, 1, escape);
                reject("{" + escaped + ':' + field.second + ',' + json(fixture).substr(1), cloud);
                // Preserve the lexical field count while concealing a missing key.
                const auto& other = fixture[field.first == fixture.front().first ? 1 : 0];
                auto missing = removed(fixture, field.first);
                missing.emplace_back(other);
                reject(json(missing), cloud);
                auto withoutOther = removed(fixture, other.first);
                reject("{" + escaped + ':' + field.second + ',' + json(withoutOther).substr(1), cloud);
                if (!string && field.first != "powder_g_per_100ml") {
                    for (const char* token : {"1.0", "1e0", "1.5", "5000.0", "5000e0", "-1",
                                              "4294967296", "18446744073709551616", "-9223372036854775809"})
                        reject(json(changed(fixture, field.first, token)), cloud);
                }
            }
            for (const char* key : {"unknown", "ratio", "session", "command_session_challenge", "target_temp"})
                reject(json(changed(fixture, key, "1")), cloud);
            for (const char* key : (cloud ? std::initializer_list<const char*>{"source", "seq"}
                                          : std::initializer_list<const char*>{"command_seq", "command_session", "device_uptime_ms"}))
                reject(json(changed(fixture, key, "1")), cloud);
            for (const char* key : kRecipe) {
                const auto prepare = fields(command(), cloud);
                auto found = std::find_if(prepare.begin(), prepare.end(), [&](const auto& f) { return f.first == key; });
                CHECK(found != prepare.end());
                if (kind != unsigned(ProductCommand::Prepare) &&
                    !(kind == unsigned(ProductCommand::SetTargetTemp) && std::string(key) == "temp"))
                    reject(json(changed(fixture, key, found->second)), cloud);
            }
        }
    }
}

void ranges() {
    phase = "numeric and string boundaries";
    for (bool cloud : {false, true}) {
        const auto fixture = fields(command(), cloud);
        const char* seqKey = cloud ? "command_seq" : "seq";
        for (const char* seq : {"", "0", "00", "01", "+1", "-1", " 1", "1 ", "1.0", "1e0", "1\n",
                                "9223372036854775808", "18446744073709551615", "18446744073709551616",
                                "999999999999999999999999999999", "\xef\xbc\x91"})
            reject(json(changed(fixture, seqKey, quote(seq))), cloud);
        for (const char* token : {"0", "5001", "65536", "-1"})
            reject(json(changed(fixture, "ttl_ms", token)), cloud);
        if (cloud) {
            for (const char* token : {"1", "4999"}) reject(json(changed(fixture, "ttl_ms", token)), true);
            for (const char* session : {"", "abc", "00000000000000000000000000000000",
                                        "ABCDEF1234567890abcdef1234567890", "1234567890abcdef1234567890abcdef0",
                                        "1234567890abcdef1234567890abcdeg"})
                reject(json(changed(fixture, "command_session", quote(session))), true);
            reject(json(fixture), true, "Babytech_other");
            reject(json(fixture), true, nullptr);
            reject(json(fixture), true, "");
        }
        for (const char* token : {"0", "2147483648", "4294967295"})
            reject(json(changed(fixture, "feeding_context_profile_version", token)), cloud);
        for (const char* token : {"0", "29", "501", "65536"})
            reject(json(changed(fixture, "water_ml", token)), cloud);
        for (const char* token : {"0", "34", "61", "256"})
            reject(json(changed(fixture, "temp", token)), cloud);
        for (const char* token : {"0", "-0", "-0.0", "0.999", "50.001", "1e999", "-1e999", "1e-999",
                                  "NaN", "Infinity", "-Infinity"})
            reject(json(changed(fixture, "powder_g_per_100ml", token)), cloud);
        for (const char* id : {"", "_unit", "unit/1", "unit 1", "unit.1", "\xe5\xae\x9d"})
            reject(json(changed(fixture, "device_id", quote(id))), cloud);
        reject(json(changed(fixture, "device_id", quote(std::string(65, 'd')))), cloud);
        reject(json(changed(fixture, "command_id", "\"\"")), cloud);
        reject(json(changed(fixture, "command_id", quote(std::string(129, 'c')))), cloud);
        reject(json(changed(fixture, "baby_id", "\"\"")), cloud);
        reject(json(changed(fixture, "baby_id", quote(std::string(97, 'b')))), cloud);
        for (auto seq : {UINT64_C(9), UINT64_C(10), UINT64_C(999999999999999999), v4::kMaxSequence}) {
            auto input = command(); input.request.sequence = seq;
            if (cloud) same(cloudRoundtrip(json(fields(input, true))).request, input.request);
            else roundtrip(input);
        }
        for (unsigned edge = 0; edge < 2; ++edge) {
            auto input = command();
            input.request.profileVersion = edge ? INT32_MAX : 1;
            input.request.waterMl = edge ? 500 : 30;
            input.request.temperatureC = edge ? 60 : 35;
            input.request.powderGPer100Ml = edge ? 50.0f : 1.0f;
            if (cloud) same(cloudRoundtrip(json(fields(input, true))).request, input.request);
            else roundtrip(input);
            input = command(ProductCommand::SetTargetTemp);
            input.request.temperatureC = edge ? 60 : 35;
            if (cloud) same(cloudRoundtrip(json(fields(input, true))).request, input.request);
            else roundtrip(input);
        }
    }
}

void malformed() {
    phase = "strict flat JSON / UTF-8 / NUL";
    const std::vector<std::string> invalidUtf8 = {
        "\x80", "\xc0\xaf", "\xc1\xbf", "\xc2", "\xe5\xae", "\xe0\x80\x80",
        "\xed\xa0\x80", "\xf0\x80\x80\x80", "\xf4\x90\x80\x80\x80", "\xf5\x80\x80\x80", "\xff"};
    for (bool cloud : {false, true}) {
        const auto fixture = fields(command(), cloud);
        const auto good = json(fixture);
        for (const char* token : {"", "null", "true", "false", "[]", "1", "\"x\"", "{}", "{", "{\"x\":1,}"})
            reject(token, cloud);
        reject(good + "{}", cloud);
        reject(good + std::string(1, '\0'), cloud);
        reject(std::string(1, '\0') + good, cloud);
        reject(good.substr(0, good.size() - 1), cloud);
        reject(good.substr(0, good.size() - 1) + ",}", cloud);
        reject("/*comment*/" + good, cloud);
        reject(good + "//comment", cloud);
        reject("{device_id:" + quote(kDevice) + ',' + good.substr(good.find(',') + 1), cloud);
        for (const char* token : {"+1", "01", "00", ".1", "1.", "-", "--1", "1e", "1e+", "'1'"})
            reject(json(changed(fixture, "powder_g_per_100ml", token)), cloud);
        for (const auto& field : fixture) {
            if (field.second[0] != '"') continue;
            for (const char* token : {"\"x\\u0000suffix\"", "\"\\uD800\"", "\"\\uDC00\"",
                                      "\"\\uD800\\u0041\"", "\"\\x41\"", "\"\\uZZZZ\"", "\"\\q\""})
                reject(json(changed(fixture, field.first, token)), cloud);
            reject(json(changed(fixture, field.first, quote(std::string("a\0b", 3)))), cloud);
            reject(json(changed(fixture, field.first, std::string("\"a\0b\"", 5))), cloud);
            for (const auto& bad : invalidUtf8)
                reject(json(changed(fixture, field.first, quote(bad))), cloud);
            reject(json(changed(fixture, field.first, "\"a\nb\"")), cloud);
        }
        const auto escaped = changed(fixture, "command_id", "\"quote\\\"slash\\/back\\\\\\b\\f\\n\\r\\t\\u0001\\u5b9d\\ud83c\\udf7c\"");
        const std::string expected = "quote\"slash/back\\\b\f\n\r\t\x01\xe5\xae\x9d\xf0\x9f\x8d\xbc";
        if (cloud) CHECK(std::string(cloudRoundtrip(json(escaped)).request.commandId) == expected);
        else {
            CommandMessage decoded;
            CHECK(boardlink::decodeCommand(raw(" \t\r\n" + json(escaped) + "\n\r\t "), decoded));
            CHECK(std::string(decoded.request.commandId) == expected);
        }
    }
    CloudCommand output;
    output.request = command().request;
    const auto before = bytes(output);
    CHECK(!boardlink::decodeCloudCommand(nullptr, 1, kDevice, output));
    CHECK(bytes(output) == before);
    CHECK(!boardlink::decodeCloudCommand(nullptr, 0, kDevice, output));
    CHECK(bytes(output) == before);
    rejected += 2;
}

void encoderValidation() {
    phase = "encoder invalid input and atomic failure";
    for (uint16_t ttl : {uint16_t(0), uint16_t(5001), uint16_t(UINT16_MAX)}) {
        auto input = command(); input.remainingTtlMs = ttl; rejectEncode(input);
    }
    for (uint64_t seq : {UINT64_C(0), v4::kMaxSequence + 1, UINT64_MAX}) {
        auto input = command(); input.request.sequence = seq; rejectEncode(input);
    }
    for (unsigned source : {0u, 3u, 255u}) {
        auto input = command(); input.request.source = v4::Source(source); rejectEncode(input);
    }
    for (unsigned kind : {0u, 7u, 255u}) {
        auto input = command(); input.request.command = ProductCommand(kind); rejectEncode(input);
    }
    for (const char* id : {"", "_bad", "bad/id", "bad id", "\xe5\xae\x9d"}) {
        auto input = command(); set(input.request.deviceId, id); rejectEncode(input);
    }
    auto input = command(); std::memset(input.request.deviceId, 'd', sizeof(input.request.deviceId)); rejectEncode(input);
    input = command(); std::memset(input.request.commandId, 'c', sizeof(input.request.commandId)); rejectEncode(input);
    input = command(); set(input.request.commandId, ""); rejectEncode(input);
    input = command(); set(input.request.commandId, "\xed\xa0\x80"); rejectEncode(input);
    input = command(); std::memset(input.request.babyId, 'b', sizeof(input.request.babyId)); rejectEncode(input);
    input = command(); set(input.request.babyId, ""); rejectEncode(input);
    input = command(); set(input.request.babyId, "\xe5\xae"); rejectEncode(input);
    for (uint32_t version : {uint32_t(0), uint32_t(INT32_MAX) + 1, UINT32_MAX}) {
        input = command(); input.request.profileVersion = version; rejectEncode(input);
    }
    for (uint16_t water : {uint16_t(0), uint16_t(29), uint16_t(501), uint16_t(UINT16_MAX)}) {
        input = command(); input.request.waterMl = water; rejectEncode(input);
    }
    for (unsigned temp : {0u, 34u, 61u, 255u}) {
        for (auto kind : {ProductCommand::Prepare, ProductCommand::SetTargetTemp}) {
            input = command(kind); input.request.temperatureC = uint8_t(temp); rejectEncode(input);
        }
    }
    for (float powder : {0.0f, -0.0f, std::nextafter(1.0f, 0.0f), std::nextafter(50.0f, 100.0f),
                          std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::quiet_NaN()}) {
        input = command(); input.request.powderGPer100Ml = powder; rejectEncode(input);
    }
    for (unsigned kind = 1; kind < std::size(kNames); ++kind) {
        if (kind == unsigned(ProductCommand::Prepare)) continue;
        const auto base = command(ProductCommand(kind), v4::Source::LocalTouch);
        input = base; set(input.request.babyId, "extra"); rejectEncode(input);
        input = base; input.request.profileVersion = 1; rejectEncode(input);
        input = base; input.request.waterMl = 30; rejectEncode(input);
        input = base; input.request.powderGPer100Ml = 1; rejectEncode(input);
        input = base; input.request.powderGPer100Ml = -0.0f; rejectEncode(input);
        if (kind != unsigned(ProductCommand::SetTargetTemp)) {
            input = base; input.request.temperatureC = 45; rejectEncode(input);
        }
    }
}

std::string denseText(size_t length) {
    const std::string suffix = "\"\\\xe5\xae\x9d\xf0\x9f\x8d\xbc";
    CHECK(length >= suffix.size());
    std::string result;
    for (size_t i = 0; i < length - suffix.size(); ++i) result += char(1 + i % 31);
    return result + suffix;
}

Message transport(Message message) {
    message.senderBoot = 11;
    message.receiverBoot = 22;
    message.messageId = 33;
    v4::Assembler assembler;
    v4::Parser parser;
    Message assembled;
    for (size_t offset = 0; offset < message.length;) {
        v4::Frame frame;
        CHECK(v4::fragment(message, offset, frame));
        uint8_t wire[v4::kMaxFrame];
        const size_t count = v4::encode(frame, wire, sizeof(wire));
        CHECK(count > 0);
        v4::Frame parsed;
        for (size_t i = 0; i < count; ++i) CHECK(parser.push(wire[i], 100, parsed) == (i + 1 == count));
        offset += frame.length;
        CHECK(assembler.accept(parsed, 100, assembled) == (offset == message.length
            ? v4::AssemblyResult::Complete : v4::AssemblyResult::Incomplete));
    }
    CHECK(payload(assembled) == payload(message) && assembled.kind == message.kind);
    CHECK(assembled.senderBoot == 11 && assembled.receiverBoot == 22 && assembled.messageId == 33);
    return assembled;
}

void stringsAndBudget() {
    phase = "full-length controls/quotes/UTF-8 and 2047/2048 boundary";
    static_assert(v4::kMaxMessage == 2047, "ordinary COMMAND keeps the v4 payload budget");
    auto input = command();
    set(input.request.deviceId, std::string(64, 'd'));
    set(input.request.commandId, denseText(128));
    set(input.request.babyId, denseText(96));
    input.request.profileVersion = INT32_MAX;
    input.request.sequence = v4::kMaxSequence;
    const Message encoded = roundtrip(input);
    CHECK(payload(encoded).find("\\u0001") != std::string::npos);
    for (size_t i = 0; i < encoded.length; ++i) CHECK(encoded.payload[i] >= 0x20);
    CommandMessage output;
    CHECK(boardlink::decodeCommand(transport(encoded), output));
    same(output.request, input.request);
    same(cloudRoundtrip(json(fields(input, true))).request, input.request);
    for (bool cloud : {false, true}) {
        const std::string base = json(fields(input, cloud));
        CHECK(base.size() <= v4::kMaxMessage);
        const std::string atLimit = std::string(v4::kMaxMessage - base.size(), ' ') + base;
        CHECK(atLimit.size() == 2047);
        if (cloud) {
            same(cloudRoundtrip(atLimit).request, input.request);
            reject(atLimit + ' ', true, input.request.deviceId);
        } else {
            CHECK(boardlink::decodeCommand(transport(raw(atLimit)), output));
            same(output.request, input.request);
            Message tooLong = raw(atLimit);
            tooLong.length = 2048;  // Must fail before reading beyond the fixed payload.
            rejectMessage(tooLong);
            tooLong.length = UINT16_MAX;
            rejectMessage(tooLong);
        }
    }
    const auto ordinary = raw(json(fields(command(), false)));
    for (unsigned kind = 0; kind <= unsigned(v4::Kind::Stop) + 1; ++kind) {
        if (kind == unsigned(v4::Kind::Command)) continue;
        auto wrong = ordinary; wrong.kind = v4::Kind(kind); rejectMessage(wrong);
    }
    auto empty = ordinary; empty.length = 0; rejectMessage(empty);
    auto invalidKind = ordinary; invalidKind.kind = v4::Kind(255); rejectMessage(invalidKind);
    // Shift a multibyte ID through every fragment alignment, including splits.
    auto unicode = command();
    set(unicode.request.commandId, std::string(118, 'a') + "\xe5\xae\x9d\xf0\x9f\x8d\xbc");
    const std::string text = payload(roundtrip(unicode));
    bool split = false;
    for (size_t padding = 0; padding < v4::kMaxFragment; ++padding) {
        const auto wire = raw(std::string(padding, ' ') + text);
        for (size_t offset = v4::kMaxFragment; offset < wire.length; offset += v4::kMaxFragment)
            split |= (wire.payload[offset] & 0xc0) == 0x80;
        CHECK(boardlink::decodeCommand(transport(wire), output));
        same(output.request, unicode.request);
    }
    CHECK(split);
}

void floatSweep() {
    phase = "float32 exact-bit Cloud -> UART sweep";
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559, "binary32 required");
    const uint32_t first = floatBits(1.0f), last = floatBits(50.0f);
    uint32_t random = UINT32_C(0x6a09e667);
    for (size_t i = 0; i < 12000; ++i) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        const uint32_t bits = first + random % (last - first + 1);
        auto input = command();
        input.request.powderGPer100Ml = fromBits(bits);
        phase = "float32 sample " + std::to_string(i) + " bits=" + std::to_string(bits);
        const auto decoded = cloudRoundtrip(json(fields(input, true)));
        CHECK(floatBits(decoded.request.powderGPer100Ml) == bits);
        same(decoded.request, input.request);
        ++floatSamples;
    }
    // Exercise both endpoints and adjacent ULPs, independently of PRNG coverage.
    for (uint32_t bits : {first, first + 1, last - 1, last, UINT32_C(0x41555555)}) {
        auto input = command(); input.request.powderGPer100Ml = fromBits(bits);
        same(cloudRoundtrip(json(fields(input, true))).request, input.request);
        ++floatSamples;
    }
}

void fixtures(const char* path) {
    std::ifstream input(path, std::ios::binary);
    CHECK(input.is_open());
    size_t count = 0;
    std::string line;
    while (std::getline(input, line)) {
        phase = std::string("Cloud fixture ") + path + ':' + std::to_string(count + 1);
        cloudRoundtrip(line);
        ++count;
    }
    CHECK(!input.bad() && count > 0);
    std::cout << "Cloud fixtures: " << count << " decoded and UART-roundtripped\n";
}
}  // namespace

int main(int argc, char** argv) {
    try {
        CHECK(argc == 1 || (argc == 3 && std::string(argv[1]) == "--cloud-fixtures"));
        combinations();
        fieldValidation();
        ranges();
        malformed();
        encoderValidation();
        stringsAndBudget();
        floatSweep();
        if (argc == 3) fixtures(argv[2]);
        std::cout << "Board command codec: " << roundTrips << " round trips, " << floatSamples
                  << " exact float32 samples, " << rejected << " atomic rejections passed (ArduinoJson "
                  << ARDUINOJSON_VERSION << ")\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Board command codec FAILED: " << error.what() << '\n';
        return 1;
    }
}
