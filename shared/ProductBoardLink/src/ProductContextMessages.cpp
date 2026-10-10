#include "ProductContextMessages.h"
#include "FlatJsonGuard.h"

#include <ArduinoJson.h>
#include <cstring>
#include <memory>
#include <new>

static_assert(ARDUINOJSON_VERSION_MAJOR == 6, "ProductBoardLink requires ArduinoJson 6");
static_assert(uint8_t(babytech::v4::Kind::Context) == 6, "CONTEXT wire kind");
static_assert(uint8_t(babytech::v4::Kind::ContextResult) == 7, "CONTEXT_RESULT wire kind");

namespace babytech { namespace boardlink {
namespace {
constexpr size_t kResultFields = 6;
// Bounded decoded strings and keys fit even when the wire uses Unicode escapes.
using Document = StaticJsonDocument<JSON_OBJECT_SIZE(kResultFields) + 384>;

const char* statusName(ContextStatus status) {
    switch (status) {
        case ContextStatus::Stored: return "stored";
        case ContextStatus::Unchanged: return "unchanged";
        case ContextStatus::Busy: return "busy";
        case ContextStatus::Conflict: return "conflict";
        case ContextStatus::StorageFault: return "storage_fault";
        default: return nullptr;
    }
}

bool validResult(const ContextResult& result) {
    constexpr char unusedCommand[129] = "context";
    return result.replyTo && result.profileVersion && result.profileVersion <= INT32_MAX &&
        validProductIdentity(result.deviceId, unusedCommand) && statusName(result.status);
}

template <size_t N> bool readText(JsonVariantConst value, char (&output)[N]) {
    if (!value.is<JsonString>()) return false;
    const auto text = value.as<JsonString>();
    if (text.size() >= N ||
        !v4::validUtf8(reinterpret_cast<const uint8_t*>(text.c_str()), text.size())) return false;
    std::memcpy(output, text.c_str(), text.size());
    output[text.size()] = 0;
    return true;
}

int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

void setEnvelope(v4::Message& message, v4::Kind kind, size_t length) {
    message.kind = kind;
    message.senderBoot = message.receiverBoot = 0;
    message.messageId = 0;
    message.length = uint16_t(length);
}
}  // namespace

bool encodeContextMessage(const ProductContext& context, v4::Message& output) {
    std::unique_ptr<v4::Message> next(new (std::nothrow) v4::Message);
    if (!next) return false;
    const size_t length = encodeProductContext(context, next->payload, sizeof(next->payload));
    if (!length) return false;
    setEnvelope(*next, v4::Kind::Context, length);
    output = *next;
    return true;
}

bool decodeContextMessage(const v4::Message& message, const char* expectedDeviceId,
                          ProductContext& output) {
    return message.kind == v4::Kind::Context && message.length &&
        message.length <= sizeof(message.payload) &&
        decodeProductContext(message.payload, message.length, expectedDeviceId, output);
}

bool encodeContextResult(const ContextResult& result, v4::Message& output) {
    if (!validResult(result)) return false;
    char hex[65]{};
    constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < kProductDigestSize; ++i) {
        hex[2 * i] = digits[result.digest[i] >> 4];
        hex[2 * i + 1] = digits[result.digest[i] & 15];
    }
    Document doc;
    doc["reply_to"] = result.replyTo;
    doc["device_id"] = result.deviceId;
    doc["profile_version"] = result.profileVersion;
    doc["cleared"] = result.cleared;
    doc["context_digest"] = static_cast<const char*>(hex);
    doc["status"] = statusName(result.status);
    const size_t length = measureJson(doc);
    if (doc.overflowed() || !length || length > v4::kMaxMessage) return false;
    std::unique_ptr<v4::Message> next(new (std::nothrow) v4::Message);
    if (!next || serializeJson(doc, next->payload, sizeof(next->payload)) != length) return false;
    setEnvelope(*next, v4::Kind::ContextResult, length);
    output = *next;
    return true;
}

bool decodeContextResult(const v4::Message& message, ContextResult& output) {
    if (message.kind != v4::Kind::ContextResult || !message.length ||
        message.length > v4::kMaxMessage || !v4::validUtf8(message.payload, message.length)) return false;
    size_t fields;
    if (!detail::FlatJsonGuard(message.payload, message.length, kResultFields).object(fields) ||
        fields != kResultFields) return false;
    Document doc;
    // Raw and decoded counts must agree: escaped duplicate keys cannot collapse
    // into a seemingly valid reply. Requiring all six typed keys excludes extras.
    if (deserializeJson(doc, message.payload, message.length, DeserializationOption::NestingLimit(1)) ||
        doc.overflowed() || !doc.is<JsonObject>() || doc.size() != fields) return false;
    const auto root = doc.as<JsonObjectConst>();
    ContextResult next;
    char hex[65]{}, status[14]{};
    if (!root["reply_to"].is<uint32_t>() || !root["profile_version"].is<uint32_t>() ||
        !root["cleared"].is<bool>() || !readText(root["device_id"], next.deviceId) ||
        !readText(root["context_digest"], hex) || !readText(root["status"], status) ||
        std::strlen(hex) != 64) return false;
    next.replyTo = root["reply_to"].as<uint32_t>();
    next.profileVersion = root["profile_version"].as<uint32_t>();
    next.cleared = root["cleared"].as<bool>();
    for (size_t i = 0; i < kProductDigestSize; ++i) {
        const int high = nibble(hex[2 * i]), low = nibble(hex[2 * i + 1]);
        if (high < 0 || low < 0) return false;
        next.digest[i] = uint8_t((high << 4) | low);
    }
    bool found = false;
    for (auto value : {ContextStatus::Stored, ContextStatus::Unchanged, ContextStatus::Busy,
                       ContextStatus::Conflict, ContextStatus::StorageFault}) {
        if (!std::strcmp(status, statusName(value))) { next.status = value; found = true; break; }
    }
    if (!found || !validResult(next)) return false;
    output = next;
    return true;
}

bool matchesContextResult(const ContextResult& result, const ProductContext& context) {
    if (!validResult(result) || !validProductContext(context) ||
        std::strcmp(result.deviceId, context.deviceId) ||
        result.profileVersion != context.profileVersion || result.cleared != context.cleared) return false;
    uint8_t digest[kProductDigestSize];
    return contextDigest(context, digest) && !std::memcmp(result.digest, digest, sizeof(digest));
}

} }
