#include "ProductCommandResult.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace babytech::boardlink;
namespace v4 = babytech::v4;

namespace {
unsigned scenarios = 0, failures = 0, roundtrips = 0, rejections = 0;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + \
    std::to_string(__LINE__) + ": " #x); } while (false)
const std::array<const char*, 5> keys{{"command_id", "source", "seq", "accepted", "reason"}};
const std::array<std::string, 5> values{{"\"C\"", "\"cloud_command\"", "\"1\"", "true", "\"accepted\""}};

template <typename T> std::array<unsigned char, sizeof(T)> raw(const T& value) {
    std::array<unsigned char, sizeof(T)> bytes;
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}
template <size_t N> void set(char (&output)[N], const std::string& value) {
    CHECK(value.size() < N);
    std::memset(output, 0, N);
    std::memcpy(output, value.data(), value.size());
}
CommandResult result(v4::Source source = v4::Source::CloudCommand, uint64_t sequence = 1,
                     bool accepted = true, const char* reason = "accepted") {
    CommandResult r;
    r.source = source;
    r.sequence = sequence;
    set(r.commandId, "C");
    r.accepted = accepted;
    set(r.reason, reason);
    return r;
}
std::string object(const std::array<std::string, 5>& fields = values) {
    std::string text = "{";
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i) text += ',';
        text += '"' + std::string(keys[i]) + "\":" + fields[i];
    }
    return text + '}';
}
std::string changed(size_t field, const std::string& value) {
    auto fields = values;
    fields[field] = value;
    return object(fields);
}
v4::Message wire(const std::string& text) {
    CHECK(text.size() <= v4::kMaxMessage);
    v4::Message m;
    m.kind = v4::Kind::CommandResult;
    m.senderBoot = 11;
    m.receiverBoot = 22;
    m.messageId = 33;
    m.length = uint16_t(text.size());
    std::memcpy(m.payload, text.data(), text.size());
    return m;
}
std::string json(const v4::Message& m) {
    return std::string(reinterpret_cast<const char*>(m.payload), m.length);
}
bool same(const CommandResult& a, const CommandResult& b) {
    return a.source == b.source && a.sequence == b.sequence && a.accepted == b.accepted &&
        !std::strcmp(a.commandId, b.commandId) && !std::strcmp(a.reason, b.reason);
}
void rejectEncode(const CommandResult& r) {
    auto output = wire("sentinel");
    std::memset(output.payload, 0xa5, sizeof(output.payload));
    const auto before = raw(output);
    CHECK(!encodeCommandResult(r, output) && raw(output) == before);
    ++rejections;
}
void rejectDecode(const v4::Message& m) {
    auto output = result(v4::Source::LocalTouch, 987, false, "sentinel_9");
    set(output.commandId, "unchanged_id");
    const auto before = raw(output);
    const auto input = raw(m);
    if (decodeCommandResult(m, output))
        throw std::runtime_error("Unexpected acceptance: " + json(m));
    CHECK(raw(output) == before && raw(m) == input);
    ++rejections;
}
void rejectDecode(const std::string& text) { rejectDecode(wire(text)); }
void roundtrip(const CommandResult& r) {
    auto m = wire("sentinel");
    std::memset(m.payload, 0xa5, sizeof(m.payload));
    const auto input = raw(r);
    CHECK(encodeCommandResult(r, m) && raw(r) == input);
    CHECK(uint8_t(m.kind) == 9 && m.length && m.length <= v4::kMaxMessage);
    CHECK(!m.senderBoot && !m.receiverBoot && !m.messageId);
    for (size_t i = 0; i < m.length; ++i) CHECK(m.payload[i] >= 0x20);
    for (size_t i = m.length; i < sizeof(m.payload); ++i) CHECK(m.payload[i] == 0);
    CHECK(json(m).find("\"device\":") == std::string::npos);
    CHECK(json(m).find("\"device_id\":") == std::string::npos);
    CommandResult decoded;
    CHECK(decodeCommandResult(m, decoded) && same(r, decoded));
    v4::Message again;
    CHECK(encodeCommandResult(decoded, again) && json(again) == json(m));
    ++roundtrips;
}
void scenario(const char* name, const std::function<void()>& run) {
    ++scenarios;
    try { run(); }
    catch (const std::exception& e) {
        ++failures;
        std::fprintf(stderr, "FAIL %s: %s\n", name, e.what());
    }
}
void validResults() {
    scenario("accept reject already_clear both source domains and sequence boundaries", [] {
        for (auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch})
            for (uint64_t seq : {UINT64_C(1), UINT64_C(10), UINT64_C(9007199254740993), v4::kMaxSequence}) {
                roundtrip(result(source, seq));
                roundtrip(result(source, seq, true, "already_clear"));
                for (const char* reason : {"busy", "manual_initialization_required", "storage_fault",
                                           "request_conflict", "cloud_ota_not_supported", "A_z09"})
                    roundtrip(result(source, seq, false, reason));
            }
        v4::Message m;
        CHECK(encodeCommandResult(result(), m) && json(m) == object());
        CommandResult output;
        const auto reordered = wire(" \r\n\t{\"reason\":\"already_clear\",\"accepted\":true,"
            "\"seq\":\"9223372036854775807\",\"source\":\"local_touch\",\"command_id\":\"C\"} \t");
        CHECK(decodeCommandResult(reordered, output));
        CHECK(same(output, result(v4::Source::LocalTouch, v4::kMaxSequence, true, "already_clear")));
        auto escaped = object();
        escaped.replace(escaped.find("command_id"), 10, "command_\\u0069d");
        escaped.replace(escaped.find("cloud_command"), 13, "cloud_\\u0063ommand");
        escaped.replace(escaped.find("\"seq\":\"1\""), 9, "\"seq\":\"\\u0031\"");
        CHECK(decodeCommandResult(wire(escaped), output) && same(output, result()));
    });
}
void identityBoundaries() {
    scenario("byte bounded UTF8 IDs quotes slashes controls and max reason", [] {
        std::string unicode;
        for (unsigned i = 0; i < 32; ++i) unicode += "\xf0\x9f\x98\x80";
        for (const auto& id : {std::string("x"), std::string("true"), std::string("123"), std::string("device"),
                               std::string(128, 'a'), std::string(128, '"'),
                               std::string(128, '\\'), unicode, std::string("\xe5\xa5\xb6\xe7\xb2\x89")}) {
            auto r = result(v4::Source::LocalTouch, v4::kMaxSequence, false, "busy");
            set(r.commandId, id);
            set(r.reason, std::string(64, 'R'));
            roundtrip(r);
        }
        for (unsigned c = 1; c < 0x20; ++c) {
            auto r = result();
            set(r.commandId, std::string(128, char(c)));
            roundtrip(r);
        }
        auto r = result();
        std::string controls;
        for (unsigned c = 1; c < 0x20; ++c) controls += char(c);
        controls += "\"\\/\x7f";
        set(r.commandId, controls);
        roundtrip(r);
        v4::Message m;
        CHECK(encodeCommandResult(r, m));
        CHECK(json(m).find("\\u0001") != std::string::npos);
        CommandResult output;
        std::string escaped;
        for (unsigned i = 0; i < 32; ++i) escaped += "\\ud83d\\ude00";
        CHECK(decodeCommandResult(wire(changed(0, '"' + escaped + '"')), output));
        CHECK(std::string(output.commandId) == unicode);
        roundtrip(output);
        rejectDecode(changed(0, '"' + escaped + "a\""));
        escaped.clear();
        for (unsigned i = 0; i < 128; ++i) escaped += "\\u0041";
        CHECK(decodeCommandResult(wire(changed(0, '"' + escaped + '"')), output));
        CHECK(std::strlen(output.commandId) == 128);
        rejectDecode(changed(0, '"' + escaped + "a\""));
        CHECK(decodeCommandResult(wire(changed(0, "\"\\u0001\\b\\f\\n\\r\\t\\\"\\\\\\/\"")), output));
        CHECK(std::string(output.commandId) == "\x01\b\f\n\r\t\"\\/");
        roundtrip(output);
        rejectDecode(changed(0, '"' + std::string(129, 'a') + '"'));
        rejectDecode(changed(4, '"' + std::string(65, 'R') + '"'));
        // Large decoded strings exhaust the fixed document, not the wire cap.
        for (size_t field : {size_t(0), size_t(1), size_t(2), size_t(4)})
            rejectDecode(changed(field, '"' + std::string(1800, 'a') + '"'));
    });
}
void invalidEncoding() {
    scenario("invalid struct leaves whole Message unchanged", [] {
        rejectEncode(CommandResult{});
        for (auto mutate : std::vector<std::function<void(CommandResult&)>>{
            [](CommandResult& r) { r.source = v4::Source(0); },
            [](CommandResult& r) { r.source = v4::Source(3); },
            [](CommandResult& r) { r.sequence = 0; },
            [](CommandResult& r) { r.sequence = v4::kMaxSequence + 1; },
            [](CommandResult& r) { r.sequence = UINT64_MAX; },
            [](CommandResult& r) { r.commandId[0] = 0; },
            [](CommandResult& r) { std::memset(r.commandId, 'x', sizeof(r.commandId)); },
            [](CommandResult& r) { r.commandId[0] = char(0xff); },
            [](CommandResult& r) { r.reason[0] = 0; },
            [](CommandResult& r) { std::memset(r.reason, 'R', sizeof(r.reason)); },
            [](CommandResult& r) { r.reason[0] = '-'; },
            [](CommandResult& r) { r.reason[0] = char(0xff); },
            [](CommandResult& r) { r.accepted = false; },
            [](CommandResult& r) { set(r.reason, "busy"); }}) {
            auto r = result();
            mutate(r);
            rejectEncode(r);
        }
        for (auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch})
            for (bool accepted : {false, true})
                for (const char* reason : {"accepted", "already_clear", "already_idle", "busy", "Accepted"}) {
                    auto r = result(source, 1, accepted, reason);
                    if (accepted == (!std::strcmp(reason, "accepted") || !std::strcmp(reason, "already_clear")) &&
                        std::strcmp(reason, "already_idle")) roundtrip(r);
                    else rejectEncode(r);
                }
    });
}
void strictFields() {
    scenario("exact five fields duplicates escaped duplicates unknown and wrong types", [] {
        for (size_t i = 0; i < keys.size(); ++i) {
            auto text = object();
            text.insert(text.size() - 1, ",\"" + std::string(keys[i]) + "\":" + values[i]);
            rejectDecode(text);
            text = object();
            text.replace(text.find(keys[i]), std::strlen(keys[i]), "unknown");
            rejectDecode(text);
            text = object();
            const auto start = text.find('"' + std::string(keys[i]) + "\":");
            const auto length = std::strlen(keys[i]) + 3 + values[i].size();
            if (i == 0) text.erase(start, length + 1);
            else text.erase(start - 1, length + 1);
            rejectDecode(text);
            for (const char* value : {"null", "1", "0", "-1", "1.0", "1e0", "true", "false", "{}", "[]", "\"true\""}) {
                if (i == 3 && !std::strcmp(value, "true")) continue;
                if (i == 0 && !std::strcmp(value, "\"true\"")) continue;
                rejectDecode(changed(i, value));
            }
        }
        auto text = object();
        text.insert(text.size() - 1, ",\"device_id\":\"D\"");
        rejectDecode(text);
        text = object();
        text.replace(text.find("reason"), 6, std::string(1800, 'a'));
        rejectDecode(text);
        for (const char* key : {"device", "device_id", "outcome", "status", "request_digest"}) {
            text = object();
            text.replace(text.find("reason"), 6, key);
            rejectDecode(text);
        }
        // Five raw entries but only four decoded keys must fail too.
        text = object();
        text.replace(text.find("reason"), 6, "s\\u0065q");
        rejectDecode(text);
        text = object();
        text.insert(text.size() - 1, ",\"s\\u0065q\":\"1\"");
        rejectDecode(text);
        for (const char* source : {"", "cloud", "local", "Cloud_command", "local_touch ", "stop", "cloud_commandX"})
            rejectDecode(changed(1, '"' + std::string(source) + '"'));
    });
}
void sequencesAndReasons() {
    scenario("canonical decimal sequences and accepted reason consistency", [] {
        for (const char* seq : {"", "0", "00", "01", "-1", "+1", "1.0", "1e0", " 1", "1 ", "1\t",
                               "9223372036854775808", "9999999999999999999", "18446744073709551615",
                               "10000000000000000000", "123456789012345678901", "1a", "0x1", "NaN", "Infinity"})
            rejectDecode(changed(2, '"' + std::string(seq) + '"'));
        for (bool accepted : {true, false})
            for (const char* reason : {"", "accepted", "already_clear", "already_idle", "busy", "Accepted", "a-b", "a b", "a.b"}) {
                auto fields = values;
                fields[3] = accepted ? "true" : "false";
                fields[4] = '"' + std::string(reason) + '"';
                const bool good = std::strlen(reason) && accepted == (!std::strcmp(reason, "accepted") ||
                    !std::strcmp(reason, "already_clear")) && std::strcmp(reason, "already_idle") &&
                    std::strpbrk(reason, "-. ") == nullptr;
                if (good) {
                    CommandResult output;
                    CHECK(decodeCommandResult(wire(object(fields)), output));
                    roundtrip(output);
                } else rejectDecode(object(fields));
            }
        for (unsigned c = 1; c <= 255; ++c) {
            auto r = result(v4::Source::LocalTouch, 1, false, "R");
            set(r.reason, std::string(1, char(c)));
            const bool good = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_';
            char escaped[9];
            std::snprintf(escaped, sizeof(escaped), "\"\\u%04x\"", c);
            auto fields = values;
            fields[3] = "false";
            fields[4] = escaped;
            if (good) {
                roundtrip(r);
                CommandResult output;
                CHECK(decodeCommandResult(wire(object(fields)), output) && !std::strcmp(r.reason, output.reason));
            } else { rejectEncode(r); rejectDecode(object(fields)); }
        }
    });
}
void malformed() {
    scenario("strict JSON grammar nesting truncation and UTF8 NUL failures", [] {
        for (const char* text : {"", "{}", "[]", "null", "true", "{command_id:'C'}", "{}{}", "/*x*/{}"})
            rejectDecode(text);
        const auto good = object();
        for (size_t n = 0; n < good.size(); ++n) rejectDecode(good.substr(0, n));
        for (const char* suffix : {"x", "{}", "[]", "/*x*/"}) rejectDecode(good + suffix);
        auto text = good;
        text.insert(text.size() - 1, ",");
        rejectDecode(text);
        text = good;
        text.replace(text.find("true"), 4, "True");
        rejectDecode(text);
        text = good;
        text.erase(text.find(",\"source\""), 1);
        rejectDecode(text);
        for (const char* id : {"", "\\u0000", "A\\u0000B", "\\ud800", "\\udc00", "\\ud800\\u0041",
                              "\\ud800\\ud800", "\\u00xz", "\\x01", "\\v", "\\"})
            rejectDecode(changed(0, '"' + std::string(id) + '"'));
        for (const auto& id : std::vector<std::string>{std::string("A\0B", 3), "\x01", "\xc0\xaf", "\xc1\xbf",
                "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82", "\x80", "\xff"}) {
            rejectDecode(changed(0, '"' + id + '"'));
            auto r = result();
            // C-string NUL terminates the struct; embedded wire NUL is tested above.
            if (id.find('\0') == std::string::npos && id != "\x01") {
                set(r.commandId, id);
                rejectEncode(r);
            }
        }
        for (size_t i = 0; i < keys.size(); ++i) rejectDecode(changed(i, "\"A\\u0000B\""));
        rejectDecode(good + std::string(1, '\0'));
    });
    scenario("kind and length bounds and maximum valid wire length", [] {
        auto m = wire(object());
        for (unsigned kind = 0; kind <= 21; ++kind) {
            if (kind == 9) continue;
            m.kind = v4::Kind(kind);
            rejectDecode(m);
        }
        m.kind = v4::Kind::CommandResult;
        for (uint16_t length : {uint16_t(0), uint16_t(v4::kMaxMessage + 1), uint16_t(UINT16_MAX)}) {
            m.length = length;
            rejectDecode(m);
        }
        auto good = object();
        good += std::string(v4::kMaxMessage - good.size(), ' ');
        CommandResult output;
        CHECK(decodeCommandResult(wire(good), output) && same(output, result()));
    });
}
}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::pair<const char*, std::function<void()>>> groups = {
        {"valid", validResults}, {"boundaries", identityBoundaries}, {"encode", invalidEncoding},
        {"fields", strictFields}, {"sequences_reasons", sequencesAndReasons}, {"malformed", malformed}};
    if (argc > 2) { std::fprintf(stderr, "Expected at most one test group\n"); return 2; }
    bool found = argc == 1;
    for (const auto& group : groups) {
        if (argc > 1 && std::strcmp(argv[1], group.first)) continue;
        found = true;
        const auto before = failures;
        group.second();
        std::printf("%s: %s\n", group.first, failures == before ? "PASS" : "FAIL");
    }
    if (!found) { std::fprintf(stderr, "Unknown test group\n"); return 2; }
    std::printf("%u scenarios, %u roundtrips, %u atomic rejections, %u failures\n",
                scenarios, roundtrips, rejections, failures);
    return failures ? 1 : 0;
}
