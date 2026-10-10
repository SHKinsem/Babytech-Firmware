#include "ProductBoardMessages.h"

#include <ArduinoJson.h>
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>

using namespace babytech;
using boardlink::Status;
using boardlink::ExecutionOwner;
using v4::Message;
namespace {
size_t rejected = 0;
struct ProductBool {
    const char* key;
    bool Status::*member;
};
constexpr ProductBool kProductBools[] = {
    {"is_preparing", &Status::isPreparing},
    {"low_water_valid", &Status::lowWaterValid},
    {"low_water", &Status::lowWater},
    {"powder_valid", &Status::powderValid},
    {"actuator_operational", &Status::actuatorOperational},
    {"actuator_config_valid", &Status::actuatorConfigValid},
    {"actuator_bus_healthy", &Status::actuatorBusHealthy},
    {"actuator_position_referenced", &Status::actuatorPositionReferenced},
    {"execution_authorized", &Status::executionAuthorized},
    {"feeding_context_configured", &Status::feedingContextConfigured},
};
constexpr const char* kProductStrings[] = {"product_progress", "product_error", "baby_id"};

template <size_t N> void set(char (&target)[N], const std::string& value) {
    assert(value.size() < N);
    std::memset(target, 0, N);
    std::memcpy(target, value.data(), value.size());
}

template <typename T> std::array<unsigned char, sizeof(T)> bytes(const T& value) {
    std::array<unsigned char, sizeof(T)> result{};
    std::memcpy(result.data(), &value, sizeof(T));
    return result;
}

v4::Hello hello() {
    v4::Hello result;
    set(result.deviceId, "Babytech_unit-01");
    set(result.epoch, "1234567890abcdef1234567890abcdef");
    set(result.physicalId, "123456abcdef");
    return result;
}

Status status() {
    Status result;
    auto& s = result.snapshot;
    s.stage = display::DisplayStage::Error;
    s.primaryCondition = display::DisplayCondition::LowWater;
    s.footerCondition = display::DisplayCondition::BabyMissing;
    s.error = display::DisplayError::PowderMotorFault;
    s.cloudConnected = true;
    s.startEnabled = true;  // Observed flags are preserved, not an action ACK.
    s.thermalSimulated = true;
    s.waterMl = UINT16_MAX;
    s.temperatureC = INT16_MIN;
    const std::string name = u8"宝宝\"\\";
    std::memcpy(s.babyName.data(), name.c_str(), name.size() + 1);
    const std::string brand = u8"配方\t品牌";
    std::memcpy(s.formulaBrand.data(), brand.c_str(), brand.size() + 1);
    result.sampleUptimeMs = UINT32_MAX;
    result.contextVersion = INT32_MAX;
    set(result.cloudWatermark, "9223372036854775807");
    set(result.localWatermark, "9007199254740993");
    result.motionBusy = true;
    result.stationary = true;
    result.eventPending = true;
    result.executionOwner = ExecutionOwner::Product;
    set(result.activeExecutionId, "abcdef1234567890abcdef1234567890");
    set(result.pendingEventId, u8"事件-\"\\\n");
    set(result.productProgress, "dispensing_powder");
    set(result.productError, "E_CONTEXT_STORAGE_2");
    for (const auto& field : kProductBools) result.*field.member = true;
    result.powderGrams = INT32_MAX;
    set(result.babyId, std::string(90, 'b') + "\xe5\xae\x9d\xe5\xae\x9d");
    return result;
}

Message helloMessage() {
    Message result;
    assert(boardlink::encodeHello(hello(), result));
    return result;
}

Message statusMessage() {
    Message result;
    assert(boardlink::encodeStatus(status(), result));
    return result;
}

std::string payload(const Message& message) {
    return std::string(reinterpret_cast<const char*>(message.payload), message.length);
}

Message raw(const Message& original, const std::string& text) {
    Message result = original;
    assert(text.size() <= sizeof(result.payload));
    result.length = uint16_t(text.size());
    std::memset(result.payload, 0xa5, sizeof(result.payload));
    std::memcpy(result.payload, text.data(), text.size());
    return result;
}

// Fixtures use ArduinoJson too; serialized() deliberately injects invalid tokens.
Message field(const Message& original, const char* key, const std::string& value) {
    StaticJsonDocument<8192> doc;
    assert(!deserializeJson(doc, static_cast<const uint8_t*>(original.payload), original.length));
    doc[key] = serialized(value);
    assert(!doc.overflowed());
    std::string text;
    serializeJson(doc, text);
    return raw(original, text);
}

Message without(const Message& original, const char* key) {
    StaticJsonDocument<8192> doc;
    assert(!deserializeJson(doc, static_cast<const uint8_t*>(original.payload), original.length));
    doc.remove(key);
    std::string text;
    serializeJson(doc, text);
    return raw(original, text);
}

void reject(const Message& message, bool isHello) {
    Status s = status();
    v4::Hello h = hello();
    h.replyTo = 123;
    const auto sb = bytes(s);
    const auto hb = bytes(h);
    assert(!(isHello ? boardlink::decodeHello(message, h) : boardlink::decodeStatus(message, s)));
    assert(bytes(s) == sb);
    assert(bytes(h) == hb);
    ++rejected;
}

template <typename T> void rejectEncode(const T& input, v4::Kind kind = v4::Kind::Hello) {
    Message output = statusMessage();
    output.senderBoot = 123;
    output.receiverBoot = 456;
    output.messageId = 789;
    const auto before = bytes(output);
    if constexpr (std::is_same<T, Status>::value)
        assert(!boardlink::encodeStatus(input, output));
    else
        assert(!boardlink::encodeHello(input, output, kind));
    assert(bytes(output) == before);
    ++rejected;
}

void roundTrip() {
    Status initial;
    assert(initial.snapshot.stage == display::DisplayStage::NotReady);
    assert(!initial.snapshot.startEnabled && !initial.stationary);
    assert(initial.executionOwner == ExecutionOwner::None);
    Message m;
    assert(boardlink::encodeStatus(initial, m));
    Status copy = status();
    assert(boardlink::decodeStatus(m, copy));
    assert(copy.snapshot.stage == display::DisplayStage::NotReady && !copy.snapshot.startEnabled);
    assert(!copy.stationary && !copy.motionBusy && !copy.eventPending);
    assert(std::string(copy.cloudWatermark) == "0" && std::string(copy.localWatermark) == "0");
    assert(!copy.activeExecutionId[0] && !copy.pendingEventId[0]);
    assert(copy.executionOwner == ExecutionOwner::None);
    assert(std::string(initial.productProgress) == "noready" && std::string(copy.productProgress) == "noready");
    assert(std::string(initial.productError) == "NONE" && std::string(copy.productError) == "NONE");
    assert(initial.powderGrams == 0 && copy.powderGrams == 0 && !initial.babyId[0] && !copy.babyId[0]);
    for (const auto& field : kProductBools) assert(!(initial.*field.member) && !(copy.*field.member));

    m = statusMessage();
    assert(!m.senderBoot && !m.receiverBoot && !m.messageId && m.kind == v4::Kind::Status);
    assert(boardlink::decodeStatus(m, copy));
    const Status original = status();
    assert(copy.snapshot.schemaVersion == original.snapshot.schemaVersion);
    assert(copy.snapshot.stage == original.snapshot.stage);
    assert(copy.snapshot.primaryCondition == original.snapshot.primaryCondition);
    assert(copy.snapshot.footerCondition == original.snapshot.footerCondition);
    assert(copy.snapshot.error == original.snapshot.error);
    assert(copy.snapshot.cloudConnected && copy.snapshot.startEnabled && copy.snapshot.thermalSimulated);
    assert(copy.snapshot.waterMl == UINT16_MAX && copy.snapshot.temperatureC == INT16_MIN);
    assert(copy.snapshot.babyName == original.snapshot.babyName);
    assert(copy.snapshot.formulaBrand == original.snapshot.formulaBrand);
    assert(copy.sampleUptimeMs == UINT32_MAX && copy.contextVersion == INT32_MAX);
    assert(std::string(copy.cloudWatermark) == original.cloudWatermark);
    assert(std::string(copy.localWatermark) == original.localWatermark);
    assert(copy.motionBusy && copy.stationary && copy.eventPending);
    assert(copy.executionOwner == original.executionOwner);
    assert(std::string(copy.activeExecutionId) == original.activeExecutionId);
    assert(std::string(copy.pendingEventId) == original.pendingEventId);
    assert(std::string(copy.productProgress) == original.productProgress);
    assert(std::string(copy.productError) == original.productError);
    assert(std::string(copy.babyId) == original.babyId);
    assert(copy.powderGrams == original.powderGrams);
    for (const auto& field : kProductBools) assert(copy.*field.member);
    Message reencoded;
    assert(boardlink::encodeStatus(copy, reencoded));
    assert(payload(m) == payload(reencoded));

    StaticJsonDocument<8192> json;
    assert(!deserializeJson(json, static_cast<const uint8_t*>(m.payload), m.length));
    assert(json.size() == 36);
    assert(json["execution_owner"].is<JsonString>() && json["execution_owner"].as<std::string>() == "product");
    for (const auto& field : kProductBools) {
        assert(json[field.key].is<bool>() && json[field.key].as<bool>());
    }
    for (const auto* key : kProductStrings) assert(json[key].is<JsonString>());
    assert(json["product_progress"].as<std::string>() == original.productProgress);
    assert(json["product_error"].as<std::string>() == original.productError);
    assert(json["baby_id"].as<std::string>() == original.babyId);
    assert(json["powder_grams"].is<int32_t>() && json["powder_grams"].as<int32_t>() == INT32_MAX);
    assert(json["cloud_watermark"].is<const char*>());
    assert(json["stage"].as<unsigned>() == unsigned(display::DisplayStage::Error));
    assert(!json.containsKey("success") && !json.containsKey("initialized"));

    for (auto role : {v4::Role::Brain, v4::Role::Motion}) {
        for (auto kind : {v4::Kind::Hello, v4::Kind::HelloAck}) {
            auto h = hello();
            h.role = role;
            h.capabilities = UINT32_MAX;
            h.replyTo = kind == v4::Kind::HelloAck ? UINT32_MAX : 0;
            set(h.deviceId, std::string(64, 'x'));
            m = statusMessage();
            m.senderBoot = m.receiverBoot = m.messageId = 123;
            assert(boardlink::encodeHello(h, m, kind));
            assert(m.kind == kind && !m.senderBoot && !m.receiverBoot && !m.messageId);
            v4::Hello decoded;
            assert(boardlink::decodeHello(m, decoded));
            assert(v4::validHello(decoded));
            assert(decoded.protocol == 4 && decoded.role == role && decoded.capabilities == UINT32_MAX);
            assert(decoded.replyTo == h.replyTo);
            assert(std::string(decoded.deviceId) == h.deviceId);
            assert(std::string(decoded.epoch) == h.epoch);
            assert(std::string(decoded.physicalId) == h.physicalId);
            assert(boardlink::encodeHello(decoded, reencoded, kind));
            assert(payload(m) == payload(reencoded));
        }
    }

    for (unsigned i = 0; i <= unsigned(display::DisplayStage::Unknown); ++i) {
        auto s = status();
        s.snapshot.stage = display::DisplayStage(i);
        assert(boardlink::encodeStatus(s, m) && boardlink::decodeStatus(m, copy));
        assert(copy.snapshot.stage == s.snapshot.stage);
    }
    for (unsigned i = 0; i <= unsigned(display::DisplayCondition::Ready); ++i) {
        auto s = status();
        s.snapshot.primaryCondition = s.snapshot.footerCondition = display::DisplayCondition(i);
        assert(boardlink::encodeStatus(s, m) && boardlink::decodeStatus(m, copy));
        assert(copy.snapshot.primaryCondition == s.snapshot.primaryCondition);
        assert(copy.snapshot.footerCondition == s.snapshot.footerCondition);
    }
    for (unsigned i = 0; i <= unsigned(display::DisplayError::PowderMotorFault); ++i) {
        auto s = status();
        s.snapshot.error = display::DisplayError(i);
        assert(boardlink::encodeStatus(s, m) && boardlink::decodeStatus(m, copy));
        assert(copy.snapshot.error == s.snapshot.error);
    }
}

void executionOwnership() {
    static_assert(std::is_same<std::underlying_type_t<ExecutionOwner>, uint8_t>::value,
                  "ExecutionOwner must retain its uint8_t API");
    const std::array<std::pair<ExecutionOwner, const char*>, 3> owners{{
        {ExecutionOwner::None, "none"}, {ExecutionOwner::Product, "product"},
        {ExecutionOwner::Workbench, "workbench"},
    }};
    const auto beforeRejected = rejected;
    size_t roundTrips = 0, old35 = 0;
    for (const auto& owner : owners) {
        auto input = status();
        input.executionOwner = owner.first;
        if (owner.first == ExecutionOwner::None) set(input.activeExecutionId, "");
        for (bool busy : {false, true}) for (bool stationary : {false, true}) {
            input.motionBusy = busy;
            input.stationary = stationary;
            const auto before = bytes(input);
            Message message;
            assert(boardlink::encodeStatus(input, message));
            assert(bytes(input) == before);
            StaticJsonDocument<8192> json;
            assert(!deserializeJson(json, static_cast<const uint8_t*>(message.payload), message.length));
            assert(json.size() == 36 && json["execution_owner"].is<JsonString>());
            assert(json["execution_owner"].as<std::string>() == owner.second);
            Status decoded = status();
            assert(boardlink::decodeStatus(message, decoded));
            assert(decoded.executionOwner == owner.first && decoded.motionBusy == busy && decoded.stationary == stationary);
            assert(std::string(decoded.activeExecutionId) == input.activeExecutionId);
            Message encoded;
            assert(boardlink::encodeStatus(decoded, encoded) && payload(message) == payload(encoded));
            ++roundTrips;
        }
        Message message;
        assert(boardlink::encodeStatus(input, message));
        const auto legacy = without(message, "execution_owner");
        StaticJsonDocument<8192> json;
        assert(!deserializeJson(json, static_cast<const uint8_t*>(legacy.payload), legacy.length));
        assert(json.size() == 35 && !json.containsKey("execution_owner"));
        reject(legacy, false); // No inference or compatibility default for old v4.
        ++old35;

        // Duplicate owner keys cannot conceal a missing field, even when an
        // escaped key spelling keeps the lexical count at exactly 36.
        auto duplicate = payload(without(message, "pending_event_id"));
        duplicate.insert(1, "\"execution_\\u006fwner\":\"none\",");
        reject(raw(message, duplicate), false);
        duplicate = payload(message);
        duplicate.insert(1, "\"execution_owner\":\"none\",");
        reject(raw(message, duplicate), false);
        duplicate = payload(message);
        duplicate.insert(duplicate.size() - 1, ",\"execution_owner\":\"workbench\"");
        reject(raw(message, duplicate), false);
    }
    const auto message = statusMessage();
    for (const auto* token : {"null", "true", "false", "0", "1", "2", "1.0", "1e0", "[]", "{}",
                             "\"\"", "\"None\"", "\"Product\"", "\"Workbench\"", "\"idle\"", "\"unknown\"",
                             "\"none \"", "\" product\"", "\"workbenchx\"", "\"product\\u0000\"",
                             "\"workbench\\n\"", "\"pr\\u043educt\"", "\"\\ud800\""})
        reject(field(message, "execution_owner", token), false);

    for (const auto& owner : owners) {
        for (const std::string& id : {std::string(""), std::string("abcdef1234567890abcdef1234567890"),
                                     std::string(32, '0'), std::string(32, 'f'), std::string(31, '0') + "1",
                                     std::string(31, 'a'), std::string(32, 'A'), std::string(31, 'a') + "g"}) {
            auto input = status();
            input.executionOwner = owner.first;
            set(input.activeExecutionId, id);
            const auto wire = field(field(message, "execution_owner", std::string("\"") + owner.second + "\""),
                                    "active_execution_id", "\"" + id + "\"");
            const bool validId = id == "abcdef1234567890abcdef1234567890" ||
                id == std::string(32, 'f') || id == std::string(31, '0') + "1";
            if (owner.first == ExecutionOwner::None ? id.empty() : validId) {
                Message encoded;
                Status decoded;
                assert(boardlink::encodeStatus(input, encoded) && boardlink::decodeStatus(wire, decoded));
                assert(decoded.executionOwner == owner.first && std::string(decoded.activeExecutionId) == id);
                assert(payload(encoded) == payload(wire));
                ++roundTrips;
            } else {
                rejectEncode(input);
                reject(wire, false);
            }
        }
        auto input = status();
        input.executionOwner = owner.first;
        std::memset(input.activeExecutionId, 'a', sizeof(input.activeExecutionId));
        rejectEncode(input);
        reject(field(field(message, "execution_owner", std::string("\"") + owner.second + "\""),
                     "active_execution_id", "\"" + std::string(33, 'a') + "\""), false);
    }
    for (unsigned value : {3u, 127u, 255u}) for (bool withId : {false, true}) {
        auto input = status();
        input.executionOwner = ExecutionOwner(value);
        if (!withId) set(input.activeExecutionId, "");
        rejectEncode(input);
    }
    // JSON escaping may represent ASCII enum characters; output is canonical.
    for (const auto& owner : owners) {
        auto wire = field(message, "execution_owner", std::string("\"\\u00") +
                          (owner.first == ExecutionOwner::None ? "6eone" :
                           owner.first == ExecutionOwner::Product ? "70roduct" : "77orkbench") + "\"");
        if (owner.first == ExecutionOwner::None) wire = field(wire, "active_execution_id", "\"\"");
        Status decoded;
        assert(boardlink::decodeStatus(wire, decoded) && decoded.executionOwner == owner.first);
        Message encoded;
        assert(boardlink::encodeStatus(decoded, encoded));
        assert(payload(encoded).find(std::string("\"execution_owner\":\"") + owner.second + "\"") != std::string::npos);
        ++roundTrips;
    }
    std::cout << "Execution owner: " << roundTrips << " valid cases, " << old35 << " old-35 regressions, "
              << rejected - beforeRejected << " atomic rejection cases passed\n";
}

void invalidFields() {
    const Message h = helloMessage(), s = statusMessage();
    for (const auto* key : {"protocol", "role", "capabilities", "reply_to", "device_id", "pairing_epoch", "physical_id"}) {
        reject(without(h, key), true);
        reject(field(h, key, "null"), true);
        reject(field(h, key, "true"), true);
        reject(field(h, key, "{}"), true);
        reject(field(h, key, "[]"), true);
    }
    for (const auto* key : {"schema_version", "stage", "primary_condition", "footer_condition", "error",
                           "cloud_connected", "start_enabled", "thermal_simulated", "water_ml", "temperature_c",
                           "baby_name", "formula_brand", "sample_uptime_ms", "context_version", "cloud_watermark",
                           "local_watermark", "motion_busy", "stationary", "event_pending",
                           "execution_owner", "active_execution_id", "pending_event_id", "product_progress", "product_error",
                           "is_preparing", "low_water_valid", "low_water", "powder_valid", "powder_grams",
                           "actuator_operational", "actuator_config_valid", "actuator_bus_healthy",
                           "actuator_position_referenced", "execution_authorized", "feeding_context_configured",
                           "baby_id"}) {
        reject(without(s, key), false);
        reject(field(s, key, "null"), false);
        reject(field(s, key, "[]"), false);
        reject(field(s, key, "{}"), false);
        reject(field(without(s, key), "unknown_field", "false"), false);
        // Keep the lexical count at 36: neither literal nor escaped duplicate
        // keys may hide a missing required key after ArduinoJson decoding.
        const std::string missing = payload(without(s, key));
        const bool stageMissing = std::strcmp(key, "stage") == 0;
        reject(raw(s, std::string(stageMissing ? "{\"error\":15," : "{\"stage\":10,") + missing.substr(1)), false);
        reject(raw(s, std::string(stageMissing ? "{\"err\\u006fr\":15," : "{\"st\\u0061ge\":10,") + missing.substr(1)), false);
    }
    for (const auto* key : {"schema_version", "stage", "primary_condition", "footer_condition", "error",
                           "water_ml", "temperature_c", "sample_uptime_ms", "context_version", "powder_grams"}) {
        for (const auto* value : {"true", "false", "\"1\"", "1.0", "1.5", "1e0", "1e999", "NaN",
                                 "Infinity", "-Infinity", "18446744073709551616", "-9223372036854775809"})
            reject(field(s, key, value), false);
    }
    for (const auto* key : {"protocol", "capabilities", "reply_to"}) {
        for (const auto* value : {"false", "\"4\"", "4.0", "4e0", "-1", "4294967296", "1e999"})
            reject(field(h, key, value), true);
    }
    for (const auto* key : {"cloud_connected", "start_enabled", "thermal_simulated", "motion_busy", "stationary", "event_pending"})
        for (const auto* value : {"0", "1", "\"true\"", "\"false\""})
            reject(field(s, key, value), false);
    for (const auto* key : {"baby_name", "formula_brand", "cloud_watermark", "local_watermark", "active_execution_id", "pending_event_id"})
        for (const auto* value : {"false", "true", "0", "1"}) reject(field(s, key, value), false);
    for (const auto& fieldInfo : kProductBools)
        for (const auto* value : {"0", "1", "-1", "\"true\"", "\"false\"", "1.0", "1e0"})
            reject(field(s, fieldInfo.key, value), false);
    for (const auto* key : kProductStrings)
        for (const auto* value : {"false", "true", "0", "1"}) reject(field(s, key, value), false);

    reject(field(h, "protocol", "3"), true);
    reject(field(h, "protocol", "260"), true);
    for (const auto* value : {"0", "2", "4294967294"}) reject(field(h, "capabilities", value), true);
    for (const auto* value : {"\"\"", "\"Brain\"", "\"display\"", "\"motion\\u0000\""}) reject(field(h, "role", value), true);
    for (const auto* value : {"\"\"", "\"_unit\"", "\"unit/1\"", "\"unit 1\"", "\"unit\\u0000suffix\""})
        reject(field(h, "device_id", value), true);
    reject(field(h, "device_id", "\"" + std::string(65, 'a') + "\""), true);
    for (const auto* key : {"pairing_epoch", "physical_id"}) {
        for (const auto* value : {"\"\"", "\"0123\"", "\"ABCDEF123456\"", "\"000000000000\"",
                                 "\"00000000000000000000000000000000\"", "\"ABCDEF1234567890abcdef1234567890\""})
            reject(field(h, key, value), true);
    }
    for (const auto* key : {"cloud_watermark", "local_watermark"}) {
        for (const auto* value : {"\"\"", "\"00\"", "\"01\"", "\"-1\"", "\"+1\"", "\" 1\"", "\"1 \"",
                                 "\"1e3\"", "\"1.0\"", "\"9223372036854775808\"", "\"18446744073709551615\"",
                                 "\"1\\u00002\"", "\"\\u0661\""}) reject(field(s, key, value), false);
    }
    for (const auto* value : {"\"0\"", "\"00000000000000000000000000000000\"", "\"ABCDEF1234567890abcdef1234567890\"",
                             "\"abcdef1234567890abcdef123456789z\""}) reject(field(s, "active_execution_id", value), false);
    reject(field(s, "schema_version", "4"), false);
    reject(field(s, "stage", "13"), false);
    reject(field(s, "primary_condition", "14"), false);
    reject(field(s, "footer_condition", "14"), false);
    reject(field(s, "error", "16"), false);
    for (const auto* key : {"stage", "water_ml", "context_version", "sample_uptime_ms"}) reject(field(s, key, "-1"), false);
    reject(field(s, "water_ml", "65536"), false);
    reject(field(s, "temperature_c", "32768"), false);
    reject(field(s, "temperature_c", "-32769"), false);
    reject(field(s, "sample_uptime_ms", "4294967296"), false);
    reject(field(s, "context_version", "2147483648"), false);
    for (const auto* value : {"-1", "-2147483648", "2147483648", "4294967295"})
        reject(field(s, "powder_grams", value), false);
    reject(field(s, "pending_event_id", "\"" + std::string(129, 'a') + "\""), false);
    for (const auto* key : {"baby_name", "formula_brand"})
        reject(field(s, key, "\"" + std::string(32, 'a') + "\""), false);
}

void productTelemetry() {
    Message message;
    Status copy;
    for (const auto* progress : {"ready", "noready", "error", "cleaning", "unscrewing_cap",
                                "dispensing_water", "dispensing_powder", "screwing_cap", "mixing", "complete"}) {
        auto s = status();
        set(s.productProgress, progress);
        const auto before = bytes(s);
        assert(boardlink::encodeStatus(s, message) && boardlink::decodeStatus(message, copy));
        assert(bytes(s) == before && std::string(copy.productProgress) == progress);
    }
    const Message original = statusMessage();
    for (const std::string& progress : {std::string(""), std::string("Ready"), std::string("idle"),
                                      std::string("unknown"), std::string("ready "), std::string(" ready"),
                                      std::string("dispensing"), std::string(23, 'x')}) {
        auto s = status();
        set(s.productProgress, progress);
        rejectEncode(s);
        reject(field(original, "product_progress", "\"" + progress + "\""), false);
    }
    for (const std::string& error : {std::string("NONE"), std::string("E_CAP_UNSCREW_TIMEOUT"),
                                   std::string("E_WATER_DISPENSE_TIMEOUT"), std::string("E_POWDER_DISPENSE_TIMEOUT"),
                                   std::string("E_CAP_SCREW_TIMEOUT"), std::string("E_MIXING_TIMEOUT"),
                                   std::string("E_CAN_FAULT"), std::string("E_MOTION_FAULT"),
                                   std::string("E_CONTEXT_STORAGE_2"), std::string("0"), std::string("_"),
                                   "E_" + std::string(37, 'Z')}) {
        auto s = status();
        set(s.productError, error);
        assert(boardlink::encodeStatus(s, message) && boardlink::decodeStatus(message, copy));
        assert(std::string(copy.productError) == error);
    }
    for (const auto* error : {"", "none", "E_Fault", "E-FAULT", "E.FAULT", "E/FAULT", "E:FAULT",
                              " E_FAULT", "E_FAULT ", "E_\xc3\x89", "E_\x7f"}) {
        auto s = status();
        set(s.productError, error);
        rejectEncode(s);
        reject(field(original, "product_error", std::string("\"") + error + "\""), false);
    }
    for (const auto* token : {"\"E_\\nFAULT\"", "\"E_\\tFAULT\"", "\"E_\\u0001\""})
        reject(field(original, "product_error", token), false);
    assert(boardlink::decodeStatus(field(original, "product_error", "\"\\u0045_2\\u005fOK\""), copy));
    assert(std::string(copy.productError) == "E_2_OK");

    // Isolate each flag in both directions to detect swapped or omitted fields.
    for (bool value : {false, true}) {
        for (const auto& selected : kProductBools) {
            Status s;
            for (const auto& other : kProductBools) s.*other.member = !value;
            s.*selected.member = value;
            assert(boardlink::encodeStatus(s, message) && boardlink::decodeStatus(message, copy));
            StaticJsonDocument<8192> doc;
            assert(!deserializeJson(doc, static_cast<const uint8_t*>(message.payload), message.length));
            for (const auto& other : kProductBools) {
                assert(copy.*other.member == s.*other.member);
                assert(doc[other.key].is<bool>() && doc[other.key].as<bool>() == s.*other.member);
            }
        }
    }
    for (int32_t grams : {int32_t(0), int32_t(1), int32_t(INT32_MAX)}) {
        auto s = status();
        s.powderGrams = grams;
        assert(boardlink::encodeStatus(s, message) && boardlink::decodeStatus(message, copy));
        assert(copy.powderGrams == grams);
    }
    for (int32_t grams : {int32_t(-1), int32_t(INT32_MIN)}) {
        auto s = status();
        s.powderGrams = grams;
        rejectEncode(s);
    }
    for (size_t length : {0u, 1u, 31u, 32u, 95u, 96u}) {
        auto s = status();
        set(s.babyId, std::string(length, 'b'));
        assert(boardlink::encodeStatus(s, message) && boardlink::decodeStatus(message, copy));
        assert(std::string(copy.babyId) == s.babyId);
        assert(copy.snapshot.babyName == s.snapshot.babyName);
    }
    for (const std::string& codepoint : {std::string("\xe5\xae\x9d"), std::string("\xf0\x9f\x98\x80")}) {
        auto s = status();
        std::string id;
        while (id.size() < 96) id += codepoint;
        set(s.babyId, id);
        assert(boardlink::encodeStatus(s, message) && boardlink::decodeStatus(message, copy));
        assert(std::string(copy.babyId) == id);
        reject(field(original, "baby_id", "\"" + id + codepoint + "\""), false);
    }
    std::string escapedId;
    for (size_t i = 0; i < 24; ++i) escapedId += "\\ud83d\\ude00";
    assert(boardlink::decodeStatus(field(original, "baby_id", "\"" + escapedId + "\""), copy));
    assert(std::strlen(copy.babyId) == 96);
    reject(field(original, "baby_id", "\"" + escapedId + "a\""), false);

    auto rejectText = [&](auto member, const char* key) {
        auto s = status();
        auto& text = s.*member;
        const size_t capacity = sizeof(text);
        std::memset(text, 'X', capacity);
        rejectEncode(s);
        reject(field(original, key, "\"" + std::string(capacity, 'X') + "\""), false);
        for (const auto* invalid : {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80",
                                   "\xe5\xae", "\x80", "\xe5\x41\x41"}) {
            set(text, invalid);
            rejectEncode(s);
        }
        std::string duplicate = payload(original);
        duplicate.insert(1, std::string("\"") + key + "\":null,");
        reject(raw(original, duplicate), false);
        duplicate = payload(original);
        duplicate.insert(duplicate.size() - 1, std::string(",\"") + key + "\":null");
        reject(raw(original, duplicate), false);
    };
    rejectText(&Status::productProgress, "product_progress");
    rejectText(&Status::productError, "product_error");
    rejectText(&Status::babyId, "baby_id");

    Message legacy = original;
    legacy = without(legacy, "execution_owner");
    for (const auto& fieldInfo : kProductBools) legacy = without(legacy, fieldInfo.key);
    for (const auto* key : kProductStrings) legacy = without(legacy, key);
    legacy = without(legacy, "powder_grams");
    reject(legacy, false);  // The unreleased 21-field STATUS is no longer accepted.
}

void helloCorrelation() {
    for (auto kind : {v4::Kind::Hello, v4::Kind::HelloAck}) {
        auto h = hello();
        h.replyTo = kind == v4::Kind::HelloAck ? 1 : 0;
        Message message;
        assert(boardlink::encodeHello(h, message, kind));
        v4::Hello decoded;
        assert(boardlink::decodeHello(message, decoded));
        assert(decoded.replyTo == h.replyTo);
        StaticJsonDocument<2048> doc;
        assert(!deserializeJson(doc, static_cast<const uint8_t*>(message.payload), message.length));
        assert(doc.size() == 7 && doc["reply_to"].is<uint32_t>());
        assert(doc["reply_to"].as<uint32_t>() == h.replyTo);

        reject(without(message, "reply_to"), true);
        for (const auto* value : {"null", "true", "false", "[]", "{}", "\"0\"", "\"1\"", "1.0",
                                 "1.5", "1e0", "1e999", "NaN", "Infinity", "-1", "4294967296",
                                 "18446744073709551616"}) reject(field(message, "reply_to", value), true);
        reject(field(message, "reply_to", kind == v4::Kind::Hello ? "1" : "0"), true);
        auto wrongKind = message;
        wrongKind.kind = kind == v4::Kind::Hello ? v4::Kind::HelloAck : v4::Kind::Hello;
        reject(wrongKind, true);
        for (const auto* value : {"0", "1", "null"}) {
            std::string duplicate = payload(message);
            duplicate.insert(1, std::string("\"reply_to\":") + value + ",");
            reject(raw(message, duplicate), true);
            duplicate = payload(message);
            duplicate.insert(duplicate.size() - 1, std::string(",\"reply_to\":") + value);
            reject(raw(message, duplicate), true);
        }
        // Seven textual fields must not conceal a duplicate after JSON key decoding.
        std::string duplicate = payload(without(message, "capabilities"));
        duplicate.insert(1, "\"reply_\\u0074o\":1,");
        reject(raw(message, duplicate), true);

        h.replyTo = kind == v4::Kind::Hello ? 1 : 0;
        rejectEncode(h, kind);
    }
    auto h = hello();
    h.replyTo = UINT32_MAX;
    rejectEncode(h);
    reject(field(helloMessage(), "reply_to", "4294967295"), true);
    Message ack;
    assert(boardlink::encodeHello(h, ack, v4::Kind::HelloAck));
    v4::Hello decoded;
    assert(boardlink::decodeHello(ack, decoded) && decoded.replyTo == UINT32_MAX);
}

void malformedJson() {
    for (bool isHello : {false, true}) {
        const Message m = isHello ? helloMessage() : statusMessage();
        for (const auto* text : {"", "null", "true", "[]", "1", "\"x\"", "{}", "{", "{\"x\":1,}"})
            reject(raw(m, text), isHello);
        reject(raw(m, payload(m) + "{}"), isHello);
        reject(raw(m, payload(m) + std::string(1, '\0')), isHello);
        reject(raw(m, payload(m).substr(0, m.length - 1)), isHello);
        reject(field(m, "unexpected", "1"), isHello);
        const auto* key = isHello ? "protocol" : "stage";
        std::string duplicate = payload(m);
        duplicate.insert(1, std::string("\"") + key + "\":null,");
        reject(raw(m, duplicate), isHello);
        duplicate = payload(m);
        duplicate.insert(duplicate.size() - 1, std::string(",\"") + key + "\":null");
        reject(raw(m, duplicate), isHello);
        const auto* value = isHello ? "4" : "10";
        for (const auto* bad : {"+1", "01", ".1", "1.", "-", "--1"}) reject(field(m, key, bad), isHello);
        std::string unquoted = payload(m);
        unquoted.replace(1, std::string(key).size() + 2, key);
        reject(raw(m, unquoted), isHello);
        reject(field(m, key, std::string("'") + value + "'"), isHello);
        auto wrongKind = m;
        wrongKind.kind = v4::Kind::Command;
        reject(wrongKind, isHello);
        wrongKind.kind = isHello ? v4::Kind::Status : v4::Kind::Hello;
        reject(wrongKind, isHello);
        auto oversized = m;
        oversized.length = v4::kMaxMessage + 1;
        reject(oversized, isHello);
        oversized.length = UINT16_MAX;
        reject(oversized, isHello);
        const auto padded = raw(m, payload(m) + std::string(v4::kMaxMessage - m.length, ' '));
        Status s;
        v4::Hello h;
        assert(isHello ? boardlink::decodeHello(padded, h) : boardlink::decodeStatus(padded, s));
    }
    const Message s = statusMessage();
    for (const auto* key : {"baby_name", "formula_brand", "pending_event_id", "baby_id", "product_progress", "product_error",
                           "execution_owner"}) {
        for (const std::string& value : {std::string("\xc0\xaf"), std::string("\xed\xa0\x80"),
                                        std::string("\xf4\x90\x80\x80"), std::string("\xe5\xae"),
                                        std::string("\x80"), std::string("\xe5\x41\x41")})
            reject(field(s, key, "\"" + value + "\""), false);
        for (const auto* value : {"\"\\u0000\"", "\"x\\u0000y\"", "\"\\ud800\"", "\"\\udc00\"",
                                 "\"\\ud800x\"", "\"\\ud800\\u0041\"", "\"\\ud800\\ud800\"",
                                 "\"\\u12xz\"", "\"\\q\"", "\"raw\nnewline\""}) reject(field(s, key, value), false);
    }
    Status decoded;
    assert(boardlink::decodeStatus(field(s, "pending_event_id", "\"\\ud83d\\ude00\""), decoded));
    assert(std::string(decoded.pendingEventId) == "\xf0\x9f\x98\x80");
}

void encoderValidation() {
    auto h = hello();
    h.protocol = 3; rejectEncode(h);
    h = hello(); h.role = v4::Role(0); rejectEncode(h);
    h = hello(); h.capabilities = 2; rejectEncode(h);
    h = hello(); std::memset(h.deviceId, 'x', sizeof(h.deviceId)); rejectEncode(h);
    h = hello(); std::memset(h.epoch, 'a', sizeof(h.epoch)); rejectEncode(h);
    h = hello(); std::memset(h.physicalId, 'a', sizeof(h.physicalId)); rejectEncode(h);
    Message m = statusMessage();
    const auto before = bytes(m);
    assert(!boardlink::encodeHello(hello(), m, v4::Kind::Command));
    assert(bytes(m) == before);
    auto s = status();
    s.contextVersion = UINT32_MAX; rejectEncode(s);
    s = status(); s.snapshot.schemaVersion = 0; rejectEncode(s);
    s = status(); s.snapshot.stage = display::DisplayStage(255); rejectEncode(s);
    s = status(); s.snapshot.primaryCondition = display::DisplayCondition(255); rejectEncode(s);
    s = status(); s.snapshot.footerCondition = display::DisplayCondition(255); rejectEncode(s);
    s = status(); s.snapshot.error = display::DisplayError(255); rejectEncode(s);
    s = status(); s.snapshot.babyName.fill('a'); rejectEncode(s);
    s = status(); s.snapshot.formulaBrand.fill('a'); rejectEncode(s);
    s = status(); std::memset(s.cloudWatermark, '1', sizeof(s.cloudWatermark)); rejectEncode(s);
    s = status(); set(s.localWatermark, "9223372036854775808"); rejectEncode(s);
    s = status(); set(s.cloudWatermark, "01"); rejectEncode(s);
    s = status(); set(s.activeExecutionId, std::string(32, '0')); rejectEncode(s);
    s = status(); std::memset(s.activeExecutionId, 'a', sizeof(s.activeExecutionId)); rejectEncode(s);
    s = status(); std::memset(s.pendingEventId, 'a', sizeof(s.pendingEventId)); rejectEncode(s);
    s = status(); set(s.pendingEventId, "\xe5\xae"); rejectEncode(s);
    s = status(); s.snapshot.babyName = {}; s.snapshot.babyName[0] = char(0x80); rejectEncode(s);

    s = status();
    s.snapshot.babyName.fill('\x01'); s.snapshot.babyName.back() = 0;
    s.snapshot.formulaBrand.fill('\x02'); s.snapshot.formulaBrand.back() = 0;
    set(s.pendingEventId, std::string(128, '\x03'));
    set(s.localWatermark, "9223372036854775806");
    // This formerly valid 21-field fixture exceeds the expanded wire budget.
    // Reject the whole message; do not omit telemetry to squeeze it in.
    rejectEncode(s);
    s.snapshot.babyName.fill('n'); s.snapshot.babyName.back() = 0;
    s.snapshot.formulaBrand.fill('f'); s.snapshot.formulaBrand.back() = 0;
    set(s.pendingEventId, std::string(128, 'p'));
    set(s.productError, "E_" + std::string(37, 'Z'));
    // Every string is at its byte limit, with distinct contents to avoid dedup.
    // A slots-only encoding document cannot succeed if it copies any strings.
    const auto inputBytes = bytes(s);
    assert(boardlink::encodeStatus(s, m) && m.length <= v4::kMaxMessage && m.length > 1000);
    assert(bytes(s) == inputBytes);
    Status copy;
    assert(boardlink::decodeStatus(m, copy));
    assert(copy.snapshot.babyName == s.snapshot.babyName);
    assert(copy.snapshot.formulaBrand == s.snapshot.formulaBrand);
    assert(std::string(copy.pendingEventId) == s.pendingEventId);
    assert(std::string(copy.cloudWatermark) == s.cloudWatermark);
    assert(std::string(copy.localWatermark) == s.localWatermark);
    assert(std::string(copy.activeExecutionId) == s.activeExecutionId);
    s = Status{};
    s.snapshot.temperatureC = INT16_MAX;
    assert(boardlink::encodeStatus(s, m) && boardlink::decodeStatus(m, copy));
    assert(copy.snapshot.temperatureC == INT16_MAX);
}

void directSerialization() {
    auto input = status();
    std::string controls;
    for (unsigned c = 1; c < 32; ++c) controls += char(c);
    set(input.pendingEventId, controls + controls + controls + controls);
    const size_t nameLength = input.snapshot.babyName.size() - 1;
    std::memcpy(input.snapshot.babyName.data(), controls.data(), nameLength);
    input.snapshot.babyName.back() = 0;
    Message output = helloMessage();
    output.senderBoot = 111;
    output.receiverBoot = 222;
    output.messageId = 333;
    std::memset(output.payload, 0xa5, sizeof(output.payload));
    assert(boardlink::encodeStatus(input, output));
    assert(output.kind == v4::Kind::Status && !output.senderBoot && !output.receiverBoot && !output.messageId);
    assert(payload(output).find("\\u0001") != std::string::npos);
    for (size_t i = 0; i < output.length; ++i) assert(output.payload[i] >= 0x20);
    for (size_t i = output.length; i < sizeof(output.payload); ++i) assert(output.payload[i] == 0);
    Status decoded;
    assert(boardlink::decodeStatus(output, decoded));
    assert(decoded.snapshot.babyName == input.snapshot.babyName);
    assert(std::string(decoded.pendingEventId) == input.pendingEventId);
    const auto before = bytes(output);
    input.snapshot.stage = display::DisplayStage(255);
    assert(!boardlink::encodeStatus(input, output) && bytes(output) == before);
    ++rejected;
}

Message transport(Message message, bool& splitUtf8) {
    message.senderBoot = 11;
    message.receiverBoot = 22;
    message.messageId = 33;
    v4::Assembler assembler;
    v4::Parser parser;
    Message assembled;
    size_t frames = 0;
    for (size_t offset = 0; offset < message.length;) {
        v4::Frame frame;
        assert(v4::fragment(message, offset, frame));
        if (offset && (message.payload[offset] & 0xc0) == 0x80) splitUtf8 = true;
        uint8_t wire[v4::kMaxFrame];
        const size_t size = v4::encode(frame, wire, sizeof(wire));
        assert(size);
        v4::Frame parsed;
        for (size_t i = 0; i < size; ++i)
            assert(parser.push(wire[i], 100, parsed) == (i + 1 == size));
        offset += frame.length;
        assert(assembler.accept(parsed, 100, assembled) ==
               (offset == message.length ? v4::AssemblyResult::Complete : v4::AssemblyResult::Incomplete));
        ++frames;
    }
    assert(frames > 1 && payload(message) == payload(assembled));
    assert(assembled.senderBoot == 11 && assembled.receiverBoot == 22 && assembled.messageId == 33);
    assert(assembled.kind == message.kind);
    return assembled;
}

void payloadBudget() {
    static_assert(v4::kMaxMessage == 2047, "STATUS must retain the v4 wire cap");
    auto s = status();
    s.executionOwner = ExecutionOwner::Workbench; // Longest owner spelling.
    s.snapshot.stage = display::DisplayStage::Unknown;
    s.snapshot.primaryCondition = s.snapshot.footerCondition = display::DisplayCondition::Ready;
    s.snapshot.cloudConnected = s.snapshot.startEnabled = s.snapshot.thermalSimulated = false;
    s.motionBusy = s.stationary = s.eventPending = false;
    for (const auto& fieldInfo : kProductBools) s.*fieldInfo.member = false;
    s.snapshot.babyName.fill('n'); s.snapshot.babyName.back() = 0;
    s.snapshot.formulaBrand.fill('f'); s.snapshot.formulaBrand.back() = 0;
    set(s.pendingEventId, std::string(128, 'p'));
    set(s.babyId, std::string(96, 'b'));
    set(s.productError, "E_" + std::string(37, 'Z'));
    set(s.localWatermark, "9223372036854775806");
    Message message;
    assert(boardlink::encodeStatus(s, message));
    const size_t unescapedMaximum = message.length;
    // Each byte in these four UTF-8 strings may require a six-byte JSON escape.
    const size_t escapedMaximum = unescapedMaximum + 5 * (31 + 31 + 128 + 96);
    assert(unescapedMaximum == 1237 && escapedMaximum == 2667);
    const auto allAscii = s;
    const std::array<std::pair<char*, size_t>, 4> strings{{
        {s.snapshot.babyName.data(), 31}, {s.snapshot.formulaBrand.data(), 31},
        {s.pendingEventId, 128}, {s.babyId, 96},
    }};
    size_t remaining = v4::kMaxMessage - unescapedMaximum;
    char* nextAscii = nullptr;
    for (const auto& text : strings) {
        for (size_t i = 0; i < text.second; ++i) {
            if (remaining >= 5) { text.first[i] = '\x01'; remaining -= 5; }
            else if (remaining) { text.first[i] = '"'; --remaining; }
            else nextAscii = &text.first[i];
        }
    }
    assert(!remaining && nextAscii);
    const auto before = bytes(s);
    assert(boardlink::encodeStatus(s, message) && message.length == v4::kMaxMessage);
    assert(bytes(s) == before);
    Status copy;
    bool splitUtf8 = false;
    assert(boardlink::decodeStatus(transport(message, splitUtf8), copy));
    assert(copy.snapshot.babyName == s.snapshot.babyName && copy.snapshot.formulaBrand == s.snapshot.formulaBrand);
    assert(std::string(copy.pendingEventId) == s.pendingEventId && std::string(copy.babyId) == s.babyId);
    assert(std::string(copy.productProgress) == s.productProgress && std::string(copy.productError) == s.productError);
    assert(copy.powderGrams == INT32_MAX);
    Message reencoded;
    assert(boardlink::encodeStatus(copy, reencoded) && payload(reencoded) == payload(message));
    StaticJsonDocument<8192> doc;
    assert(!deserializeJson(doc, static_cast<const uint8_t*>(message.payload), message.length));
    assert(doc.size() == 36);
    // +1 escaped byte crosses the limit even though every field still fits.
    *nextAscii = '"';
    rejectEncode(s);
    for (const auto& text : strings) std::memset(text.first, '\x01', text.second);
    rejectEncode(s);
    assert(escapedMaximum > v4::kMaxMessage);

    // Over-budget input fails before payload access; the declared length is
    // intentionally larger than Message::payload's physical capacity.
    message.length = v4::kMaxMessage + 1;
    reject(message, false);
    Message defaults;
    assert(boardlink::encodeStatus(Status{}, defaults));
    assert(boardlink::encodeStatus(allAscii, message));
    std::cout << "STATUS budget: fields=36, default_payload=" << defaults.length
              << ", max_unescaped_payload=" << unescapedMaximum
              << ", theoretical_max_escaped_payload=" << escapedMaximum
              << ", max_accepted_payload=" << v4::kMaxMessage
              << ", sizeof(Status)=" << sizeof(Status)
              << ", encode_pool=" << JSON_OBJECT_SIZE(36)
              << ", decode_pool=" << JSON_OBJECT_SIZE(36) + v4::kMaxMessage + 1 << '\n';
}

void fragmentation() {
    bool splitUtf8 = false;
    v4::Hello h;
    auto sourceHello = hello();
    set(sourceHello.deviceId, std::string(64, 'x'));
    Message helloWire;
    assert(boardlink::encodeHello(sourceHello, helloWire));
    assert(boardlink::decodeHello(transport(helloWire, splitUtf8), h));
    sourceHello.replyTo = UINT32_MAX;
    assert(boardlink::encodeHello(sourceHello, helloWire, v4::Kind::HelloAck));
    assert(boardlink::decodeHello(transport(helloWire, splitUtf8), h));
    assert(h.replyTo == UINT32_MAX);
    Status original = status();
    std::string id;
    for (size_t i = 0; i < 42; ++i) id += u8"事";
    id += "ab";
    set(original.pendingEventId, id);
    Message message;
    assert(boardlink::encodeStatus(original, message));
    Status copy;
    assert(boardlink::decodeStatus(transport(message, splitUtf8), copy));
    assert(std::string(copy.pendingEventId) == id);
    // Legal leading whitespace exercises every alignment of a 160-byte frame.
    for (size_t padding = 1; padding < v4::kMaxFragment && !splitUtf8; ++padding) {
        const Message shifted = raw(message, std::string(padding, ' ') + payload(message));
        assert(boardlink::decodeStatus(transport(shifted, splitUtf8), copy));
        assert(std::string(copy.pendingEventId) == id);
    }
    assert(splitUtf8);
}
}  // namespace

int main() {
    roundTrip();
    executionOwnership();
    invalidFields();
    productTelemetry();
    helloCorrelation();
    malformedJson();
    encoderValidation();
    directSerialization();
    payloadBudget();
    fragmentation();
    std::cout << "Board message codec: round trips, boundaries, fragmented UTF-8, and "
              << rejected << " atomic rejection cases passed (ArduinoJson "
              << ARDUINOJSON_VERSION << ")\n";
}
