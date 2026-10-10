#include "ProductContextMessages.h"
#include "FakeProductCrypto.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace babytech::boardlink;
namespace v4 = babytech::v4;

// Target only checked Message/scratch allocations, not JSON pools or the harness.
namespace { bool failMessage = false, failScratch = false; }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (failMessage) return nullptr;
    try { return ::operator new(size); }
    catch (const std::bad_alloc&) { return nullptr; }
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept { ::operator delete(pointer); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    if (failScratch) return nullptr;
    try { return ::operator new[](size); }
    catch (const std::bad_alloc&) { return nullptr; }
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

namespace {
unsigned scenarios = 0, failures = 0, roundtrips = 0, rejections = 0, matches = 0, frames = 0;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + \
    std::to_string(__LINE__) + ": " #x); } while (false)
const std::array<const char*, 6> keys{{"reply_to", "device_id", "profile_version", "cleared",
                                    "context_digest", "status"}};
const std::string hex = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
const std::array<std::string, 6> values{{"1", "\"D\"", "7", "false", '"' + hex + '"', "\"stored\""}};
const std::array<ContextStatus, 5> statuses{{ContextStatus::Stored, ContextStatus::Unchanged,
    ContextStatus::Busy, ContextStatus::Conflict, ContextStatus::StorageFault}};

template <typename T> std::array<uint8_t, sizeof(T)> raw(const T& value) {
    std::array<uint8_t, sizeof(T)> output;
    std::memcpy(output.data(), &value, sizeof(value));
    return output;
}
template <size_t N> void set(char (&output)[N], const std::string& value) {
    CHECK(value.size() < N);
    std::memset(output, 0, N);
    std::memcpy(output, value.data(), value.size());
}
ProductContext context(bool cleared = false) {
    ProductContext c;
    set(c.deviceId, "D");
    c.profileVersion = cleared ? 8 : 7;
    c.cleared = cleared;
    if (!cleared) {
        set(c.babyId, "baby-7");
        set(c.babyName, "Ada");
        set(c.formulaBrand, "Friso");
        c.waterMl = 120;
        c.temperatureC = 40;
        c.powderGPer100Ml = 13.5f;
    }
    return c;
}
ContextResult result() {
    ContextResult r;
    r.replyTo = 1;
    set(r.deviceId, "D");
    r.profileVersion = 7;
    r.status = ContextStatus::Stored;
    for (size_t i = 0; i < sizeof(r.digest); ++i) r.digest[i] = uint8_t(i);
    return r;
}
ContextResult resultFor(const ProductContext& c) {
    auto r = result();
    set(r.deviceId, c.deviceId);
    r.profileVersion = c.profileVersion;
    r.cleared = c.cleared;
    CHECK(contextDigest(c, r.digest));
    return r;
}
std::string object(const std::array<std::string, 6>& fields = values) {
    std::string text = "{";
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i) text += ',';
        text += '"' + std::string(keys[i]) + "\":" + fields[i];
    }
    return text + '}';
}
std::string changed(size_t field, const std::string& token) {
    auto fields = values;
    fields[field] = token;
    return object(fields);
}
v4::Message wire(const std::string& text, v4::Kind kind = v4::Kind::ContextResult) {
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
    auto m = wire("sentinel");
    std::memset(m.payload, 0xa5, sizeof(m.payload));
    return m;
}
void envelope(const v4::Message& m, v4::Kind kind) {
    CHECK(m.kind == kind && m.length && m.length <= v4::kMaxMessage);
    CHECK(!m.senderBoot && !m.receiverBoot && !m.messageId);
    CHECK(v4::validUtf8(m.payload, m.length));
    CHECK(std::all_of(m.payload + m.length, m.payload + sizeof(m.payload),
                      [](uint8_t b) { return b == 0; }));
}
std::vector<uint8_t> identity(const ProductContext& c) {
    std::array<uint8_t, kContextIdentityMaxSize> bytes;
    const auto length = encodeContextIdentity(c, bytes.data(), bytes.size());
    CHECK(length);
    return {bytes.begin(), bytes.begin() + length};
}
bool same(const ContextResult& a, const ContextResult& b) {
    return a.replyTo == b.replyTo && !std::strcmp(a.deviceId, b.deviceId) &&
        a.profileVersion == b.profileVersion && a.cleared == b.cleared &&
        a.status == b.status && !std::memcmp(a.digest, b.digest, sizeof(a.digest));
}
void matching(const ContextResult& r, const ProductContext& c, bool expected) {
    const auto beforeR = raw(r);
    const auto beforeC = raw(c);
    CHECK(matchesContextResult(r, c) == expected);
    CHECK(raw(r) == beforeR && raw(c) == beforeC);
    ++matches;
}
v4::Message roundtrip(const ProductContext& c) {
    auto m = sentinel();
    const auto before = raw(c);
    CHECK(encodeContextMessage(c, m) && raw(c) == before);
    envelope(m, v4::Kind::Context);
    ProductContext decoded = context(!c.cleared);
    const auto input = raw(m);
    CHECK(decodeContextMessage(m, c.deviceId, decoded));
    CHECK(raw(m) == input && sameProductContext(c, decoded));
    CHECK(identity(c) == identity(decoded));
    matching(resultFor(c), decoded, true);
    auto again = sentinel();
    CHECK(encodeContextMessage(decoded, again) && json(again) == json(m));
    ++roundtrips;
    return m;
}
v4::Message roundtrip(const ContextResult& r) {
    auto m = sentinel();
    const auto before = raw(r);
    CHECK(encodeContextResult(r, m) && raw(r) == before);
    envelope(m, v4::Kind::ContextResult);
    auto decoded = result();
    const auto input = raw(m);
    CHECK(decodeContextResult(m, decoded) && same(r, decoded) && raw(m) == input);
    auto again = sentinel();
    CHECK(encodeContextResult(decoded, again) && json(again) == json(m));
    ++roundtrips;
    return m;
}
void rejectEncode(const ProductContext& c) {
    auto output = sentinel();
    const auto before = raw(output);
    const auto input = raw(c);
    CHECK(!encodeContextMessage(c, output) && raw(output) == before && raw(c) == input);
    ++rejections;
}
void rejectEncode(const ContextResult& r) {
    auto output = sentinel();
    const auto before = raw(output);
    const auto input = raw(r);
    CHECK(!encodeContextResult(r, output) && raw(output) == before && raw(r) == input);
    ++rejections;
}
void rejectResult(const v4::Message& m) {
    for (bool cleared : {false, true}) {
        auto output = resultFor(context(cleared));
        output.replyTo = 123;
        output.status = ContextStatus::Conflict;
        const auto before = raw(output);
        const auto input = raw(m);
        CHECK(!decodeContextResult(m, output) && raw(output) == before && raw(m) == input);
        ++rejections;
    }
}
void rejectResult(const std::string& text) { rejectResult(wire(text)); }
void rejectContext(const v4::Message& m, const char* expected = "D") {
    for (bool cleared : {false, true}) {
        auto output = context(cleared);
        const auto before = raw(output);
        const auto input = raw(m);
        CHECK(!decodeContextMessage(m, expected, output) && raw(output) == before && raw(m) == input);
        ++rejections;
    }
}
void scenario(const char* name, const std::function<void()>& run) {
    ++scenarios;
    try { run(); }
    catch (const std::exception& e) {
        ++failures;
        std::fprintf(stderr, "FAIL %s: %s\n", name, e.what());
    }
    failMessage = failScratch = false;
    fake_product_crypto::reset();
}

void fullContext() {
    scenario("full UTF8 byte limits controls and empty display fields", [] {
        auto c = context();
        roundtrip(c);
        set(c.deviceId, std::string(64, 'D'));
        c.profileVersion = INT32_MAX;
        c.waterMl = 500;
        c.temperatureC = 60;
        for (const auto& unit : std::vector<std::string>{"a", "\xc3\xa9", "\xe5\xa5\xb6",
                "\xf0\x9f\x98\x80", "\"", "\\", "\n"}) {
            auto repeated = [&unit](size_t capacity) {
                std::string s;
                for (size_t i = 0; i < capacity / unit.size(); ++i) s += unit;
                return s;
            };
            set(c.babyId, repeated(96));
            set(c.babyName, repeated(320));
            set(c.formulaBrand, repeated(480));
            roundtrip(c);
        }
        c = context();
        std::string controls;
        for (unsigned byte = 1; byte < 32; ++byte) controls += char(byte);
        controls += "\"\\/\x7f";
        set(c.babyId, controls);
        set(c.babyName, controls + "\xf0\x9f\x98\x80");
        set(c.formulaBrand, controls + "\xe5\xa5\xb6");
        const auto m = roundtrip(c);
        CHECK(json(m).find("\\u0001") != std::string::npos);
        set(c.babyName, "");
        set(c.formulaBrand, "");
        roundtrip(c);
        c = context();
        c.waterMl = 30;
        c.temperatureC = 35;
        c.profileVersion = 1;
        roundtrip(c);
    });
    scenario("full semantics order escaping metadata and legacy defaults", [] {
        const auto m = wire(R"( {"temp":40,"powder_g_per_100ml":13.500000,"baby_name":"\u0041da","water_ml":120,"baby_id":"baby-7","profile_version":7,"formula_brand":"Friso","device_id":"D","cleared":false,"type":"feeding_context","updated_at":"ignored"} )", v4::Kind::Context);
        auto c = context(true);
        CHECK(decodeContextMessage(m, "D", c) && identity(c) == identity(context()));
        matching(resultFor(context()), c, true);
        roundtrip(c);
        const auto legacy = wire(R"({"type":"feeding_context","device_id":"D","profile_version":7,"baby_id":"baby-7","water_ml":120,"temp":40,"powder_g_per_100ml":13.5})", v4::Kind::Context);
        CHECK(decodeContextMessage(legacy, "D", c));
        CHECK(!c.babyName[0] && !std::strcmp(c.formulaBrand, "Friso"));
        roundtrip(c);
    });
    scenario("versioned tombstones contain no baby or recipe fields", [] {
        for (uint32_t version : {1u, 8u, uint32_t(INT32_MAX)}) {
            auto c = context(true);
            c.profileVersion = version;
            const auto m = roundtrip(c);
            CHECK(json(m) == "{\"type\":\"feeding_context\",\"device_id\":\"D\",\"profile_version\":" +
                  std::to_string(version) + ",\"cleared\":true}");
            auto old = context();
            CHECK(decodeContextMessage(m, "D", old) && identity(old) == identity(c));
            for (auto status : statuses) {
                auto r = resultFor(c);
                r.status = status;
                const auto reply = roundtrip(r);
                ContextResult decoded;
                CHECK(decodeContextResult(reply, decoded));
                matching(decoded, old, true);
                matching(decoded, context(), false);
            }
        }
    });
}

void floatSemantics() {
    scenario("binary32 boundaries and deterministic stratified samples", [] {
        auto c = context();
        auto checkBits = [&c](uint32_t bits) {
            std::memcpy(&c.powderGPer100Ml, &bits, sizeof(bits));
            roundtrip(c);
        };
        constexpr uint32_t first = 0x3f800000, last = 0x42480000;
        for (uint32_t exponent = 127; exponent <= 132; ++exponent) {
            const uint32_t base = exponent << 23;
            for (uint32_t delta : {0u, 1u, 2u, 0x3fffffu, 0x400000u, 0x7ffffeu, 0x7fffffu}) {
                if (base + delta <= last) checkBits(base + delta);
                if (base >= first + delta) checkBits(base - delta);
            }
            for (unsigned bit = 0; bit < 23; ++bit)
                if (base + (1u << bit) <= last) checkBits(base + (1u << bit));
        }
        checkBits(first); checkBits(last - 1); checkBits(last);
        uint32_t random = 0x5eed1234;
        constexpr uint32_t samples = 4096;
        for (uint32_t i = 0; i < samples; ++i) {
            checkBits(first + uint32_t(uint64_t(last - first) * i / (samples - 1)));
            random = random * 1664525u + 1013904223u;
            checkBits(first + random % (last - first + 1));
        }
    });
}

void validResults() {
    scenario("six field canonical output all statuses ranges and digest bytes", [] {
        CHECK(json(roundtrip(result())) == object());
        for (auto status : statuses)
            for (bool cleared : {false, true})
                for (uint32_t reply : {1u, uint32_t(INT32_MAX), UINT32_MAX})
                    for (uint32_t version : {1u, uint32_t(INT32_MAX)}) {
                        auto r = result();
                        r.replyTo = reply;
                        r.profileVersion = version;
                        r.cleared = cleared;
                        r.status = status;
                        set(r.deviceId, std::string(64, 'D'));
                        for (size_t i = 0; i < sizeof(r.digest); ++i) r.digest[i] = uint8_t(255 - i);
                        roundtrip(r);
                    }
        for (unsigned byte = 0; byte <= 255; ++byte) {
            auto r = result();
            std::memset(r.digest, byte, sizeof(r.digest));
            roundtrip(r);
        }
        const auto text = std::string(" \r\n\t") + R"({"status":"stored","context_digest":")" + hex +
            R"(","cleared":false,"profile_version":7,"device_id":"\u0044","reply_\u0074o":1} )";
        ContextResult r;
        CHECK(decodeContextResult(wire(text), r) && same(r, result()));
        auto escaped = object();
        escaped.replace(escaped.find(hex), 1, "\\u0030");
        CHECK(decodeContextResult(wire(escaped), r) && same(r, result()));
        roundtrip(r);
        auto padded = object();
        padded += std::string(v4::kMaxMessage - padded.size(), ' ');
        CHECK(decodeContextResult(wire(padded), r) && same(r, result()));
    });
}

void strictResults() {
    scenario("required fields unknown duplicate escaped duplicate and strict types", [] {
        for (size_t i = 0; i < keys.size(); ++i) {
            auto text = object();
            const auto name = std::string(keys[i]);
            text.insert(text.size() - 1, ",\"" + name + "\":" + values[i]);
            rejectResult(text);
            text = object();
            text.replace(text.find(name), name.size(), "unknown");
            rejectResult(text);
            text = object();
            const auto start = text.find('"' + name + "\":");
            const auto length = name.size() + 3 + values[i].size();
            if (!i) text.erase(start, length + 1);
            else text.erase(start - 1, length + 1);
            rejectResult(text);
            // Six raw entries, five decoded entries, including escaped aliases.
            text = object();
            text.replace(text.find(name), name.size(), i ? "reply_\\u0074o" : "device_\\u0069d");
            rejectResult(text);
            for (const char* token : {"null", "0", "-1", "1.0", "1e0", "true", "false", "{}", "[]", "\"1\""}) {
                if (i == 3 && (!std::strcmp(token, "true") || !std::strcmp(token, "false"))) continue;
                if (i == 1 && !std::strcmp(token, "\"1\"")) continue;
                rejectResult(changed(i, token));
            }
            rejectResult(changed(i, "\"A\\u0000B\""));
        }
        auto text = object();
        text.insert(text.size() - 1, ",\"reply_\\u0074o\":1");
        rejectResult(text);
        text = object();
        text.insert(text.size() - 1, ",\"future\":true");
        rejectResult(text);
        for (size_t field : {size_t(0), size_t(2)})
            for (const char* token : {"-0", "00", "01", "+1", "0x1", "1.", ".1", "NaN", "Infinity",
                                     "4294967296", "18446744073709551615", "9999999999999999999999"})
                rejectResult(changed(field, token));
        rejectResult(changed(2, "2147483648"));
        for (const auto& id : std::vector<std::string>{"", "-D", "_D", "D.x", "D x", "D/x", "D:x",
                                                     std::string(65, 'D'), "\xc3\xa9"})
            rejectResult(changed(1, '"' + id + '"'));
        for (const char* status : {"", "Stored", "stored ", "unknown", "already_clear", "storedX", "storage_faultX"})
            rejectResult(changed(5, '"' + std::string(status) + '"'));
        for (size_t field : {size_t(1), size_t(4), size_t(5)})
            rejectResult(changed(field, '"' + std::string(1800, 'a') + '"'));
    });
    scenario("digest exactly 64 decoded lowercase hex bytes", [] {
        for (size_t length : {size_t(0), size_t(1), size_t(63), size_t(65), size_t(128)})
            rejectResult(changed(4, '"' + std::string(length, 'a') + '"'));
        for (size_t at = 0; at < hex.size(); ++at)
            for (char bad : std::string("ABCDEFgG-_: /")) {
                auto changedHex = hex;
                changedHex[at] = bad;
                rejectResult(changed(4, '"' + changedHex + '"'));
            }
        rejectResult(changed(4, '"' + std::string(63, '0') + "\\u0041\""));
    });
    scenario("strict JSON truncation trailing input NUL controls and UTF8", [] {
        for (const char* text : {"", "{}", "[]", "null", "true", "{reply_to:1}", "{'reply_to':1}", "/*x*/{}"})
            rejectResult(text);
        const auto good = object();
        for (size_t length = 0; length < good.size(); ++length) rejectResult(good.substr(0, length));
        for (const char* suffix : {"x", "{}", "[]", "/*x*/"}) rejectResult(good + suffix);
        auto text = good;
        text.insert(text.size() - 1, ",");
        rejectResult(text);
        text = good;
        text.erase(text.find(",\"device_id\""), 1);
        rejectResult(text);
        for (const char* escape : {"\\u0000", "D\\u0000x", "\\ud800", "\\udc00", "\\ud800\\u0041",
                                  "\\ud800\\ud800", "\\u00xz", "\\x01", "\\v", "\\"})
            rejectResult(changed(1, '"' + std::string(escape) + '"'));
        for (const auto& bad : std::vector<std::string>{std::string("D\0x", 3), "\x01", "\xc0\xaf", "\xc1\xbf",
                "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82", "\x80", "\xff"}) {
            for (size_t field : {size_t(1), size_t(4), size_t(5)}) rejectResult(changed(field, '"' + bad + '"'));
        }
        // NUL and invalid UTF8 cannot hide in ignored bytes, keys or the suffix.
        for (size_t at = 0; at <= good.size(); ++at) {
            auto nul = good;
            nul.insert(at, 1, '\0');
            rejectResult(nul);
        }
        rejectResult(good + "\xff");
        text = good;
        text.replace(text.find("status"), 6, "sta\\u0000tus");
        rejectResult(text);
    });
}

void contextRejections() {
    scenario("context wrapper preserves active and tombstone outputs on invalid input", [] {
        auto c = context();
        auto good = roundtrip(c);
        rejectContext(good, "other");
        rejectContext(good, nullptr);
        rejectContext(good, "");
        rejectContext(good, "-invalid");
        const auto text = json(good);
        for (size_t n = 0; n < text.size(); ++n) rejectContext(wire(text.substr(0, n), v4::Kind::Context));
        for (const char* bad : {"{}", "[]", "null", "{type:'feeding_context'}"})
            rejectContext(wire(bad, v4::Kind::Context));
        for (const char* extra : {",\"device_id\":\"D\"", ",\"device_\\u0069d\":\"D\"", ",\"future\":1"}) {
            auto bad = text;
            bad.insert(bad.size() - 1, extra);
            rejectContext(wire(bad, v4::Kind::Context));
        }
        auto bad = text;
        bad.replace(bad.find("Ada"), 3, "A\\u0000B");
        rejectContext(wire(bad, v4::Kind::Context));
        bad = text;
        bad.replace(bad.find("Ada"), 3, "\xff");
        rejectContext(wire(bad, v4::Kind::Context));
        rejectContext(wire(text + std::string(1, '\0'), v4::Kind::Context));
        auto tombstone = json(roundtrip(context(true)));
        tombstone.insert(tombstone.size() - 1, ",\"baby_id\":\"old\"");
        rejectContext(wire(tombstone, v4::Kind::Context));
        rejectEncode(ProductContext{});
        for (auto mutate : std::vector<std::function<void(ProductContext&)>>{
            [](ProductContext& x) { x.profileVersion = 0; },
            [](ProductContext& x) { x.profileVersion = uint32_t(INT32_MAX) + 1; },
            [](ProductContext& x) { std::memset(x.deviceId, 'D', sizeof(x.deviceId)); },
            [](ProductContext& x) { x.babyId[0] = 0; },
            [](ProductContext& x) { std::memset(x.babyName, 'a', sizeof(x.babyName)); },
            [](ProductContext& x) { x.formulaBrand[0] = char(0xff); },
            [](ProductContext& x) { x.waterMl = 29; },
            [](ProductContext& x) { x.temperatureC = 61; },
            [](ProductContext& x) { x.powderGPer100Ml = std::numeric_limits<float>::quiet_NaN(); },
            [](ProductContext& x) { x.cleared = true; }}) {
            auto invalid = c;
            mutate(invalid);
            rejectEncode(invalid);
        }
    });
}

void envelopesAndFaults() {
    scenario("wrong kind length and success/failure preserve complete envelopes", [] {
        auto cm = roundtrip(context()), rm = roundtrip(result());
        for (unsigned kind = 0; kind <= 255; ++kind) {
            cm.kind = rm.kind = v4::Kind(kind);
            if (cm.kind != v4::Kind::Context) rejectContext(cm);
            if (rm.kind != v4::Kind::ContextResult) rejectResult(rm);
        }
        cm.kind = v4::Kind::Context;
        rm.kind = v4::Kind::ContextResult;
        for (uint16_t n : {uint16_t(0), uint16_t(v4::kMaxMessage + 1), uint16_t(UINT16_MAX)}) {
            cm.length = rm.length = n;
            rejectContext(cm); rejectResult(rm);
        }
        // The codec deliberately does not authorize transport boot IDs/message IDs.
        cm = roundtrip(context());
        rm = roundtrip(result());
        cm.senderBoot = rm.senderBoot = UINT64_MAX;
        cm.receiverBoot = rm.receiverBoot = UINT64_MAX;
        cm.messageId = rm.messageId = UINT32_MAX;
        ProductContext c;
        ContextResult r;
        CHECK(decodeContextMessage(cm, "D", c) && sameProductContext(c, context()));
        CHECK(decodeContextResult(rm, r) && same(r, result()));
        for (auto mutate : std::vector<std::function<void(ContextResult&)>>{
            [](ContextResult& x) { x.replyTo = 0; },
            [](ContextResult& x) { x.profileVersion = 0; },
            [](ContextResult& x) { x.profileVersion = uint32_t(INT32_MAX) + 1; },
            [](ContextResult& x) { x.deviceId[0] = 0; },
            [](ContextResult& x) { std::memset(x.deviceId, 'D', sizeof(x.deviceId)); },
            [](ContextResult& x) { x.deviceId[0] = char(0xff); },
            [](ContextResult& x) { x.deviceId[0] = '-'; },
            [](ContextResult& x) { x.status = ContextStatus(-1); },
            [](ContextResult& x) { x.status = ContextStatus(5); }}) {
            auto invalid = result();
            mutate(invalid);
            rejectEncode(invalid);
        }
    });
    scenario("checked Message scratch allocation failures are atomic", [] {
        failMessage = true;
        rejectEncode(context()); rejectEncode(context(true)); rejectEncode(result());
        failMessage = false;
        failScratch = true;
        rejectEncode(context()); rejectEncode(context(true));
        failScratch = false;
        roundtrip(context()); roundtrip(context(true)); roundtrip(result());
    });
    scenario("fully escaped 2046/2047 succeeds 2048 overflow is atomic", [] {
        auto c = context();
        set(c.deviceId, std::string(64, 'D'));
        set(c.babyId, std::string(96, 'b'));
        set(c.babyName, std::string(320, 'n'));
        set(c.formulaBrand, std::string(480, 'f'));
        const auto baseline = roundtrip(c).length;
        for (size_t target : {v4::kMaxMessage - 1, v4::kMaxMessage, v4::kMaxMessage + 1}) {
            const auto extra = target - baseline;
            std::string brand(480, 'f');
            CHECK(extra / 5 + extra % 5 <= brand.size());
            brand.replace(0, extra / 5, extra / 5, char(1));
            brand.replace(extra / 5, extra % 5, extra % 5, '"');
            set(c.formulaBrand, brand);
            CHECK(validProductContext(c));
            if (target <= v4::kMaxMessage) CHECK(roundtrip(c).length == target);
            else rejectEncode(c);
        }
    });
}

void digestMatching() {
    scenario("independent SHA256 golden fixtures for full context and tombstone", [] {
        const std::array<const char*, 2> golden{{
            "9ee5e1b837d6fe60064f252dabe6ccb50e03f1e34fad194b16e91509df4e9e19",
            "f91d7e6c090738146cbfe7e1db5b9820430c9e76a8343afc3ac1c67970c9decf"}};
        for (size_t i = 0; i < golden.size(); ++i) {
            auto r = resultFor(context(i == 1));
            const auto text = json(roundtrip(r));
            CHECK(text.find('"' + std::string(golden[i]) + '"') != std::string::npos);
            matching(r, context(i == 1), true);
        }
    });
    scenario("match binds every semantic field but does not claim durable success", [] {
        const auto c = context();
        auto r = resultFor(c);
        for (auto status : statuses) {
            r.status = status;
            matching(r, c, true);
        }
        r.replyTo = UINT32_MAX;
        matching(r, c, true); // Caller must separately correlate reply_to/session.
        for (auto mutate : std::vector<std::function<void(ProductContext&)>>{
            [](ProductContext& x) { set(x.deviceId, "other"); },
            [](ProductContext& x) { ++x.profileVersion; },
            [](ProductContext& x) { set(x.babyId, "other"); },
            [](ProductContext& x) { set(x.babyName, "Ada2"); },
            [](ProductContext& x) { set(x.formulaBrand, "Friso2"); },
            [](ProductContext& x) { ++x.waterMl; },
            [](ProductContext& x) { ++x.temperatureC; },
            [](ProductContext& x) { x.powderGPer100Ml = std::nextafter(x.powderGPer100Ml, 50.0f); },
            [](ProductContext& x) { x = context(true); }}) {
            auto changedContext = c;
            mutate(changedContext);
            matching(r, changedContext, false);
        }
        for (size_t i = 0; i < sizeof(r.digest); ++i) {
            auto changedResult = r;
            changedResult.digest[i] ^= 1;
            matching(changedResult, c, false);
        }
        for (auto mutate : std::vector<std::function<void(ContextResult&)>>{
            [](ContextResult& x) { x.replyTo = 0; },
            [](ContextResult& x) { ++x.profileVersion; },
            [](ContextResult& x) { x.cleared = true; },
            [](ContextResult& x) { set(x.deviceId, "other"); },
            [](ContextResult& x) { std::memset(x.deviceId, 'D', sizeof(x.deviceId)); },
            [](ContextResult& x) { x.status = ContextStatus(5); }}) {
            auto changedResult = r;
            mutate(changedResult);
            matching(changedResult, c, false);
        }
        matching(r, ProductContext{}, false);
        fake_product_crypto::reset();
        fake_product_crypto::fail = true;
        matching(r, c, false);
        CHECK(fake_product_crypto::calls == 1);
        fake_product_crypto::reset();
        matching(r, c, true);
    });
}

void frameCodec() {
    scenario("production fragment frame parser assembler carries both kinds", [] {
        auto c = context();
        set(c.babyName, std::string(320, 'n'));
        set(c.formulaBrand, std::string(480, 'f'));
        const std::vector<std::pair<v4::Message, ProductContext>> fixtures{
            {roundtrip(c), c}, {roundtrip(context(true)), context(true)}, {roundtrip(resultFor(c)), c}};
        for (const auto& fixture : fixtures) {
            auto message = fixture.first;
            message.senderBoot = 11;
            message.receiverBoot = 22;
            message.messageId = 33;
            v4::Parser parser;
            v4::Assembler assembler;
            v4::Message assembled;
            uint32_t now = 1;
            for (size_t offset = 0; offset < message.length;) {
                v4::Frame fragment, decoded;
                CHECK(v4::fragment(message, offset, fragment));
                std::array<uint8_t, v4::kMaxFrame> bytes;
                const auto n = v4::encode(fragment, bytes.data(), bytes.size());
                CHECK(n);
                for (size_t i = 0; i < n; ++i) CHECK(parser.push(bytes[i], now, decoded) == (i + 1 == n));
                offset += fragment.length;
                const auto result = assembler.accept(decoded, now++, assembled);
                CHECK(result == (offset == message.length ? v4::AssemblyResult::Complete : v4::AssemblyResult::Incomplete));
                ++frames;
            }
            CHECK(assembled.kind == message.kind && assembled.senderBoot == message.senderBoot &&
                  assembled.receiverBoot == message.receiverBoot && assembled.messageId == message.messageId);
            CHECK(json(assembled) == json(message));
            if (assembled.kind == v4::Kind::Context) {
                ProductContext decoded;
                CHECK(decodeContextMessage(assembled, "D", decoded));
                CHECK(sameProductContext(decoded, fixture.second));
            } else {
                ContextResult decoded;
                CHECK(decodeContextResult(assembled, decoded));
                matching(decoded, c, true);
            }
        }
    });
}
}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::pair<const char*, std::function<void()>>> groups{
        {"context", fullContext}, {"float32", floatSemantics}, {"valid", validResults},
        {"strict", strictResults}, {"context-rejections", contextRejections},
        {"envelope-faults", envelopesAndFaults}, {"matching", digestMatching}, {"frames", frameCodec}};
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
    std::printf("%u scenarios, %u roundtrips, %u atomic rejections, %u matching checks, %u frames, %u failures\n",
                scenarios, roundtrips, rejections, matches, frames, failures);
    return failures ? 1 : 0;
}
