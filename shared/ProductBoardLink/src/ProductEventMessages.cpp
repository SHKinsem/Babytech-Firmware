#include "ProductEventMessages.h"
#include "BoundedJsonWriter.h"
#include "FlatJsonGuard.h"

#include <ArduinoJson.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

static_assert(ARDUINOJSON_VERSION_MAJOR == 6, "Product events require ArduinoJson 6");
static_assert(uint8_t(babytech::v4::Kind::Terminal) == 12, "TERMINAL kind");
static_assert(uint8_t(babytech::v4::Kind::CloudReceipt) == 13, "CLOUD_RECEIPT kind");

namespace babytech { namespace boardlink {
namespace {
constexpr size_t kTerminalFields = 17;
constexpr size_t kDocumentSize = JSON_OBJECT_SIZE(kTerminalFields) + v4::kMaxMessage + 1;

template<size_t N> bool text(const char (&value)[N], bool nonempty = true) {
    size_t length = 0;
    while (length < N && value[length]) ++length;
    return length < N && (!nonempty || length) &&
        v4::validUtf8(reinterpret_cast<const uint8_t*>(value), length);
}

template<size_t N> bool readText(JsonVariantConst value, char (&output)[N]) {
    if (!value.is<JsonString>()) return false;
    const auto string = value.as<JsonString>();
    if (string.size() >= N || !v4::validUtf8(
        reinterpret_cast<const uint8_t*>(string.c_str()), string.size())) return false;
    std::memcpy(output, string.c_str(), string.size());
    output[string.size()] = 0;
    return true;
}

bool sequence(const char* value, uint64_t& output) {
    if (!value || *value < '1' || *value > '9') return false;
    uint64_t next = 0;
    for (const char* at = value; *at; ++at) {
        if (*at < '0' || *at > '9') return false;
        const uint8_t digit = uint8_t(*at - '0');
        if (next > (v4::kMaxSequence - digit) / 10) return false;
        next = next * 10 + digit;
    }
    output = next;
    return true;
}

bool eventIdentity(const char (&id)[59], v4::Source& source, uint64_t& seq) {
    if (!text(id) || std::strlen(id) < 40 || std::memcmp(id, "evt-", 4) ||
        id[36] != '-' || id[38] != '-') return false;
    bool nonzero = false;
    for (size_t i = 4; i < 36; ++i) {
        const char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        nonzero = nonzero || c != '0';
    }
    if (!nonzero || (id[37] != 'c' && id[37] != 'l') || !sequence(id + 39, seq)) return false;
    source = id[37] == 'c' ? v4::Source::CloudCommand : v4::Source::LocalTouch;
    return true;
}

bool code(const char (&value)[65], bool required) {
    if (!text(value, required)) return false;
    for (const char* at = value; *at; ++at)
        if (!((*at >= 'A' && *at <= 'Z') || (*at >= 'a' && *at <= 'z') ||
              (*at >= '0' && *at <= '9') || *at == '_')) return false;
    return true;
}

bool validEvent(const v4::Pairing& pairing, const TerminalEvent& event) {
    const auto& request = event.request;
    if (!v4::validPairing(pairing) || !validProductRequest(request) ||
        request.command != ProductCommand::Prepare ||
        std::strcmp(request.deviceId, pairing.deviceId) || !text(event.eventId) ||
        !std::isfinite(event.targetPowderG) || event.targetPowderG != productTargetPowderG(request) ||
        !code(event.reason, !event.completed) || !code(event.errorCode, false) ||
        (event.completed && (event.reason[0] || event.errorCode[0]))) return false;
    char expectedEvent[59]{};
    if (!makeProductEventId(pairing, request.source, request.sequence, expectedEvent) ||
        std::strcmp(expectedEvent, event.eventId)) return false;
    if (request.source == v4::Source::LocalTouch) {
        char expectedCommand[129]{};
        auto brainPairing = pairing;
        brainPairing.role = v4::Role::Brain;
        if (!makeLocalCommandId(brainPairing, request.sequence, expectedCommand) ||
            std::strcmp(expectedCommand, request.commandId)) return false;
    }
    return true;
}

bool validReceipt(const CloudReceipt& receipt) {
    constexpr char command[129] = "receipt";
    v4::Source source;
    uint64_t seq;
    return validProductIdentity(receipt.deviceId, command) && eventIdentity(receipt.eventId, source, seq);
}

bool serialize(DynamicJsonDocument& doc, v4::Kind kind, v4::Message& output) {
    if (!doc.capacity() || doc.overflowed()) return false;
    std::unique_ptr<v4::Message> next(new (std::nothrow) v4::Message);
    if (!next) return false;
    detail::BoundedJsonWriter writer{next->payload, v4::kMaxMessage};
    serializeJson(doc, writer);
    if (writer.overflow || !writer.length) return false;
    next->kind = kind;
    next->senderBoot = next->receiverBoot = 0;
    next->messageId = 0;
    next->length = uint16_t(writer.length);
    output = *next;
    return true;
}

bool parse(const uint8_t* bytes, size_t length, size_t maxFields,
           DynamicJsonDocument& doc, size_t& fields) {
    return bytes && length && length <= v4::kMaxMessage && doc.capacity() &&
        v4::validUtf8(bytes, length) &&
        detail::FlatJsonGuard(bytes, length, maxFields, true).object(fields) &&
        !deserializeJson(doc, bytes, length, DeserializationOption::NestingLimit(1)) &&
        !doc.overflowed() && doc.is<JsonObject>() && doc.size() == fields;
}
} // namespace

bool terminalEventFromSlot(const v4::Pairing& pairing, const MotionExecutionSlot& slot,
                           TerminalEvent& output) {
    if (slot.kind != MotionSlotKind::Terminal || !validExecutionId(slot.executionId)) return false;
    uint8_t digest[kProductDigestSize];
    if (!requestDigest(slot.request, digest) || std::memcmp(digest, slot.digest, sizeof(digest))) return false;
    TerminalEvent next;
    next.request = slot.request;
    std::memcpy(next.eventId, slot.eventId, sizeof(next.eventId));
    next.targetPowderG = slot.targetPowderG;
    next.completed = slot.completed;
    next.uptimeMs = slot.uptimeMs;
    std::memcpy(next.reason, slot.reason, sizeof(next.reason));
    std::memcpy(next.errorCode, slot.errorCode, sizeof(next.errorCode));
    if (!validEvent(pairing, next)) return false;
    output = next;
    return true;
}

bool encodeTerminalEvent(const v4::Pairing& pairing, const TerminalEvent& event, v4::Message& output) {
    if (!validEvent(pairing, event)) return false;
    DynamicJsonDocument doc(kDocumentSize);
    if (!doc.capacity()) return false;
    const auto& request = event.request;
    char seq[20]{}, powder[32]{}, target[32]{};
    std::snprintf(seq, sizeof(seq), "%llu", static_cast<unsigned long long>(request.sequence));
    std::snprintf(powder, sizeof(powder), "%.9g", double(request.powderGPer100Ml));
    std::snprintf(target, sizeof(target), "%.9g", double(event.targetPowderG));
    doc["event_id"] = event.eventId;
    doc["event"] = event.completed ? "feeding_completed" : "feeding_failed";
    doc["device_id"] = request.deviceId;
    doc["command_id"] = request.commandId;
    doc["source"] = request.source == v4::Source::CloudCommand ? "cloud_command" : "local_touch";
    if (request.source == v4::Source::CloudCommand) doc["command_seq"] = static_cast<const char*>(seq);
    doc["baby_id"] = request.babyId;
    doc["feeding_context_profile_version"] = request.profileVersion;
    doc["water_ml"] = request.waterMl;
    doc["temp"] = request.temperatureC;
    doc["powder_g_per_100ml"] = serialized(static_cast<const char*>(powder));
    doc["target_powder_g"] = serialized(static_cast<const char*>(target));
    doc["water_delivery_basis"] = "estimated_turns";
    doc["dispensed_water_ml"] = nullptr;
    doc["uptime_ms"] = event.uptimeMs;
    if (event.reason[0]) doc["reason"] = event.reason;
    if (event.errorCode[0]) doc["error_code"] = event.errorCode;
    return serialize(doc, v4::Kind::Terminal, output);
}

bool decodeTerminalEvent(const v4::Message& message, const v4::Pairing& pairing, TerminalEvent& output) {
    if (message.kind != v4::Kind::Terminal || !v4::validPairing(pairing)) return false;
    DynamicJsonDocument doc(kDocumentSize);
    size_t fields;
    if (!parse(message.payload, message.length, kTerminalFields, doc, fields)) return false;
    const auto root = doc.as<JsonObjectConst>();
    TerminalEvent next;
    char eventName[18]{}, source[14]{}, basis[16]{};
    auto& request = next.request;
    request.command = ProductCommand::Prepare;
    if (!readText(root["event_id"], next.eventId) || !readText(root["event"], eventName) ||
        !readText(root["device_id"], request.deviceId) || !readText(root["command_id"], request.commandId) ||
        !readText(root["source"], source) || !readText(root["baby_id"], request.babyId) ||
        !readText(root["water_delivery_basis"], basis) || std::strcmp(basis, "estimated_turns") ||
        !root.containsKey("dispensed_water_ml") || !root["dispensed_water_ml"].isNull() ||
        !root["feeding_context_profile_version"].is<uint32_t>() || !root["water_ml"].is<uint16_t>() ||
        !root["temp"].is<uint8_t>() || !root["uptime_ms"].is<uint32_t>() ||
        !root["powder_g_per_100ml"].is<JsonFloat>() || !root["target_powder_g"].is<JsonFloat>() ||
        !eventIdentity(next.eventId, request.source, request.sequence)) return false;
    const bool cloud = request.source == v4::Source::CloudCommand;
    if (std::strcmp(source, cloud ? "cloud_command" : "local_touch")) return false;
    if (cloud) {
        char seq[20]{};
        uint64_t value;
        if (!readText(root["command_seq"], seq) || !sequence(seq, value) || value != request.sequence) return false;
    } else if (root.containsKey("command_seq")) return false;
    if (!std::strcmp(eventName, "feeding_completed")) next.completed = true;
    else if (std::strcmp(eventName, "feeding_failed")) return false;
    const bool hasReason = root.containsKey("reason"), hasError = root.containsKey("error_code");
    if ((next.completed && (hasReason || hasError)) || (!next.completed && !hasReason) ||
        (hasReason && (!readText(root["reason"], next.reason) || !next.reason[0])) ||
        (hasError && (!readText(root["error_code"], next.errorCode) || !next.errorCode[0])) ||
        fields != 14 + size_t(cloud) + size_t(hasReason) + size_t(hasError)) return false;
    request.profileVersion = root["feeding_context_profile_version"].as<uint32_t>();
    request.waterMl = root["water_ml"].as<uint16_t>();
    request.temperatureC = root["temp"].as<uint8_t>();
    request.powderGPer100Ml = root["powder_g_per_100ml"].as<float>();
    next.targetPowderG = root["target_powder_g"].as<float>();
    next.uptimeMs = root["uptime_ms"].as<uint32_t>();
    if (!validEvent(pairing, next)) return false;
    output = next;
    return true;
}

bool encodeCloudReceipt(const CloudReceipt& receipt, v4::Message& output) {
    if (!validReceipt(receipt)) return false;
    DynamicJsonDocument doc(JSON_OBJECT_SIZE(4));
    if (!doc.capacity()) return false;
    doc["type"] = "feeding_event_receipt";
    doc["device_id"] = receipt.deviceId;
    doc["event_id"] = receipt.eventId;
    doc["status"] = "stored";
    return serialize(doc, v4::Kind::CloudReceipt, output);
}

bool decodeCloudReceipt(const uint8_t* bytes, size_t length, const char* expectedDeviceId,
                        CloudReceipt& output) {
    if (!expectedDeviceId) return false;
    DynamicJsonDocument doc(JSON_OBJECT_SIZE(4) + 256);
    size_t fields;
    if (!parse(bytes, length, 4, doc, fields) || fields != 4) return false;
    const auto root = doc.as<JsonObjectConst>();
    CloudReceipt next;
    char type[22]{}, status[7]{};
    if (!readText(root["type"], type) || std::strcmp(type, "feeding_event_receipt") ||
        !readText(root["status"], status) || std::strcmp(status, "stored") ||
        !readText(root["device_id"], next.deviceId) || std::strcmp(next.deviceId, expectedDeviceId) ||
        !readText(root["event_id"], next.eventId) || !validReceipt(next)) return false;
    output = next;
    return true;
}

bool decodeCloudReceipt(const v4::Message& message, const char* expectedDeviceId, CloudReceipt& output) {
    return message.kind == v4::Kind::CloudReceipt &&
        decodeCloudReceipt(message.payload, message.length, expectedDeviceId, output);
}

} }
