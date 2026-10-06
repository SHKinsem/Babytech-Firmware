#include "ProductContext.h"
#include "BoardProtocolV4.h"
#include "FlatJsonGuard.h"
#include "RecordBytes.h"

#include <ArduinoJson.h>
#include <cmath>
#include <cstring>
#include <limits>

static_assert(ARDUINOJSON_VERSION_MAJOR == 6, "ProductContext requires ArduinoJson 6");
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
              "Context identity uses IEEE-754 binary32");

namespace babytech { namespace boardlink {
namespace {
constexpr size_t kMaxFields = 11;

bool boundedText(const char* value, size_t capacity, bool nonempty = false) {
    if (!value) return false;
    size_t length = 0;
    while (length < capacity && value[length]) ++length;
    return length < capacity && (!nonempty || length) &&
        v4::validUtf8(reinterpret_cast<const uint8_t*>(value), length);
}

bool deviceId(const char* value) {
    if (!boundedText(value, 65, true)) return false;
    if (!((value[0] >= 'A' && value[0] <= 'Z') || (value[0] >= 'a' && value[0] <= 'z') ||
          (value[0] >= '0' && value[0] <= '9'))) return false;
    for (const char* at = value; *at; ++at)
        if (!((*at >= 'A' && *at <= 'Z') || (*at >= 'a' && *at <= 'z') ||
              (*at >= '0' && *at <= '9') || *at == '-' || *at == '_')) return false;
    return true;
}

bool readText(JsonVariantConst value, char* output, size_t capacity) {
    if (!value.is<JsonString>()) return false;
    const JsonString text = value.as<JsonString>();
    if (text.size() >= capacity ||
        !v4::validUtf8(reinterpret_cast<const uint8_t*>(text.c_str()), text.size())) return false;
    std::memcpy(output, text.c_str(), text.size());
    output[text.size()] = 0;
    return true;
}

bool number(JsonVariantConst value, double minimum, double maximum, double& output) {
    if (!value.is<double>() || value.is<bool>()) return false;
    const double parsed = value.as<double>();
    if (!std::isfinite(parsed) || parsed < minimum || parsed > maximum) return false;
    output = parsed;
    return true;
}

bool valid(const ProductContext& context) {
    if (!deviceId(context.deviceId) || !context.profileVersion ||
        context.profileVersion > INT32_MAX) return false;
    if (context.cleared)
        return !context.babyId[0] && !context.babyName[0] && !context.formulaBrand[0] &&
            !context.waterMl && !context.temperatureC && context.powderGPer100Ml == 0;
    return boundedText(context.babyId, sizeof(context.babyId), true) &&
        boundedText(context.babyName, sizeof(context.babyName)) &&
        boundedText(context.formulaBrand, sizeof(context.formulaBrand)) &&
        context.waterMl >= 30 && context.waterMl <= 500 &&
        context.temperatureC >= 35 && context.temperatureC <= 60 &&
        std::isfinite(context.powderGPer100Ml) &&
        context.powderGPer100Ml >= 1 && context.powderGPer100Ml <= 50;
}

bool knownField(const char* key, bool cleared) {
    for (const char* allowed : {"type", "device_id", "profile_version", "cleared", "updated_at"})
        if (!std::strcmp(key, allowed)) return true;
    if (!cleared)
        for (const char* allowed : {"baby_id", "baby_name", "formula_brand", "water_ml",
                                   "temp", "powder_g_per_100ml"})
            if (!std::strcmp(key, allowed)) return true;
    return false;
}

void putInteger(uint8_t*& at, uint32_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) { *at++ = uint8_t(value); value >>= 8; }
}
void putText(uint8_t*& at, const char* text) {
    const size_t length = std::strlen(text);
    putInteger(at, uint32_t(length), 2);
    std::memcpy(at, text, length);
    at += length;
}
}

bool decodeProductContext(const uint8_t* bytes, size_t length,
                          const char* expectedDeviceId, ProductContext& output) {
    if (!bytes || !length || length > v4::kMaxMessage || !deviceId(expectedDeviceId) ||
        !v4::validUtf8(bytes, length)) return false;
    size_t fields;
    if (!detail::FlatJsonGuard(bytes, length, kMaxFields, true).object(fields)) return false;
    // Commissioning is not a polling path. Keep the 2KB JSON scratch off the
    // caller's stack and treat allocation/parse failures like any invalid input.
    DynamicJsonDocument doc(JSON_OBJECT_SIZE(kMaxFields) + v4::kMaxMessage + 1);
    if (deserializeJson(doc, bytes, length, DeserializationOption::NestingLimit(1)) ||
        doc.overflowed() || !doc.is<JsonObject>() || doc.size() != fields) return false;
    const JsonObjectConst object = doc.as<JsonObjectConst>();
    if (!object["type"].is<const char*>() || object["type"] != "feeding_context" ||
        !object["profile_version"].is<uint32_t>() ||
        (object.containsKey("cleared") && !object["cleared"].is<bool>())) return false;
    ProductContext parsed;
    parsed.profileVersion = object["profile_version"].as<uint32_t>();
    parsed.cleared = object["cleared"] | false;
    if (!readText(object["device_id"], parsed.deviceId, sizeof(parsed.deviceId)) ||
        std::strcmp(parsed.deviceId, expectedDeviceId)) return false;
    for (JsonPairConst field : object)
        if (!knownField(field.key().c_str(), parsed.cleared)) return false;
    if (object.containsKey("updated_at") && !object["updated_at"].is<JsonString>()) return false;
    if (!parsed.cleared) {
        if (!readText(object["baby_id"], parsed.babyId, sizeof(parsed.babyId))) return false;
        if (object.containsKey("baby_name") &&
            !readText(object["baby_name"], parsed.babyName, sizeof(parsed.babyName))) return false;
        if (object.containsKey("formula_brand")) {
            if (!readText(object["formula_brand"], parsed.formulaBrand, sizeof(parsed.formulaBrand)))
                return false;
        } else std::strcpy(parsed.formulaBrand, "Friso");
        double water, temperature, powder;
        if (!number(object["water_ml"], 30, 500, water) ||
            !number(object["temp"], 35, 60, temperature) ||
            !number(object["powder_g_per_100ml"], -std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::max(), powder)) return false;
        parsed.waterMl = uint16_t(water);
        parsed.temperatureC = uint8_t(temperature);
        // Legacy Motion validates the stored rate after binary32 conversion.
        // Retain that boundary; valid() checks the resulting 1..50 range.
        parsed.powderGPer100Ml = float(powder);
    }
    if (!valid(parsed)) return false;
    output = parsed;
    return true;
}

bool validProductContext(const ProductContext& context) { return valid(context); }

size_t encodeContextIdentity(const ProductContext& context, uint8_t* output, size_t capacity) {
    if (!valid(context)) return 0;
    size_t length = 2 + 2 + std::strlen(context.deviceId) + 4;
    if (!context.cleared)
        length += 2 + std::strlen(context.babyId) + 2 + std::strlen(context.babyName) +
            2 + std::strlen(context.formulaBrand) + 2 + 1 + 4;
    if (!output || capacity < length) return 0;
    uint8_t* at = output;
    *at++ = 1;
    *at++ = context.cleared ? 1 : 0;
    putText(at, context.deviceId);
    putInteger(at, context.profileVersion, 4);
    if (!context.cleared) {
        putText(at, context.babyId);
        putText(at, context.babyName);
        putText(at, context.formulaBrand);
        putInteger(at, context.waterMl, 2);
        *at++ = context.temperatureC;
        uint32_t bits;
        std::memcpy(&bits, &context.powderGPer100Ml, sizeof(bits));
        putInteger(at, bits, 4);
    }
    return length;
}

bool decodeContextIdentity(const uint8_t* bytes, size_t length, ProductContext& output) {
    if (!bytes || !length || length > kContextIdentityMaxSize) return false;
    detail::ByteReader reader(bytes, length);
    if (reader.integer(1) != 1) return false;
    const auto cleared = reader.integer(1);
    if (cleared > 1) return false;
    ProductContext parsed;
    parsed.cleared = cleared == 1;
    if (!reader.text(parsed.deviceId, sizeof(parsed.deviceId))) return false;
    parsed.profileVersion = uint32_t(reader.integer(4));
    if (!parsed.cleared) {
        if (!reader.text(parsed.babyId, sizeof(parsed.babyId)) ||
            !reader.text(parsed.babyName, sizeof(parsed.babyName)) ||
            !reader.text(parsed.formulaBrand, sizeof(parsed.formulaBrand))) return false;
        parsed.waterMl = uint16_t(reader.integer(2));
        parsed.temperatureC = uint8_t(reader.integer(1));
        const uint32_t bits = uint32_t(reader.integer(4));
        std::memcpy(&parsed.powderGPer100Ml, &bits, sizeof(bits));
    }
    if (!reader.done() || !valid(parsed)) return false;
    output = parsed;
    return true;
}

bool sameProductContext(const ProductContext& left, const ProductContext& right) {
    if (!valid(left) || !valid(right) || std::strcmp(left.deviceId, right.deviceId) ||
        left.profileVersion != right.profileVersion || left.cleared != right.cleared) return false;
    return left.cleared || (!std::strcmp(left.babyId, right.babyId) &&
        !std::strcmp(left.babyName, right.babyName) && !std::strcmp(left.formulaBrand, right.formulaBrand) &&
        left.waterMl == right.waterMl && left.temperatureC == right.temperatureC &&
        left.powderGPer100Ml == right.powderGPer100Ml);
}

} }
