#include "ProductResultQuery.h"
#include "FlatJsonGuard.h"

#include <ArduinoJson.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static_assert(ARDUINOJSON_VERSION_MAJOR == 6, "ProductBoardLink requires ArduinoJson 6");

namespace babytech { namespace boardlink {
namespace {
constexpr size_t kQueryFields = 4;
constexpr size_t kResultFields = 9;
// Decoded keys and maximum bounded values fit in 640 bytes, independent of
// wire escaping. No dynamic document or full-message stack scratch is needed.
using Document = StaticJsonDocument<JSON_OBJECT_SIZE(kResultFields) + 640>;

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
bool code(const char (&text)[65]) {
    size_t length;
    if (!bounded(text, length) || !length) return false;
    for (size_t i = 0; i < length; ++i)
        if (!((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z') ||
              (text[i] >= '0' && text[i] <= '9') || text[i] == '_')) return false;
    return true;
}
const char* statusName(ResultQueryStatus status) {
    switch (status) {
        case ResultQueryStatus::Known: return "known";
        case ResultQueryStatus::Unknown: return "unknown";
        case ResultQueryStatus::Expired: return "result_expired";
        case ResultQueryStatus::Conflict: return "request_conflict";
        case ResultQueryStatus::StorageFault: return "storage_fault";
        default: return nullptr;
    }
}
const char* outcomeName(MotionOutcome outcome) {
    switch (outcome) {
        case MotionOutcome::None: return "none";
        case MotionOutcome::Succeeded: return "succeeded";
        case MotionOutcome::Interrupted: return "interrupted";
        case MotionOutcome::Failed: return "failed";
        default: return nullptr;
    }
}
bool validResult(const QueriedResult& result) {
    if (!validResultQuery(result.query) || !statusName(result.status) ||
        !outcomeName(result.outcome) || !code(result.reason)) return false;
    size_t length;
    if (!bounded(result.requestDigestHex, length)) return false;
    if (result.status != ResultQueryStatus::Known)
        return !result.accepted && result.outcome == MotionOutcome::None && !length &&
            !std::strcmp(result.reason, statusName(result.status));
    const bool positive = !std::strcmp(result.reason, "accepted") ||
        !std::strcmp(result.reason, "already_idle") || !std::strcmp(result.reason, "already_clear");
    if (result.accepted != positive || (!result.accepted && result.outcome != MotionOutcome::None))
        return false;
    // An empty Known digest represents Cloud Stop, never an ordinary result.
    if (!length)
        return result.query.source == v4::Source::CloudCommand &&
            result.outcome == MotionOutcome::None && std::strcmp(result.reason, "already_clear");
    if (!std::strcmp(result.reason, "already_idle") ||
        (result.outcome != MotionOutcome::None && std::strcmp(result.reason, "accepted"))) return false;
    if (length != 64) return false;
    for (size_t i = 0; i < length; ++i)
        if (!((result.requestDigestHex[i] >= '0' && result.requestDigestHex[i] <= '9') ||
              (result.requestDigestHex[i] >= 'a' && result.requestDigestHex[i] <= 'f'))) return false;
    return true;
}
bool parse(const v4::Message& message, Document& doc, size_t expected) {
    size_t fields;
    return message.length && message.length <= v4::kMaxMessage &&
        v4::validUtf8(message.payload, message.length) &&
        detail::FlatJsonGuard(message.payload, message.length, expected).object(fields) &&
        fields == expected &&
        !deserializeJson(doc, message.payload, message.length, DeserializationOption::NestingLimit(1)) &&
        !doc.overflowed() && doc.is<JsonObject>() && doc.size() == fields;
}
bool readQuery(JsonObjectConst root, ResultQuery& query) {
    char source[14]{}, sequence[20]{};
    if (!readText(root["device_id"], query.deviceId) ||
        !readText(root["command_id"], query.commandId) ||
        !readText(root["source"], source) || !readText(root["seq"], sequence)) return false;
    if (!std::strcmp(source, "cloud_command")) query.source = v4::Source::CloudCommand;
    else if (!std::strcmp(source, "local_touch")) query.source = v4::Source::LocalTouch;
    else return false;
    const size_t length = std::strlen(sequence);
    if (!length || sequence[0] == '0' ||
        (length == 19 && std::memcmp(sequence, "9223372036854775807", 19) > 0)) return false;
    for (size_t i = 0; i < length; ++i) {
        if (sequence[i] < '0' || sequence[i] > '9') return false;
        query.sequence = query.sequence * 10 + uint64_t(sequence[i] - '0');
    }
    return validResultQuery(query);
}
void putQuery(Document& doc, const ResultQuery& query, const char* sequence) {
    doc["device_id"] = query.deviceId;
    doc["command_id"] = query.commandId;
    doc["source"] = query.source == v4::Source::CloudCommand ? "cloud_command" : "local_touch";
    doc["seq"] = sequence;
}
// Match ProductBoardMessages' JSON-safe streaming of uncommon ASCII controls.
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
bool serialize(const Document& doc, v4::Kind kind, v4::Message& output) {
    if (doc.overflowed()) return false;
    const size_t length = measureJson(doc);
    JsonWriter measured(nullptr);
    if (!length || serializeJson(doc, measured) != length || measured.failed()) return false;
    JsonWriter writer(output.payload);
    if (serializeJson(doc, writer) != length || writer.failed() || writer.size() != measured.size()) {
        assert(false && "ProductResultQuery serialization invariant");
        std::abort();
    }
    std::memset(output.payload + writer.size(), 0, sizeof(output.payload) - writer.size());
    output.kind = kind;
    output.senderBoot = output.receiverBoot = 0;
    output.messageId = 0;
    output.length = uint16_t(writer.size());
    return true;
}
void digestHex(const uint8_t (&digest)[kProductDigestSize], char (&output)[65]) {
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < kProductDigestSize; ++i) {
        output[2 * i] = hex[digest[i] >> 4];
        output[2 * i + 1] = hex[digest[i] & 15];
    }
    output[64] = 0;
}
bool matches(const ResultQuery& query, uint64_t sequence, const char* id) {
    return query.sequence == sequence && !std::strcmp(query.commandId, id);
}
bool conflicts(const ResultQuery& query, uint64_t sequence, const char* id) {
    return (query.sequence == sequence) != (!std::strcmp(query.commandId, id));
}
void unknown(QueriedResult& result, ResultQueryStatus status) {
    result.status = status;
    std::strcpy(result.reason, statusName(status));
}
void fromSlot(const MotionExecutionSlot& slot, QueriedResult& result) {
    result.status = ResultQueryStatus::Known;
    result.accepted = true;
    std::strcpy(result.reason, "accepted");
    result.outcome = slot.kind == MotionSlotKind::Intent ? MotionOutcome::None :
        (slot.completed ? MotionOutcome::Succeeded : MotionOutcome::Failed);
    digestHex(slot.digest, result.requestDigestHex);
}
}  // namespace

bool validResultQuery(const ResultQuery& query) {
    return (query.source == v4::Source::CloudCommand || query.source == v4::Source::LocalTouch) &&
        query.sequence && query.sequence <= v4::kMaxSequence &&
        validProductIdentity(query.deviceId, query.commandId);
}
bool sameResultQuery(const ResultQuery& left, const ResultQuery& right) {
    return validResultQuery(left) && validResultQuery(right) && left.source == right.source &&
        left.sequence == right.sequence && !std::strcmp(left.deviceId, right.deviceId) &&
        !std::strcmp(left.commandId, right.commandId);
}
bool encodeResultQuery(const ResultQuery& query, v4::Message& output) {
    if (!validResultQuery(query)) return false;
    Document doc;
    char sequence[20];
    std::snprintf(sequence, sizeof(sequence), "%llu", static_cast<unsigned long long>(query.sequence));
    putQuery(doc, query, sequence);
    return serialize(doc, v4::Kind::ResultQuery, output);
}
bool decodeResultQuery(const v4::Message& message, ResultQuery& output) {
    if (message.kind != v4::Kind::ResultQuery) return false;
    Document doc;
    ResultQuery next;
    if (!parse(message, doc, kQueryFields) || !readQuery(doc.as<JsonObjectConst>(), next)) return false;
    output = next;
    return true;
}
bool encodeQueriedResult(const QueriedResult& result, v4::Message& output) {
    if (!validResult(result)) return false;
    Document doc;
    char sequence[20];
    std::snprintf(sequence, sizeof(sequence), "%llu", static_cast<unsigned long long>(result.query.sequence));
    putQuery(doc, result.query, sequence);
    doc["status"] = statusName(result.status);
    doc["accepted"] = result.accepted;
    doc["reason"] = result.reason;
    doc["outcome"] = outcomeName(result.outcome);
    doc["request_digest"] = result.requestDigestHex;
    return serialize(doc, v4::Kind::Result, output);
}
bool decodeQueriedResult(const v4::Message& message, QueriedResult& output) {
    if (message.kind != v4::Kind::Result) return false;
    Document doc;
    QueriedResult next;
    char status[17]{}, outcome[12]{};
    if (!parse(message, doc, kResultFields)) return false;
    const auto root = doc.as<JsonObjectConst>();
    if (!readQuery(root, next.query) || !readText(root["status"], status) ||
        !root["accepted"].is<bool>() || !readText(root["reason"], next.reason) ||
        !readText(root["outcome"], outcome) || !readText(root["request_digest"], next.requestDigestHex)) return false;
    bool found = false;
    for (unsigned i = 0; i <= unsigned(ResultQueryStatus::StorageFault); ++i)
        if (!std::strcmp(status, statusName(ResultQueryStatus(i)))) { next.status = ResultQueryStatus(i); found = true; }
    if (!found) return false;
    found = false;
    for (unsigned i = 0; i <= unsigned(MotionOutcome::Failed); ++i)
        if (!std::strcmp(outcome, outcomeName(MotionOutcome(i)))) { next.outcome = MotionOutcome(i); found = true; }
    next.accepted = root["accepted"].as<bool>();
    if (!found || !validResult(next)) return false;
    output = next;
    return true;
}
bool queryMotionResult(const MotionStateStore& store, const ResultQuery& query, QueriedResult& output) {
    if (!validResultQuery(query)) return false;
    const auto& state = store.state();
    // A fault may precede the first successful load, leaving no verified pair.
    // Do not mistake those default zero watermarks for genuine absence.
    if (v4::validPairing(state.pairing) && std::strcmp(state.pairing.deviceId, query.deviceId)) return false;
    QueriedResult next;
    next.query = query;
    if (!store.ready()) unknown(next, ResultQueryStatus::StorageFault);
    else {
        const bool cloud = query.source == v4::Source::CloudCommand;
        const auto& latest = cloud ? state.cloudResult : state.localResult;
        const uint64_t latestSequence = latest.kind == MotionResultKind::CloudStop ?
            latest.stopSequence : latest.request.sequence;
        const char* latestId = latest.kind == MotionResultKind::CloudStop ?
            latest.stopCommandId : latest.request.commandId;
        const MotionExecutionSlot* exact = nullptr;
        bool conflict = latest.kind != MotionResultKind::None && conflicts(query, latestSequence, latestId);
        // Scan only retained evidence in this source domain. Exact journal
        // evidence carries a feeding outcome absent from the original ACK.
        for (size_t i = 0; i <= state.pendingResultCount; ++i) {
            const auto& slot = i == 0 ? state.slot : state.pendingResults[i - 1];
            if (slot.kind == MotionSlotKind::Empty || slot.request.source != query.source) continue;
            if (matches(query, slot.request.sequence, slot.request.commandId)) exact = &slot;
            conflict |= conflicts(query, slot.request.sequence, slot.request.commandId);
        }
        if (conflict) unknown(next, ResultQueryStatus::Conflict);
        else if (exact) fromSlot(*exact, next);
        else if (latest.kind != MotionResultKind::None && matches(query, latestSequence, latestId)) {
            next.status = ResultQueryStatus::Known;
            next.accepted = latest.accepted;
            std::memcpy(next.reason, latest.reason, sizeof(next.reason));
            next.outcome = latest.outcome;
            if (latest.kind == MotionResultKind::Ordinary) digestHex(latest.digest, next.requestDigestHex);
        } else unknown(next, query.sequence <= (cloud ? state.cloudSequence : state.localSequence) ?
            ResultQueryStatus::Expired : ResultQueryStatus::Unknown);
    }
    output = next;
    return true;
}

} }
