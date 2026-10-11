#include "ProductEventMessages.h"
#include "MotionStateRecord.h"
#include "FakeProductCrypto.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace babytech::boardlink;
namespace v4 = babytech::v4;

static_assert(uint8_t(v4::Kind::Terminal) == 12, "TERMINAL contract kind");
static_assert(uint8_t(v4::Kind::CloudReceipt) == 13, "CLOUD_RECEIPT contract kind");
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559, "binary32 tests");

namespace {
unsigned scenarios = 0, failures = 0, checks = 0, roundtrips = 0, rejections = 0, extracts = 0;
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + \
    std::to_string(__LINE__) + ": " #x); } while (false)
using Fields = std::vector<std::pair<std::string, std::string>>;
const std::string epoch = "0123456789abcdef0123456789abcdef";

template <typename T> std::array<uint8_t, sizeof(T)> raw(const T& value) {
    std::array<uint8_t, sizeof(T)> bytes;
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}
template <size_t N> void set(char (&output)[N], const std::string& text) {
    CHECK(text.size() < N);
    std::memset(output, 0, N);
    std::memcpy(output, text.data(), text.size());
}
uint32_t bits(float value) {
    uint32_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
float fromBits(uint32_t value) {
    float result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
std::string number(float value) {
    char buffer[32];
    CHECK(std::snprintf(buffer, sizeof(buffer), "%.9g", double(value)) > 0);
    return buffer;
}
std::string quote(const std::string& text) {
    std::string result = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { result += '\\'; result += char(c); }
        else if (c < 0x20) {
            char escape[7];
            std::snprintf(escape, sizeof(escape), "\\u%04x", unsigned(c));
            result += escape;
        } else result += char(c);
    }
    return result + '"';
}
std::string object(const Fields& fields) {
    std::string result = "{";
    for (const auto& field : fields) {
        if (result.size() > 1) result += ',';
        result += quote(field.first) + ':' + field.second;
    }
    return result + '}';
}
Fields replace(Fields fields, const std::string& key, const std::string& token) {
    for (auto& field : fields) if (field.first == key) { field.second = token; return fields; }
    fields.emplace_back(key, token);
    return fields;
}
Fields without(Fields fields, const std::string& key) {
    fields.erase(std::remove_if(fields.begin(), fields.end(), [&](const auto& field) {
        return field.first == key;
    }), fields.end());
    return fields;
}
v4::Pairing pairing(v4::Role role = v4::Role::Brain) {
    v4::Pairing p;
    p.role = role;
    set(p.deviceId, "Babytech_01");
    set(p.epoch, epoch);
    set(p.localPhysicalId, role == v4::Role::Brain ? "aabbccddeeff" : "112233445566");
    set(p.peerPhysicalId, role == v4::Role::Brain ? "112233445566" : "aabbccddeeff");
    CHECK(v4::validPairing(p));
    return p;
}
std::string eventId(v4::Source source, uint64_t sequence, const std::string& e = epoch) {
    return "evt-" + e + (source == v4::Source::CloudCommand ? "-c-" : "-l-") +
        std::to_string(sequence);
}
TerminalEvent event(v4::Source source = v4::Source::CloudCommand, bool completed = true,
                    uint64_t sequence = 42) {
    TerminalEvent t;
    t.request.source = source;
    t.request.command = ProductCommand::Prepare;
    t.request.sequence = sequence;
    set(t.request.deviceId, "Babytech_01");
    set(t.request.commandId, source == v4::Source::CloudCommand ? "cloud-command-42" :
        "local-" + epoch + '-' + std::to_string(sequence));
    set(t.request.babyId, "baby-7");
    t.request.profileVersion = 7;
    t.request.waterMl = 120;
    t.request.temperatureC = 40;
    t.request.powderGPer100Ml = 13.123456f;
    set(t.eventId, eventId(source, sequence));
    t.targetPowderG = productTargetPowderG(t.request);
    t.completed = completed;
    t.uptimeMs = 123456;
    if (!completed) { set(t.reason, "stopped"); set(t.errorCode, "E_STOPPED"); }
    CHECK(validProductRequest(t.request));
    return t;
}
Fields fields(const TerminalEvent& t) {
    Fields f = {{"event_id", quote(t.eventId)},
        {"event", quote(t.completed ? "feeding_completed" : "feeding_failed")},
        {"device_id", quote(t.request.deviceId)}, {"command_id", quote(t.request.commandId)},
        {"source", quote(t.request.source == v4::Source::CloudCommand ? "cloud_command" : "local_touch")},
        {"baby_id", quote(t.request.babyId)},
        {"feeding_context_profile_version", std::to_string(t.request.profileVersion)},
        {"water_ml", std::to_string(t.request.waterMl)}, {"temp", std::to_string(t.request.temperatureC)},
        {"powder_g_per_100ml", number(t.request.powderGPer100Ml)},
        {"target_powder_g", number(t.targetPowderG)}, {"water_delivery_basis", "\"estimated_turns\""},
        {"dispensed_water_ml", "null"}, {"uptime_ms", std::to_string(t.uptimeMs)}};
    if (t.request.source == v4::Source::CloudCommand)
        f.emplace_back("command_seq", quote(std::to_string(t.request.sequence)));
    if (!t.completed) {
        f.emplace_back("reason", quote(t.reason));
        if (t.errorCode[0]) f.emplace_back("error_code", quote(t.errorCode));
    }
    return f;
}
CloudReceipt receipt(v4::Source source = v4::Source::CloudCommand, uint64_t sequence = 42) {
    CloudReceipt r;
    set(r.deviceId, "Babytech_01");
    set(r.eventId, eventId(source, sequence));
    return r;
}
Fields fields(const CloudReceipt& r) {
    return {{"type", "\"feeding_event_receipt\""}, {"device_id", quote(r.deviceId)},
            {"event_id", quote(r.eventId)}, {"status", "\"stored\""}};
}
v4::Message wire(const std::string& text, v4::Kind kind = v4::Kind::Terminal) {
    CHECK(text.size() <= v4::kMaxMessage);
    v4::Message m;
    m.kind = kind;
    m.senderBoot = 11;
    m.receiverBoot = 22;
    m.messageId = 33;
    m.length = uint16_t(text.size());
    std::memcpy(m.payload, text.data(), text.size());
    return m;
}
std::string json(const v4::Message& m) {
    CHECK(m.length <= sizeof(m.payload));
    return {reinterpret_cast<const char*>(m.payload), m.length};
}
v4::Message sentinel() {
    auto m = wire("sentinel", v4::Kind::Stop);
    std::memset(m.payload, 0xa5, sizeof(m.payload));
    return m;
}
bool same(const TerminalEvent& a, const TerminalEvent& b) {
    return sameProductRequest(a.request, b.request) &&
        bits(a.request.powderGPer100Ml) == bits(b.request.powderGPer100Ml) &&
        !std::strcmp(a.eventId, b.eventId) && bits(a.targetPowderG) == bits(b.targetPowderG) &&
        a.completed == b.completed && a.uptimeMs == b.uptimeMs &&
        !std::strcmp(a.reason, b.reason) && !std::strcmp(a.errorCode, b.errorCode);
}
bool same(const CloudReceipt& a, const CloudReceipt& b) {
    return !std::strcmp(a.deviceId, b.deviceId) && !std::strcmp(a.eventId, b.eventId);
}
void envelope(const v4::Message& m, v4::Kind kind) {
    CHECK(m.kind == kind && m.length && m.length <= v4::kMaxMessage);
    CHECK(!m.senderBoot && !m.receiverBoot && !m.messageId);
    CHECK(v4::validUtf8(m.payload, m.length));
}
// Independent fixture/parser checks prevent encoder+decoder agreement hiding a wire bug.
void exactFields(const v4::Message& m, const Fields& expected) {
    DynamicJsonDocument actual(8192), wanted(8192);
    CHECK(!deserializeJson(actual, m.payload, m.length));
    CHECK(!deserializeJson(wanted, object(expected)));
    CHECK(actual.is<JsonObject>() && actual.size() == expected.size());
    const auto a = actual.as<JsonObjectConst>(), w = wanted.as<JsonObjectConst>();
    for (const auto& field : expected) {
        CHECK(a.containsKey(field.first));
        const auto av = a[field.first], wv = w[field.first];
        if (wv.is<JsonString>()) {
            CHECK(av.is<JsonString>());
            const auto x = av.as<JsonString>(), y = wv.as<JsonString>();
            CHECK(x.size() == y.size() && !std::memcmp(x.c_str(), y.c_str(), x.size()));
        } else if (wv.isNull()) CHECK(av.isNull());
        else { CHECK(!av.is<bool>() && av.is<double>()); CHECK(av.as<double>() == wv.as<double>()); }
    }
}
v4::Message roundtrip(const v4::Pairing& p, const TerminalEvent& t) {
    auto m = sentinel();
    const auto beforeP = raw(p);
    const auto beforeT = raw(t);
    CHECK(encodeTerminalEvent(p, t, m));
    CHECK(raw(p) == beforeP && raw(t) == beforeT);
    envelope(m, v4::Kind::Terminal);
    exactFields(m, fields(t));
    auto decoded = event(v4::Source::LocalTouch, false, 1);
    const auto beforeM = raw(m);
    CHECK(decodeTerminalEvent(m, p, decoded) && same(t, decoded));
    CHECK(raw(m) == beforeM && raw(p) == beforeP);
    auto again = sentinel();
    CHECK(encodeTerminalEvent(p, decoded, again) && json(again) == json(m));
    auto fixture = wire(object(fields(t)));
    CHECK(decodeTerminalEvent(fixture, p, decoded) && same(t, decoded));
    ++roundtrips;
    return m;
}
v4::Message roundtrip(const CloudReceipt& r) {
    auto m = sentinel();
    const auto beforeR = raw(r);
    CHECK(encodeCloudReceipt(r, m) && raw(r) == beforeR);
    envelope(m, v4::Kind::CloudReceipt);
    exactFields(m, fields(r));
    auto decoded = receipt(v4::Source::LocalTouch, 1);
    const auto beforeM = raw(m);
    CHECK(decodeCloudReceipt(m, r.deviceId, decoded) && same(r, decoded));
    CHECK(raw(m) == beforeM);
    CHECK(decodeCloudReceipt(m.payload, m.length, r.deviceId, decoded) && same(r, decoded));
    auto again = sentinel();
    CHECK(encodeCloudReceipt(decoded, again) && json(again) == json(m));
    const auto fixture = object(fields(r));
    CHECK(decodeCloudReceipt(reinterpret_cast<const uint8_t*>(fixture.data()), fixture.size(),
                             r.deviceId, decoded) && same(r, decoded));
    ++roundtrips;
    return m;
}
void rejectEncode(const v4::Pairing& p, const TerminalEvent& t) {
    auto out = sentinel();
    const auto before = raw(out);
    const auto beforeP = raw(p);
    const auto beforeT = raw(t);
    CHECK(!encodeTerminalEvent(p, t, out));
    CHECK(raw(out) == before && raw(p) == beforeP && raw(t) == beforeT);
    ++rejections;
}
void rejectEncode(const CloudReceipt& r) {
    auto out = sentinel();
    const auto before = raw(out);
    const auto beforeR = raw(r);
    CHECK(!encodeCloudReceipt(r, out));
    CHECK(raw(out) == before && raw(r) == beforeR);
    ++rejections;
}
void rejectTerminal(const v4::Message& m, const v4::Pairing& p = pairing()) {
    auto out = event(v4::Source::LocalTouch, false, 1);
    const auto before = raw(out);
    const auto beforeM = raw(m);
    const auto beforeP = raw(p);
    CHECK(!decodeTerminalEvent(m, p, out));
    CHECK(raw(out) == before && raw(m) == beforeM && raw(p) == beforeP);
    ++rejections;
}
void rejectReceiptBytes(const uint8_t* bytes, size_t length, const char* expected = "Babytech_01") {
    auto out = receipt(v4::Source::LocalTouch, 1);
    const auto before = raw(out);
    const std::vector<uint8_t> input = bytes ? std::vector<uint8_t>(bytes, bytes + length) :
        std::vector<uint8_t>();
    CHECK(!decodeCloudReceipt(bytes, length, expected, out));
    CHECK(raw(out) == before);
    CHECK(!bytes || std::equal(input.begin(), input.end(), bytes));
    ++rejections;
}
void rejectReceipt(const v4::Message& m, const char* expected = "Babytech_01") {
    auto out = receipt(v4::Source::LocalTouch, 1);
    const auto before = raw(out);
    const auto beforeM = raw(m);
    CHECK(!decodeCloudReceipt(m, expected, out));
    CHECK(raw(out) == before && raw(m) == beforeM);
    ++rejections;
    if (m.kind == v4::Kind::CloudReceipt && m.length <= sizeof(m.payload))
        rejectReceiptBytes(m.payload, m.length, expected);
}
MotionExecutionSlot slot(const TerminalEvent& t) {
    MotionExecutionSlot s;
    s.kind = MotionSlotKind::Terminal;
    s.request = t.request;
    if (validProductRequest(s.request)) CHECK(requestDigest(s.request, s.digest));
    set(s.executionId, "0123456789abcdef0123456789abcdef");
    std::memcpy(s.eventId, t.eventId, sizeof(s.eventId));
    s.targetPowderG = t.targetPowderG;
    s.completed = t.completed;
    s.uptimeMs = t.uptimeMs;
    std::memcpy(s.reason, t.reason, sizeof(s.reason));
    std::memcpy(s.errorCode, t.errorCode, sizeof(s.errorCode));
    return s;
}
void rejectSlot(const v4::Pairing& p, const MotionExecutionSlot& s) {
    auto out = event(v4::Source::LocalTouch, false, 1);
    const auto before = raw(out);
    const auto beforeS = raw(s);
    const auto beforeP = raw(p);
    CHECK(!terminalEventFromSlot(p, s, out));
    CHECK(raw(out) == before && raw(s) == beforeS && raw(p) == beforeP);
    ++rejections;
}
void scenario(const std::string& label, const std::function<void()>& test) {
    ++scenarios;
    try { test(); }
    catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "FAIL %s: %s\n", label.c_str(), error.what());
    }
}
void validEvents() {
    for (auto role : {v4::Role::Brain, v4::Role::Motion})
        for (auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch})
            for (uint64_t seq : {UINT64_C(1), UINT64_C(42), v4::kMaxSequence})
                for (unsigned outcome = 0; outcome < 3; ++outcome)
                    scenario("event role/source/seq/outcome " + std::to_string(unsigned(role)) + '/' +
                        std::to_string(unsigned(source)) + '/' + std::to_string(seq) + '/' +
                        std::to_string(outcome), [=] {
                        auto t = event(source, outcome == 0, seq);
                        if (outcome == 2) set(t.errorCode, "");
                        t.uptimeMs = seq == 1 ? 0 : UINT32_MAX;
                        const auto p = pairing(role);
                        roundtrip(p, t);
                        const auto s = slot(t);
                        const auto bytes = raw(s);
                        const auto beforeP = raw(p);
                        auto out = event();
                        CHECK(terminalEventFromSlot(p, s, out) && same(t, out));
                        CHECK(raw(s) == bytes && raw(p) == beforeP);
                        ++extracts;
                    });
    // Exercise receive/extraction independently even when encoding rejects a role.
    for (uint64_t seq : {UINT64_C(1), UINT64_C(42), v4::kMaxSequence})
        for (bool completed : {true, false}) {
            scenario("Motion local direct encode " + std::to_string(seq) + '/' + std::to_string(completed), [=] {
                const auto t = event(v4::Source::LocalTouch, completed, seq);
                const auto p = pairing(v4::Role::Motion);
                const auto beforeT = raw(t);
                const auto beforeP = raw(p);
                auto m = sentinel();
                CHECK(encodeTerminalEvent(p, t, m));
                envelope(m, v4::Kind::Terminal);
                exactFields(m, fields(t));
                CHECK(raw(t) == beforeT && raw(p) == beforeP);
            });
            scenario("Motion local direct decode " + std::to_string(seq) + '/' + std::to_string(completed), [=] {
                const auto t = event(v4::Source::LocalTouch, completed, seq);
                const auto m = wire(object(fields(t)));
                const auto before = raw(m);
                auto decoded = event();
                CHECK(decodeTerminalEvent(m, pairing(v4::Role::Motion), decoded) && same(t, decoded));
                CHECK(raw(m) == before);
            });
            scenario("Motion local direct extraction " + std::to_string(seq) + '/' + std::to_string(completed), [=] {
                const auto t = event(v4::Source::LocalTouch, completed, seq);
                const auto s = slot(t);
                const auto before = raw(s);
                auto decoded = event();
                CHECK(terminalEventFromSlot(pairing(v4::Role::Motion), s, decoded) && same(t, decoded));
                CHECK(raw(s) == before);
                ++extracts;
            });
        }
    for (auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch})
        for (uint64_t seq : {UINT64_C(1), v4::kMaxSequence})
            scenario("receipt source/seq " + std::to_string(unsigned(source)) + '/' +
                std::to_string(seq), [=] { roundtrip(receipt(source, seq)); });
}
void boundaries() {
    scenario("maximum fields and recipe bounds", [] {
        auto p = pairing();
        set(p.deviceId, std::string(64, 'D'));
        for (bool upper : {false, true}) {
            auto t = event(v4::Source::CloudCommand, false, v4::kMaxSequence);
            set(t.request.deviceId, p.deviceId);
            set(t.request.commandId, std::string(128, 'C'));
            set(t.request.babyId, std::string(96, 'B'));
            set(t.reason, std::string(64, 'r'));
            set(t.errorCode, std::string(64, 'E'));
            t.request.profileVersion = upper ? INT32_MAX : 1;
            t.request.waterMl = upper ? 500 : 30;
            t.request.temperatureC = upper ? 60 : 35;
            t.request.powderGPer100Ml = upper ? 50 : 1;
            t.targetPowderG = productTargetPowderG(t.request);
            roundtrip(p, t);
        }
        auto r = receipt(v4::Source::LocalTouch, v4::kMaxSequence);
        set(r.deviceId, p.deviceId);
        roundtrip(r);
    });
    scenario("UTF-8 identities and all representable controls", [] {
        auto t = event();
        const std::string utf8 = "\xe5\xae\x9d\xe5\xae\x9d\xf0\x9f\x8d\xbc";
        std::string controls;
        for (char c = 1; c < 32; ++c) controls += c;
        set(t.request.commandId, utf8 + "\"\\" + controls);
        set(t.request.babyId, controls + utf8 + "\"\\");
        roundtrip(pairing(), t);
    });
    scenario("maximum UTF8 byte lengths", [] {
        auto t = event();
        std::string command, baby;
        for (unsigned i = 0; i < 32; ++i) command += "\xf0\x9f\x8d\xbc";
        for (unsigned i = 0; i < 32; ++i) baby += "\xe5\xae\x9d";
        set(t.request.commandId, command);
        set(t.request.babyId, baby);
        roundtrip(pairing(), t);
    });
    scenario("maximum escaped identities within wire budget", [] {
        auto t = event(v4::Source::CloudCommand, false, v4::kMaxSequence);
        auto p = pairing();
        set(p.deviceId, std::string(64, 'D'));
        set(t.request.deviceId, p.deviceId);
        set(t.request.commandId, std::string(128, '\x01'));
        set(t.request.babyId, std::string(96, '\x02'));
        set(t.reason, std::string(64, 'r'));
        set(t.errorCode, std::string(64, 'E'));
        CHECK(object(fields(t)).size() <= v4::kMaxMessage);
        roundtrip(p, t);
    });
    scenario("valid wire padded to 2047, receipt 2048 rejected", [] {
        auto t = event();
        auto text = object(fields(t));
        text += std::string(v4::kMaxMessage - text.size(), ' ');
        auto out = event(v4::Source::LocalTouch, false);
        CHECK(decodeTerminalEvent(wire(text), pairing(), out) && same(t, out));
        auto r = receipt();
        text = object(fields(r));
        text += std::string(v4::kMaxMessage - text.size(), ' ');
        auto decoded = receipt(v4::Source::LocalTouch, 1);
        CHECK(decodeCloudReceipt(wire(text, v4::Kind::CloudReceipt), r.deviceId, decoded));
        CHECK(same(r, decoded));
        CHECK(decodeCloudReceipt(reinterpret_cast<const uint8_t*>(text.data()), text.size(),
                                 r.deviceId, decoded) && same(r, decoded));
        text += ' ';
        rejectReceiptBytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    });
}
void randomFloats() {
    uint32_t state = 0x12345678;
    for (unsigned i = 0; i < 512; ++i) {
        state = state * 1664525u + 1013904223u;
        const uint32_t floatBits = 0x3f800000u + state % (0x42480000u - 0x3f800000u + 1);
        const uint16_t water = uint16_t(30 + state % 471);
        scenario("seeded binary32 " + std::to_string(i), [=] {
            auto t = event(i % 2 ? v4::Source::LocalTouch : v4::Source::CloudCommand, i % 3 == 0);
            t.request.powderGPer100Ml = fromBits(floatBits);
            t.request.waterMl = water;
            t.targetPowderG = productTargetPowderG(t.request);
            CHECK(bits(t.request.powderGPer100Ml) == floatBits);
            roundtrip(pairing(), t);
        });
    }
}
void invalidEncoding() {
    using Mutation = std::pair<const char*, std::function<void(TerminalEvent&)>>;
    const std::vector<Mutation> mutations = {
        {"sequence zero", [](auto& t) { t.request.sequence = 0; }},
        {"sequence overflow", [](auto& t) { t.request.sequence = v4::kMaxSequence + 1; }},
        {"unknown source", [](auto& t) { t.request.source = v4::Source(3); }},
        {"not prepare", [](auto& t) { t.request.command = ProductCommand::Clean; }},
        {"wrong device", [](auto& t) { set(t.request.deviceId, "Other"); }},
        {"device control", [](auto& t) { set(t.request.deviceId, "D\n"); }},
        {"empty command", [](auto& t) { set(t.request.commandId, ""); }},
        {"empty baby", [](auto& t) { set(t.request.babyId, ""); }},
        {"zero profile", [](auto& t) { t.request.profileVersion = 0; }},
        {"profile overflow", [](auto& t) { t.request.profileVersion = uint32_t(INT32_MAX) + 1; }},
        {"water below", [](auto& t) { t.request.waterMl = 29; }},
        {"water above", [](auto& t) { t.request.waterMl = 501; }},
        {"temp below", [](auto& t) { t.request.temperatureC = 34; }},
        {"temp above", [](auto& t) { t.request.temperatureC = 61; }},
        {"ratio below", [](auto& t) { t.request.powderGPer100Ml = 0.99f; }},
        {"ratio above", [](auto& t) { t.request.powderGPer100Ml = 50.01f; }},
        {"ratio NaN", [](auto& t) { t.request.powderGPer100Ml = std::numeric_limits<float>::quiet_NaN(); }},
        {"ratio infinity", [](auto& t) { t.request.powderGPer100Ml = std::numeric_limits<float>::infinity(); }},
        {"target NaN", [](auto& t) { t.targetPowderG = std::numeric_limits<float>::quiet_NaN(); }},
        {"target infinity", [](auto& t) { t.targetPowderG = std::numeric_limits<float>::infinity(); }},
        {"target negative", [](auto& t) { t.targetPowderG = -1; }},
        {"target not frozen recipe", [](auto& t) { t.targetPowderG += 1; }},
        {"wrong event epoch", [](auto& t) { set(t.eventId, eventId(t.request.source, 42, std::string(32, 'a'))); }},
        {"wrong event source", [](auto& t) { set(t.eventId, eventId(v4::Source::LocalTouch, 42)); }},
        {"wrong event sequence", [](auto& t) { set(t.eventId, eventId(t.request.source, 43)); }},
        {"success reason", [](auto& t) { set(t.reason, "stopped"); }},
        {"success error", [](auto& t) { set(t.errorCode, "E_STOPPED"); }},
        {"failure missing reason", [](auto& t) { t.completed = false; }},
        {"failure reason invalid UTF8", [](auto& t) { t.completed = false; set(t.reason, std::string("\xff", 1)); }},
        {"failure error invalid UTF8", [](auto& t) { t.completed = false; set(t.reason, "stopped");
            set(t.errorCode, std::string("\xff", 1)); }},
        {"command unterminated", [](auto& t) { std::memset(t.request.commandId, 'C', sizeof(t.request.commandId)); }},
        {"baby unterminated", [](auto& t) { std::memset(t.request.babyId, 'B', sizeof(t.request.babyId)); }},
        {"event unterminated", [](auto& t) { std::memset(t.eventId, 'e', sizeof(t.eventId)); }},
        {"reason unterminated", [](auto& t) { t.completed = false; std::memset(t.reason, 'r', sizeof(t.reason)); }},
        {"error unterminated", [](auto& t) { t.completed = false; set(t.reason, "stopped");
            std::memset(t.errorCode, 'E', sizeof(t.errorCode)); }},
        {"command invalid UTF8", [](auto& t) { set(t.request.commandId, std::string("\xc0\xaf", 2)); }},
        {"baby invalid UTF8", [](auto& t) { set(t.request.babyId, std::string("\xed\xa0\x80", 3)); }}
    };
    for (const auto& mutation : mutations) scenario(mutation.first, [&] {
        auto t = event();
        mutation.second(t);
        rejectEncode(pairing(), t);
        rejectSlot(pairing(), slot(t));
    });
    scenario("local command must match canonical epoch and sequence", [] {
        auto t = event(v4::Source::LocalTouch);
        set(t.request.commandId, "cloud-command-42");
        rejectEncode(pairing(), t);
        rejectTerminal(wire(object(fields(t))));
    });
    for (auto command : {ProductCommand::Initialize, ProductCommand::Clean,
                         ProductCommand::SetTargetTemp, ProductCommand::ResetError,
                         ProductCommand::CheckFirmwareUpdate})
        scenario("valid non-feeding request has no terminal event " + std::to_string(unsigned(command)), [=] {
            auto t = event(v4::Source::LocalTouch);
            t.request.command = command;
            set(t.request.babyId, "");
            t.request.profileVersion = 0;
            t.request.waterMl = 0;
            t.request.temperatureC = command == ProductCommand::SetTargetTemp ? 40 : 0;
            t.request.powderGPer100Ml = 0;
            t.targetPowderG = 0;
            CHECK(validProductRequest(t.request));
            rejectEncode(pairing(), t);
            rejectSlot(pairing(), slot(t));
        });
    for (auto kind : {MotionSlotKind::Empty, MotionSlotKind::Intent, MotionSlotKind(3)})
        scenario("nonterminal slot " + std::to_string(unsigned(kind)), [=] {
            auto s = slot(event());
            s.kind = kind;
            rejectSlot(pairing(), s);
        });
    scenario("slot digest/execution identity and SHA failure", [] {
        auto s = slot(event());
        s.digest[0] ^= 1;
        rejectSlot(pairing(), s);
        s = slot(event());
        for (const std::string id : {"", "00000000000000000000000000000000", "not-hex",
                                     "ABCDEF0123456789abcdef0123456789"}) {
            set(s.executionId, id);
            rejectSlot(pairing(), s);
        }
        std::memset(s.executionId, 'a', sizeof(s.executionId));
        rejectSlot(pairing(), s);
        s = slot(event());
        fake_product_crypto::fail = true;
        try { rejectSlot(pairing(), s); }
        catch (...) { fake_product_crypto::fail = false; throw; }
        fake_product_crypto::fail = false;
    });
    const std::vector<std::function<void(v4::Pairing&)>> badPairs = {
        [](auto& p) { p.role = v4::Role(0); },
        [](auto& p) { set(p.deviceId, ""); },
        [](auto& p) { set(p.deviceId, "Other"); },
        [](auto& p) { set(p.epoch, std::string(32, '0')); },
        [](auto& p) { set(p.epoch, std::string(32, 'A')); },
        [](auto& p) { set(p.epoch, std::string(32, 'a')); },
        [](auto& p) { set(p.localPhysicalId, p.peerPhysicalId); },
        [](auto& p) { std::memset(p.epoch, 'a', sizeof(p.epoch)); },
        [](auto& p) { std::memset(p.deviceId, 'D', sizeof(p.deviceId)); },
        [](auto& p) { set(p.peerPhysicalId, "000000000000"); }
    };
    for (size_t i = 0; i < badPairs.size(); ++i) scenario("invalid pairing " + std::to_string(i), [&] {
        auto p = pairing();
        badPairs[i](p);
        const auto t = event();
        rejectEncode(p, t);
        rejectTerminal(wire(object(fields(t))), p);
        rejectSlot(p, slot(t));
    });
}
void terminalFields() {
    const auto base = fields(event());
    for (const auto& field : base) {
        scenario("terminal missing " + field.first, [&] { rejectTerminal(wire(object(without(base, field.first)))); });
        scenario("terminal duplicate " + field.first, [&] {
            auto f = base;
            f.push_back(field);
            rejectTerminal(wire(object(f)));
        });
        scenario("terminal null/bool/array/object " + field.first, [&] {
            for (const std::string token : {"null", "true", "false", "[]", "{}"}) {
                if (field.first == "dispensed_water_ml" && token == "null") continue;
                rejectTerminal(wire(object(replace(base, field.first, token))));
            }
        });
    }
    for (const std::string key : {"feeding_context_profile_version", "water_ml", "temp", "uptime_ms"})
        scenario("terminal integer type " + key, [&] {
            for (const std::string token : {"1.5", "40.0", "-1", "4294967296", "\"40\""})
                rejectTerminal(wire(object(replace(base, key, token))));
        });
    for (const std::string key : {"powder_g_per_100ml", "target_powder_g"})
        scenario("terminal float type " + key, [&] {
            for (const std::string token : {"NaN", "Infinity", "-Infinity", "1e1000", "-1", "\"13.5\""})
                rejectTerminal(wire(object(replace(base, key, token))));
        });
    const Fields bad = {{"event", "\"feeding_started\""}, {"source", "\"legacy\""},
        {"water_delivery_basis", "\"measured\""}, {"dispensed_water_ml", "120"},
        {"feeding_context_profile_version", "0"}, {"feeding_context_profile_version", "2147483648"},
        {"water_ml", "29"}, {"water_ml", "501"}, {"temp", "34"}, {"temp", "61"},
        {"powder_g_per_100ml", "0.99"}, {"powder_g_per_100ml", "50.01"},
        {"device_id", "\"Other\""}, {"device_id", "\"D\\n\""}, {"device_id", "\"\""},
        {"command_id", "\"\""}, {"baby_id", "\"\""},
        {"device_id", quote(std::string(65, 'D'))}, {"command_id", quote(std::string(129, 'C'))},
        {"baby_id", quote(std::string(97, 'B'))},
        {"command_id", "\"a\\u0000b\""}, {"baby_id", "\"a\\u0000b\""},
        {"unknown", "1"}, {"completed", "true"}, {"reason", "\"\""}, {"error_code", "\"\""}};
    for (size_t i = 0; i < bad.size(); ++i) scenario("terminal bad value " + std::to_string(i), [&] {
        rejectTerminal(wire(object(replace(base, bad[i].first, bad[i].second))));
    });
    scenario("failure reason required, optional error typed and bounded", [] {
        const auto f = fields(event(v4::Source::CloudCommand, false));
        rejectTerminal(wire(object(without(f, "reason"))));
        for (const std::string token : {"\"\"", "null", "false", "1", "[]"})
            rejectTerminal(wire(object(replace(f, "reason", token))));
        for (const std::string token : {"null", "false", "1", "[]"})
            rejectTerminal(wire(object(replace(f, "error_code", token))));
        rejectTerminal(wire(object(replace(f, "reason", quote(std::string(65, 'r'))))));
        rejectTerminal(wire(object(replace(f, "error_code", quote(std::string(65, 'E'))))));
    });
    scenario("local forbids command_seq, recovers sequence from event", [] {
        const auto t = event(v4::Source::LocalTouch, true, v4::kMaxSequence);
        rejectTerminal(wire(object(replace(fields(t), "command_seq", quote(std::to_string(v4::kMaxSequence))))));
        rejectTerminal(wire(object(replace(fields(t), "command_id", "\"local-wrong-42\""))));
        auto decoded = event();
        CHECK(decodeTerminalEvent(wire(object(fields(t))), pairing(), decoded));
        CHECK(decoded.request.sequence == v4::kMaxSequence && same(t, decoded));
    });
    for (const std::string token : {"0", "42", "true", "\"0\"", "\"042\"", "\"+42\"", "\"-42\"",
                                  "\"42.0\"", "\"4.2e1\"", "\" 42\"", "\"42 \"", "\"\"",
                                  "\"9223372036854775808\"", "\"43\""})
        scenario("cloud sequence " + token, [&] {
            rejectTerminal(wire(object(replace(base, "command_seq", token))));
        });
}
std::vector<std::string> badEventIds() {
    const std::string prefix = "evt-" + epoch;
    return {"", "legacy-event", "EVT-" + epoch + "-c-42", "evt-" + std::string(32, '0') + "-c-42",
        "evt-" + std::string(32, 'A') + "-c-42", "evt-" + std::string(32, 'g') + "-c-42",
        "evt-" + std::string(31, 'a') + "-c-42", "evt-" + std::string(33, 'a') + "-c-42",
        prefix + "-x-42", prefix + "-C-42", prefix + "-c-0", prefix + "-c-042",
        prefix + "-c-+42", prefix + "-c--42", prefix + "-c-42.0", prefix + "-c-4e1",
        prefix + "-c-9223372036854775808", prefix + "-c-18446744073709551615",
        prefix + "-c-", prefix + "-c-42 ", prefix + "-c-42-extra", prefix + "-c- 42"};
}
void receipts() {
    const auto base = fields(receipt());
    for (const auto& field : base) scenario("receipt field " + field.first, [&] {
        rejectReceipt(wire(object(without(base, field.first)), v4::Kind::CloudReceipt));
        auto dup = base;
        dup.push_back(field);
        rejectReceipt(wire(object(dup), v4::Kind::CloudReceipt));
        for (const std::string token : {"null", "true", "false", "1", "[]", "{}"})
            rejectReceipt(wire(object(replace(base, field.first, token)), v4::Kind::CloudReceipt));
    });
    for (const std::string status : {"received", "published", "accepted", "unchanged", "legacy_unverified",
                                    "failed", "STORED", "", "stored "})
        scenario("receipt status " + status, [&] {
            rejectReceipt(wire(object(replace(base, "status", quote(status))), v4::Kind::CloudReceipt));
        });
    const Fields bad = {{"type", "\"feeding_completed\""}, {"type", "\"feeding_event_receipt \""},
        {"device_id", "\"Other\""}, {"device_id", "\"\""}, {"device_id", "\"-bad\""},
        {"device_id", "\"D\\t\""}, {"device_id", quote(std::string(65, 'D'))},
        {"device_id", "\"D\\u0000\""}, {"event_id", "\"evt-\\u0000\""}, {"extra", "1"}};
    for (size_t i = 0; i < bad.size(); ++i) scenario("receipt bad value " + std::to_string(i), [&] {
        rejectReceipt(wire(object(replace(base, bad[i].first, bad[i].second)), v4::Kind::CloudReceipt));
    });
    for (const auto& id : badEventIds()) scenario("noncanonical event ID " + id, [&] {
        auto r = receipt();
        auto t = event();
        if (id.size() < sizeof(r.eventId)) {
            set(r.eventId, id);
            rejectEncode(r);
            set(t.eventId, id);
            rejectEncode(pairing(), t);
        }
        rejectReceipt(wire(object(replace(fields(r), "event_id", quote(id))), v4::Kind::CloudReceipt));
        rejectTerminal(wire(object(replace(fields(t), "event_id", quote(id)))));
    });
    scenario("receipt encode invalid and unterminated identities", [] {
        auto r = receipt();
        set(r.deviceId, "D\n");
        rejectEncode(r);
        r = receipt();
        std::memset(r.deviceId, 'D', sizeof(r.deviceId));
        rejectEncode(r);
        r = receipt();
        std::memset(r.eventId, 'e', sizeof(r.eventId));
        rejectEncode(r);
        r = receipt();
        set(r.deviceId, std::string("\xff", 1));
        rejectEncode(r);
    });
    scenario("receipt expected identity null/empty/invalid/mismatch", [&] {
        const auto m = wire(object(base), v4::Kind::CloudReceipt);
        rejectReceipt(m, nullptr);
        for (const std::string& expected : std::vector<std::string>{"", "Other", "D\n", "-bad", std::string(65, 'D')})
            rejectReceipt(m, expected.c_str());
    });
}
void malformed() {
    const auto terminal = object(fields(event()));
    const auto received = object(fields(receipt()));
    const std::vector<std::string> broken = {"", "[]", "null", "true", "{}", "{", "{\"x\":1,}",
        "{'event':'feeding_completed'}", "/* comment */{}", "{\"x\":01}", "{\"x\":+1}"};
    for (const auto& text : broken) scenario("malformed root " + text, [&] {
        rejectTerminal(wire(text));
        rejectReceipt(wire(text, v4::Kind::CloudReceipt));
    });
    scenario("truncation each byte", [&] {
        for (size_t i = 0; i < terminal.size(); ++i) rejectTerminal(wire(terminal.substr(0, i)));
        for (size_t i = 0; i < received.size(); ++i)
            rejectReceipt(wire(received.substr(0, i), v4::Kind::CloudReceipt));
    });
    scenario("trailing garbage and embedded NUL", [&] {
        for (const std::string& suffix : std::vector<std::string>{"x", "{}", "[]", std::string(1, '\0')}) {
            rejectTerminal(wire(terminal + suffix));
            rejectReceipt(wire(received + suffix, v4::Kind::CloudReceipt));
        }
    });
    scenario("duplicate escaped decoded keys", [&] {
        auto t = terminal;
        t.insert(t.size() - 1, ",\"event\\u005fid\":" + quote(event().eventId));
        rejectTerminal(wire(t));
        auto r = received;
        r.insert(r.size() - 1, ",\"sta\\u0074us\":\"stored\"");
        rejectReceipt(wire(r, v4::Kind::CloudReceipt));
    });
    const std::vector<std::string> badUtf8 = {std::string("\x80", 1), std::string("\xc0\xaf", 2),
        std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4), std::string("\xe2\x82", 2)};
    for (size_t i = 0; i < badUtf8.size(); ++i) scenario("invalid UTF8 " + std::to_string(i), [&] {
        auto f = fields(event());
        rejectTerminal(wire(object(replace(f, "command_id", quote(badUtf8[i])))));
        rejectTerminal(wire(object(replace(f, "baby_id", quote(badUtf8[i])))));
        rejectReceipt(wire(object(replace(fields(receipt()), "device_id", quote(badUtf8[i]))),
                           v4::Kind::CloudReceipt));
    });
    scenario("raw controls and invalid Unicode escapes", [] {
        for (const std::string& token : std::vector<std::string>{std::string("\"a\x01\"", 4), "\"\\u0000\"",
                                       "\"\\uD800\"", "\"\\uDC00\"", "\"\\x41\""}) {
            rejectTerminal(wire(object(replace(fields(event()), "command_id", token))));
            rejectReceipt(wire(object(replace(fields(receipt()), "event_id", token)), v4::Kind::CloudReceipt));
        }
    });
    scenario("wrong wire kinds", [&] {
        for (unsigned kind = 0; kind <= 21; ++kind) {
            if (kind != 12) rejectTerminal(wire(terminal, v4::Kind(kind)));
            if (kind != 13) rejectReceipt(wire(received, v4::Kind(kind)));
        }
    });
    scenario("invalid Message lengths and raw null", [&] {
        auto t = wire(terminal), r = wire(received, v4::Kind::CloudReceipt);
        for (uint16_t length : {uint16_t(0), uint16_t(2048), uint16_t(UINT16_MAX)}) {
            t.length = r.length = length;
            rejectTerminal(t);
            rejectReceipt(r);
        }
        rejectReceiptBytes(nullptr, 0);
        rejectReceiptBytes(nullptr, 1);
        rejectReceiptBytes(nullptr, 2048);
    });
    scenario("reordered and whitespace fixtures remain semantic", [&] {
        auto f = fields(event());
        std::reverse(f.begin(), f.end());
        auto t = event(v4::Source::LocalTouch, false);
        CHECK(decodeTerminalEvent(wire(" \n\t" + object(f) + "\r\n"), pairing(), t));
        CHECK(same(t, event()));
        f = fields(receipt());
        std::reverse(f.begin(), f.end());
        auto r = receipt(v4::Source::LocalTouch, 1);
        CHECK(decodeCloudReceipt(wire(" \n" + object(f) + "\t", v4::Kind::CloudReceipt), "Babytech_01", r));
        CHECK(same(r, receipt()));
    });
}

int emitFixtures(bool simulation = false) {
    try {
        auto p = pairing();
        set(p.deviceId, "bt-receipt");
        Fields fixtures;
        for (auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch}) {
            for (bool completed : {true, false}) {
                const uint64_t seq = completed ? 1 : 2;
                auto t = event(source, completed, seq);
                set(t.request.deviceId, p.deviceId);
                set(t.request.babyId, "baby-original");
                if (source == v4::Source::CloudCommand)
                    set(t.request.commandId, "v4-event-codec-cloud-" + std::to_string(seq));
                auto encoded = roundtrip(p, t);
                if (simulation) {
                    CHECK(encodeBrainSimulationEvent(p, t, encoded));
                    TerminalEvent rejected;
                    CHECK(!decodeTerminalEvent(encoded, p, rejected));
                }
                fixtures.emplace_back(std::string(source == v4::Source::CloudCommand ? "cloud_" : "local_") +
                    (completed ? "completed" : "failed"), json(encoded));
            }
        }
        // Embed the production bytes verbatim; no fixture JSON stands in for the codec.
        std::printf("%s\n", object(fixtures).c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Fixture production encode/decode failed: %s\n", error.what());
        return 1;
    }
}

int receiptFixtures(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) { std::fprintf(stderr, "Cannot open receipt JSONL: %s\n", path); return 2; }
    unsigned lines = 0, accepted = 0;
    std::string text;
    while (std::getline(input, text)) {
        ++lines;
        if (!text.empty() && text.back() == '\r') text.pop_back();
        scenario("Cloud actual receipt line " + std::to_string(lines), [&] {
            const auto bytes = reinterpret_cast<const uint8_t*>(text.data());
            const auto before = text;
            auto r = receipt(v4::Source::LocalTouch, v4::kMaxSequence);
            CHECK(decodeCloudReceipt(bytes, text.size(), "bt-receipt", r));
            CHECK(text == before && !std::strcmp(r.deviceId, "bt-receipt"));
            const auto m = wire(text, v4::Kind::CloudReceipt);
            const auto beforeM = raw(m);
            CloudReceipt decoded;
            CHECK(decodeCloudReceipt(m, "bt-receipt", decoded) && same(r, decoded));
            CHECK(raw(m) == beforeM);
            ++accepted;
        });
    }
    if (input.bad()) { std::fprintf(stderr, "Receipt JSONL read failed\n"); return 2; }
    if (!lines) { std::fprintf(stderr, "Receipt JSONL is empty; no decoding verified\n"); return 2; }
    std::printf("Cloud receipt fixture: %u lines, %u accepted by both decoder overloads, "
                "%u failed, %u checks\n", lines, accepted, failures, checks);
    return failures ? 1 : 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && !std::strcmp(argv[1], "--emit-fixture")) return emitFixtures();
    if (argc == 2 && !std::strcmp(argv[1], "--emit-simulation-fixture")) return emitFixtures(true);
    if (argc == 3 && !std::strcmp(argv[1], "--receipt-fixture")) return receiptFixtures(argv[2]);
    const std::vector<std::pair<const char*, std::function<void()>>> groups = {
        {"valid", validEvents}, {"boundaries", boundaries}, {"floats", randomFloats},
        {"encode", invalidEncoding}, {"fields", terminalFields}, {"receipts", receipts},
        {"malformed", malformed}};
    if (argc > 2) { std::fprintf(stderr, "Expected at most one test group\n"); return 2; }
    bool found = argc == 1;
    for (const auto& group : groups) {
        if (argc > 1 && std::strcmp(argv[1], group.first)) continue;
        found = true;
        const auto before = failures;
        group.second();
        std::printf("%s: %s\n", group.first, before == failures ? "PASS" : "FAIL");
    }
    if (!found) { std::fprintf(stderr, "Unknown test group\n"); return 2; }
    std::printf("%u scenarios, %u checks, %u byte-stable roundtrips, %u slot extractions, "
                "%u atomic rejections, %u failures\n", scenarios, checks, roundtrips, extracts, rejections, failures);
    return failures ? 1 : 0;
}
