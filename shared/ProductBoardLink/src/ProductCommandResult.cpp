#include "ProductCommandResult.h"
#include "FlatJsonGuard.h"

#include <ArduinoJson.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static_assert(ARDUINOJSON_VERSION_MAJOR == 6, "ProductBoardLink requires ArduinoJson 6");
static_assert(uint8_t(babytech::v4::Kind::CommandResult) == 9, "COMMAND_RESULT wire kind");

namespace babytech { namespace boardlink {
namespace {
constexpr size_t kFields = 5;
// Five decoded keys and bounded strings need at most 266 bytes, regardless
// of wire escaping. Neither a dynamic document nor a Message scratch is used.
using Document = StaticJsonDocument<JSON_OBJECT_SIZE(kFields) + 320>;

template <size_t N> bool bounded(const char (&text)[N], size_t& length) {
    for (length = 0; length < N; ++length)
        if (!text[length])
            return v4::validUtf8(reinterpret_cast<const uint8_t*>(text), length);
    return false;
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
bool valid(const CommandResult& result) {
    if ((result.source != v4::Source::CloudCommand && result.source != v4::Source::LocalTouch) ||
        !result.sequence || result.sequence > v4::kMaxSequence) return false;
    size_t length;
    if (!bounded(result.commandId, length) || !length ||
        !bounded(result.reason, length) || !length) return false;
    for (size_t i = 0; i < length; ++i)
        if (!((result.reason[i] >= 'a' && result.reason[i] <= 'z') ||
              (result.reason[i] >= 'A' && result.reason[i] <= 'Z') ||
              (result.reason[i] >= '0' && result.reason[i] <= '9') || result.reason[i] == '_')) return false;
    const bool positive = !std::strcmp(result.reason, "accepted") ||
        !std::strcmp(result.reason, "already_clear");
    return result.accepted == positive && std::strcmp(result.reason, "already_idle");
}
bool parse(const v4::Message& message, Document& doc) {
    size_t fields;
    return message.length && message.length <= v4::kMaxMessage &&
        v4::validUtf8(message.payload, message.length) &&
        detail::FlatJsonGuard(message.payload, message.length, kFields).object(fields) &&
        fields == kFields &&
        !deserializeJson(doc, message.payload, message.length, DeserializationOption::NestingLimit(1)) &&
        !doc.overflowed() && doc.is<JsonObject>() && doc.size() == fields;
}
// ArduinoJson can emit uncommon ASCII controls literally. Measure and stream
// their JSON-safe escapes, just as ProductResultQuery does, before committing.
class JsonWriter {
public:
    explicit JsonWriter(uint8_t* output) : output_(output) {}
    size_t write(uint8_t c) {
        if (failed_) return 0;
        const size_t needed = c < 0x20 ? 6 : 1;
        if (needed > v4::kMaxMessage - size_) { failed_ = true; return 0; }
        if (output_ && c < 0x20) {
            constexpr char hex[] = "0123456789abcdef";
            const uint8_t escape[] = {'\\', 'u', '0', '0', uint8_t(hex[c >> 4]), uint8_t(hex[c & 15])};
            std::memcpy(output_ + size_, escape, sizeof(escape));
        } else if (output_) output_[size_] = c;
        size_ += needed;
        return 1;
    }
    size_t write(const uint8_t* bytes, size_t length) {
        size_t consumed = 0;
        while (consumed < length && write(bytes[consumed])) ++consumed;
        return consumed;
    }
    size_t size() const { return size_; }
    bool failed() const { return failed_; }
private:
    uint8_t* output_;
    size_t size_ = 0;
    bool failed_ = false;
};
bool serialize(const Document& doc, v4::Message& output) {
    if (doc.overflowed()) return false;
    const size_t length = measureJson(doc);
    JsonWriter measured(nullptr);
    if (!length || serializeJson(doc, measured) != length || measured.failed()) return false;
    JsonWriter writer(output.payload);
    if (serializeJson(doc, writer) != length || writer.failed() || writer.size() != measured.size()) {
        assert(false && "ProductCommandResult serialization invariant");
        std::abort();
    }
    std::memset(output.payload + writer.size(), 0, sizeof(output.payload) - writer.size());
    output.kind = v4::Kind::CommandResult;
    output.senderBoot = output.receiverBoot = 0;
    output.messageId = 0;
    output.length = uint16_t(writer.size());
    return true;
}
}  // namespace

bool encodeCommandResult(const CommandResult& result, v4::Message& output) {
    if (!valid(result)) return false;
    Document doc;
    char sequence[20];
    std::snprintf(sequence, sizeof(sequence), "%llu", static_cast<unsigned long long>(result.sequence));
    doc["command_id"] = result.commandId;
    doc["source"] = result.source == v4::Source::CloudCommand ? "cloud_command" : "local_touch";
    doc["seq"] = sequence;
    doc["accepted"] = result.accepted;
    doc["reason"] = result.reason;
    return serialize(doc, output);
}

bool decodeCommandResult(const v4::Message& message, CommandResult& output) {
    if (message.kind != v4::Kind::CommandResult) return false;
    Document doc;
    CommandResult next;
    char source[14]{}, sequence[20]{};
    if (!parse(message, doc)) return false;
    const auto root = doc.as<JsonObjectConst>();
    if (!readText(root["command_id"], next.commandId) || !readText(root["source"], source) ||
        !readText(root["seq"], sequence) || !root["accepted"].is<bool>() ||
        !readText(root["reason"], next.reason)) return false;
    if (!std::strcmp(source, "cloud_command")) next.source = v4::Source::CloudCommand;
    else if (!std::strcmp(source, "local_touch")) next.source = v4::Source::LocalTouch;
    else return false;
    const size_t length = std::strlen(sequence);
    if (!length || sequence[0] == '0' ||
        (length == 19 && std::memcmp(sequence, "9223372036854775807", 19) > 0)) return false;
    for (size_t i = 0; i < length; ++i) {
        if (sequence[i] < '0' || sequence[i] > '9') return false;
        next.sequence = next.sequence * 10 + uint64_t(sequence[i] - '0');
    }
    next.accepted = root["accepted"].as<bool>();
    if (!valid(next)) return false;
    output = next;
    return true;
}

} }
