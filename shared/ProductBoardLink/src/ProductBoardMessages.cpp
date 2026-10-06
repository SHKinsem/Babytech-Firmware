#include "ProductBoardMessages.h"
#include "FlatJsonGuard.h"

#include <ArduinoJson.h>
#include <cassert>
#include <cstdlib>
#include <cstring>

static_assert(ARDUINOJSON_VERSION_MAJOR == 6, "ProductBoardLink requires ArduinoJson 6");

namespace babytech { namespace boardlink {
namespace {
constexpr size_t kHelloFields = 7;
constexpr size_t kStatusFields = 35;
using Document = StaticJsonDocument<JSON_OBJECT_SIZE(kStatusFields) + v4::kMaxMessage + 1>;

bool validHelloKind(v4::Kind kind, uint32_t replyTo) {
    return (kind == v4::Kind::Hello && replyTo == 0) ||
           (kind == v4::Kind::HelloAck && replyTo != 0);
}

bool boundedText(const char* text, size_t capacity, size_t& length) {
    for (length = 0; length < capacity; ++length) {
        if (!text[length])
            return v4::validUtf8(reinterpret_cast<const uint8_t*>(text), length);
    }
    return false;
}

bool readText(JsonVariantConst value, char* output, size_t capacity) {
    if (!value.is<JsonString>()) return false;
    const JsonString text = value.as<JsonString>();
    if (text.size() >= capacity ||
        !v4::validUtf8(reinterpret_cast<const uint8_t*>(text.c_str()), text.size()))
        return false;
    std::memcpy(output, text.c_str(), text.size());
    output[text.size()] = 0;
    return true;
}

template <typename T> bool readInteger(JsonVariantConst value, T& output) {
    if (!value.is<T>()) return false;
    output = value.as<T>();
    return true;
}

bool readBool(JsonVariantConst value, bool& output) {
    if (!value.is<bool>()) return false;
    output = value.as<bool>();
    return true;
}

bool watermark(const char (&text)[20]) {
    size_t length;
    if (!boundedText(text, sizeof(text), length) || !length ||
        (length > 1 && text[0] == '0')) return false;
    for (size_t i = 0; i < length; ++i)
        if (text[i] < '0' || text[i] > '9') return false;
    return length < 19 || std::memcmp(text, "9223372036854775807", 19) <= 0;
}

bool executionId(const char (&text)[33]) {
    size_t length;
    if (!boundedText(text, sizeof(text), length)) return false;
    if (!length) return true;
    if (length != 32) return false;
    bool nonzero = false;
    for (size_t i = 0; i < length; ++i) {
        if (!((text[i] >= '0' && text[i] <= '9') ||
              (text[i] >= 'a' && text[i] <= 'f'))) return false;
        nonzero |= text[i] != '0';
    }
    return nonzero;
}

bool productProgress(const char (&text)[24]) {
    size_t length;
    if (!boundedText(text, sizeof(text), length)) return false;
    for (const char* value : {"ready", "noready", "error", "cleaning", "unscrewing_cap",
                              "dispensing_water", "dispensing_powder", "screwing_cap",
                              "mixing", "complete"})
        if (std::strcmp(text, value) == 0) return true;
    return false;
}

bool productError(const char (&text)[40]) {
    size_t length;
    if (!boundedText(text, sizeof(text), length) || !length) return false;
    for (size_t i = 0; i < length; ++i)
        if (!((text[i] >= 'A' && text[i] <= 'Z') ||
              (text[i] >= '0' && text[i] <= '9') || text[i] == '_')) return false;
    return true;
}

bool validStatus(const Status& status) {
    const auto& s = status.snapshot;
    size_t length;
    return s.schemaVersion == display::kDisplaySchemaVersion &&
           uint8_t(s.stage) <= uint8_t(display::DisplayStage::Unknown) &&
           uint8_t(s.primaryCondition) <= uint8_t(display::DisplayCondition::Ready) &&
           uint8_t(s.footerCondition) <= uint8_t(display::DisplayCondition::Ready) &&
           uint8_t(s.error) <= uint8_t(display::DisplayError::PowderMotorFault) &&
           boundedText(s.babyName.data(), s.babyName.size(), length) &&
           boundedText(s.formulaBrand.data(), s.formulaBrand.size(), length) &&
           status.contextVersion <= INT32_MAX && watermark(status.cloudWatermark) &&
           watermark(status.localWatermark) && executionId(status.activeExecutionId) &&
           boundedText(status.pendingEventId, sizeof(status.pendingEventId), length) &&
           productProgress(status.productProgress) && productError(status.productError) &&
           status.powderGrams >= 0 && boundedText(status.babyId, sizeof(status.babyId), length);
}

bool parse(const v4::Message& message, Document& doc, size_t expectedFields) {
    if (!message.length || message.length > v4::kMaxMessage ||
        !v4::validUtf8(message.payload, message.length)) return false;
    size_t fields;
    if (!detail::FlatJsonGuard(message.payload, message.length, kStatusFields).object(fields) ||
        fields != expectedFields)
        return false;
    if (deserializeJson(doc, static_cast<const uint8_t*>(message.payload), message.length,
                        DeserializationOption::NestingLimit(1)) || doc.overflowed() ||
        !doc.is<JsonObject>()) return false;
    return doc.size() == fields;
}

// ArduinoJson 6 emits uncommon ASCII controls (e.g. U+0001) literally inside
// strings. Adapt its compact serializer stream to JSON-safe escapes, without
// constructing JSON fields ourselves. The final expanded size is authoritative.
class JsonWriter {
public:
    JsonWriter(uint8_t* output, size_t capacity) : output_(output), capacity_(capacity) {}
    size_t write(uint8_t c) {
        if (failed_) return 0;
        const size_t needed = c < 0x20 ? 6 : 1;
        if (needed > capacity_ - size_) { failed_ = true; return 0; }
        if (output_ && c < 0x20) {
            constexpr char hexDigits[] = "0123456789abcdef";
            const uint8_t escape[] = {'\\', 'u', '0', '0',
                                      uint8_t(hexDigits[c >> 4]), uint8_t(hexDigits[c & 15])};
            std::memcpy(output_ + size_, escape, sizeof(escape));
        } else if (output_) output_[size_] = c;
        size_ += needed;
        return 1;
    }
    size_t write(const uint8_t* data, size_t length) {
        size_t consumed = 0;
        while (consumed < length && write(data[consumed])) ++consumed;
        return consumed;
    }
    bool failed() const { return failed_; }
    size_t size() const { return size_; }
private:
    uint8_t* output_;
    size_t capacity_;
    size_t size_ = 0;
    bool failed_ = false;
};

bool serialize(const JsonDocument& doc, v4::Kind kind, v4::Message& output) {
    if (doc.overflowed()) return false;
    const size_t length = measureJson(doc);
    if (!length || length > sizeof(output.payload)) return false;
    // measureJson excludes our extra control-character escapes. Dry-run the
    // same bounded writer before touching output so every rejection is atomic.
    JsonWriter measured(nullptr, sizeof(output.payload));
    if (serializeJson(doc, measured) != length || measured.failed()) return false;

    JsonWriter writer(output.payload, sizeof(output.payload));
    const size_t written = serializeJson(doc, writer);
    // The const document and linked input strings cannot change during this
    // call. After preflight, a short write is an invariant failure, not false.
    if (written != length || writer.failed() || writer.size() != measured.size()) {
        assert(false && "ProductBoardLink serialization invariant");
        std::abort();
    }
    std::memset(output.payload + writer.size(), 0, sizeof(output.payload) - writer.size());
    output.kind = kind;
    output.senderBoot = output.receiverBoot = 0;
    output.messageId = 0;
    output.length = uint16_t(writer.size());
    return true;
}
}  // namespace

bool encodeHello(const v4::Hello& hello, v4::Message& output, v4::Kind kind) {
    if (!validHelloKind(kind, hello.replyTo) || !v4::validHello(hello))
        return false;
    // Encoding links validated const strings; only seven object slots are used.
    StaticJsonDocument<JSON_OBJECT_SIZE(kHelloFields)> doc;
    doc["protocol"] = hello.protocol;
    doc["role"] = hello.role == v4::Role::Brain ? "brain" : "motion";
    doc["capabilities"] = hello.capabilities;
    doc["reply_to"] = hello.replyTo;
    doc["device_id"] = hello.deviceId;
    doc["pairing_epoch"] = hello.epoch;
    doc["physical_id"] = hello.physicalId;
    return serialize(doc, kind, output);
}

bool decodeHello(const v4::Message& message, v4::Hello& output) {
    if (message.kind != v4::Kind::Hello && message.kind != v4::Kind::HelloAck) return false;
    Document doc;
    if (!parse(message, doc, kHelloFields)) return false;
    const JsonObjectConst root = doc.as<JsonObjectConst>();
    v4::Hello next;
    char role[7]{};
    if (!readInteger(root["protocol"], next.protocol) ||
        !readInteger(root["capabilities"], next.capabilities) ||
        !readInteger(root["reply_to"], next.replyTo) ||
        !readText(root["role"], role, sizeof(role)) ||
        !readText(root["device_id"], next.deviceId, sizeof(next.deviceId)) ||
        !readText(root["pairing_epoch"], next.epoch, sizeof(next.epoch)) ||
        !readText(root["physical_id"], next.physicalId, sizeof(next.physicalId))) return false;
    if (std::strcmp(role, "brain") == 0) next.role = v4::Role::Brain;
    else if (std::strcmp(role, "motion") == 0) next.role = v4::Role::Motion;
    else return false;
    if (!validHelloKind(message.kind, next.replyTo) || !v4::validHello(next)) return false;
    output = next;
    return true;
}

bool encodeStatus(const Status& status, v4::Message& output) {
    if (!validStatus(status)) return false;
    const auto& s = status.snapshot;
    // All keys and validated const strings are linked, not copied into the pool.
    StaticJsonDocument<JSON_OBJECT_SIZE(kStatusFields)> doc;
    doc["schema_version"] = s.schemaVersion;
    doc["stage"] = uint8_t(s.stage);
    doc["primary_condition"] = uint8_t(s.primaryCondition);
    doc["footer_condition"] = uint8_t(s.footerCondition);
    doc["error"] = uint8_t(s.error);
    doc["cloud_connected"] = s.cloudConnected;
    doc["start_enabled"] = s.startEnabled;
    doc["thermal_simulated"] = s.thermalSimulated;
    doc["water_ml"] = s.waterMl;
    doc["temperature_c"] = s.temperatureC;
    doc["baby_name"] = s.babyName.data();
    doc["formula_brand"] = s.formulaBrand.data();
    doc["sample_uptime_ms"] = status.sampleUptimeMs;
    doc["context_version"] = status.contextVersion;
    doc["cloud_watermark"] = status.cloudWatermark;
    doc["local_watermark"] = status.localWatermark;
    doc["motion_busy"] = status.motionBusy;
    doc["stationary"] = status.stationary;
    doc["event_pending"] = status.eventPending;
    doc["active_execution_id"] = status.activeExecutionId;
    doc["pending_event_id"] = status.pendingEventId;
    doc["product_progress"] = status.productProgress;
    doc["product_error"] = status.productError;
    doc["is_preparing"] = status.isPreparing;
    doc["low_water_valid"] = status.lowWaterValid;
    doc["low_water"] = status.lowWater;
    doc["powder_valid"] = status.powderValid;
    doc["powder_grams"] = status.powderGrams;
    doc["actuator_operational"] = status.actuatorOperational;
    doc["actuator_config_valid"] = status.actuatorConfigValid;
    doc["actuator_bus_healthy"] = status.actuatorBusHealthy;
    doc["actuator_position_referenced"] = status.actuatorPositionReferenced;
    doc["execution_authorized"] = status.executionAuthorized;
    doc["feeding_context_configured"] = status.feedingContextConfigured;
    doc["baby_id"] = status.babyId;
    return serialize(doc, v4::Kind::Status, output);
}

bool decodeStatus(const v4::Message& message, Status& output) {
    if (message.kind != v4::Kind::Status) return false;
    Document doc;
    if (!parse(message, doc, kStatusFields)) return false;
    const JsonObjectConst root = doc.as<JsonObjectConst>();
    Status next;
    auto& s = next.snapshot;
    uint8_t stage, primary, footer, error;
    if (!readInteger(root["schema_version"], s.schemaVersion) ||
        !readInteger(root["stage"], stage) || !readInteger(root["primary_condition"], primary) ||
        !readInteger(root["footer_condition"], footer) || !readInteger(root["error"], error) ||
        !readBool(root["cloud_connected"], s.cloudConnected) ||
        !readBool(root["start_enabled"], s.startEnabled) ||
        !readBool(root["thermal_simulated"], s.thermalSimulated) ||
        !readInteger(root["water_ml"], s.waterMl) ||
        !readInteger(root["temperature_c"], s.temperatureC) ||
        !readText(root["baby_name"], s.babyName.data(), s.babyName.size()) ||
        !readText(root["formula_brand"], s.formulaBrand.data(), s.formulaBrand.size()) ||
        !readInteger(root["sample_uptime_ms"], next.sampleUptimeMs) ||
        !readInteger(root["context_version"], next.contextVersion) ||
        !readText(root["cloud_watermark"], next.cloudWatermark, sizeof(next.cloudWatermark)) ||
        !readText(root["local_watermark"], next.localWatermark, sizeof(next.localWatermark)) ||
        !readBool(root["motion_busy"], next.motionBusy) ||
        !readBool(root["stationary"], next.stationary) ||
        !readBool(root["event_pending"], next.eventPending) ||
        !readText(root["active_execution_id"], next.activeExecutionId, sizeof(next.activeExecutionId)) ||
        !readText(root["pending_event_id"], next.pendingEventId, sizeof(next.pendingEventId)) ||
        !readText(root["product_progress"], next.productProgress, sizeof(next.productProgress)) ||
        !readText(root["product_error"], next.productError, sizeof(next.productError)) ||
        !readBool(root["is_preparing"], next.isPreparing) ||
        !readBool(root["low_water_valid"], next.lowWaterValid) ||
        !readBool(root["low_water"], next.lowWater) ||
        !readBool(root["powder_valid"], next.powderValid) ||
        !readInteger(root["powder_grams"], next.powderGrams) ||
        !readBool(root["actuator_operational"], next.actuatorOperational) ||
        !readBool(root["actuator_config_valid"], next.actuatorConfigValid) ||
        !readBool(root["actuator_bus_healthy"], next.actuatorBusHealthy) ||
        !readBool(root["actuator_position_referenced"], next.actuatorPositionReferenced) ||
        !readBool(root["execution_authorized"], next.executionAuthorized) ||
        !readBool(root["feeding_context_configured"], next.feedingContextConfigured) ||
        !readText(root["baby_id"], next.babyId, sizeof(next.babyId))) return false;
    s.stage = display::DisplayStage(stage);
    s.primaryCondition = display::DisplayCondition(primary);
    s.footerCondition = display::DisplayCondition(footer);
    s.error = display::DisplayError(error);
    if (!validStatus(next)) return false;
    output = next;
    return true;
}

} }  // namespace babytech::boardlink
