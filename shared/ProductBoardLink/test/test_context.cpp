#include "ProductContext.h"
#include "BoardProtocolV4.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <vector>

using babytech::boardlink::ProductContext;
using babytech::boardlink::decodeProductContext;
using babytech::boardlink::encodeContextIdentity;
using babytech::boardlink::encodeProductContext;
using babytech::boardlink::kContextIdentityMaxSize;
using babytech::boardlink::sameProductContext;

// Fail only the encoder's checked array allocation; leave ArduinoJson and the
// test harness allocations alone. Ordinary allocations retain standard behavior.
namespace { bool failScratchAllocation = false; }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    if (failScratchAllocation) return nullptr;
    try { return ::operator new[](size); }
    catch (const std::bad_alloc&) { return nullptr; }
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
    ::operator delete[](pointer);
}

namespace {
size_t cases = 0;
constexpr const char* kDevice = "Babytech_unit-01";
const std::string kActive = R"({"type":"feeding_context","device_id":"Babytech_unit-01","baby_id":"baby-7","baby_name":"Ada","formula_brand":"Friso","water_ml":120,"temp":40,"powder_g_per_100ml":13.5,"profile_version":7,"updated_at":"2026-10-06T12:00:00Z"})";
const std::string kCleared = R"({"type":"feeding_context","device_id":"Babytech_unit-01","cleared":true,"profile_version":8})";

template <typename T> std::array<uint8_t, sizeof(T)> bytes(const T& value) {
    std::array<uint8_t, sizeof(T)> result{};
    std::memcpy(result.data(), &value, sizeof(T));
    return result;
}

template <size_t N> void set(char (&target)[N], const std::string& value) {
    assert(value.size() < N);
    std::memset(target, 0, N);
    std::memcpy(target, value.data(), value.size());
}

// Mutations use the real parser, but serialized() preserves deliberately bad tokens.
std::string field(const std::string& source, const char* key, const std::string& token) {
    DynamicJsonDocument doc(8192);
    assert(!deserializeJson(doc, source));
    doc[key] = serialized(token);
    assert(!doc.overflowed());
    std::string result;
    serializeJson(doc, result);
    return result;
}

std::string without(const std::string& source, const char* key) {
    DynamicJsonDocument doc(8192);
    assert(!deserializeJson(doc, source));
    doc.remove(key);
    std::string result;
    serializeJson(doc, result);
    return result;
}

std::string quoted(const std::string& value) {
    DynamicJsonDocument doc(8192);
    doc.set(value);
    assert(!doc.overflowed());
    std::string result;
    serializeJson(doc, result);
    return result;
}

ProductContext sentinel() {
    ProductContext value;
    set(value.deviceId, "sentinel-device");
    value.profileVersion = 314159;
    set(value.babyId, "untouched-id");
    set(value.babyName, "untouched-name");
    set(value.formulaBrand, "untouched-brand");
    value.waterMl = 321;
    value.temperatureC = 51;
    value.powderGPer100Ml = 17.25f;
    return value;
}

ProductContext accept(const std::string& text, const char* expected = kDevice) {
    ProductContext output = sentinel();
    const auto input = text;
    if (!decodeProductContext(reinterpret_cast<const uint8_t*>(text.data()), text.size(),
                              expected, output)) {
        std::cerr << "Unexpected rejection: " << text << std::endl;
        assert(false);
    }
    assert(text == input);
    ++cases;
    return output;
}

void rejectRaw(const uint8_t* data, size_t size, const char* expected = kDevice) {
    // Test both prior active content and a retained tombstone on every failure.
    for (bool cleared : {false, true}) {
        ProductContext output = sentinel();
        if (cleared) {
            output = ProductContext{};
            set(output.deviceId, kDevice);
            output.profileVersion = INT32_MAX;
            output.cleared = true;
        }
        const auto before = bytes(output);
        const bool decoded = decodeProductContext(data, size, expected, output);
        if (decoded) std::cerr << "Unexpected acceptance (" << size << " bytes)" << std::endl;
        assert(!decoded);
        assert(bytes(output) == before);
        ++cases;
    }
}

void reject(const std::string& text, const char* expected = kDevice) {
    const auto before = text;
    rejectRaw(reinterpret_cast<const uint8_t*>(text.data()), text.size(), expected);
    assert(text == before);
}

std::vector<uint8_t> identity(const ProductContext& value) {
    std::array<uint8_t, kContextIdentityMaxSize + 8> output;
    output.fill(0xa5);
    const auto before = bytes(value);
    const size_t size = encodeContextIdentity(value, output.data(), output.size());
    assert(size && size <= kContextIdentityMaxSize);
    assert(bytes(value) == before);
    assert(std::all_of(output.begin() + size, output.end(), [](uint8_t b) { return b == 0xa5; }));
    return {output.begin(), output.begin() + size};
}

void equivalent(const ProductContext& left, const ProductContext& right, bool expected) {
    assert(sameProductContext(left, right) == expected);
    assert(sameProductContext(right, left) == expected);
    assert((identity(left) == identity(right)) == expected);
    ++cases;
}

void legacy() {
    const auto active = accept(kActive);
    assert(std::string(active.deviceId) == kDevice && active.profileVersion == 7 && !active.cleared);
    assert(std::string(active.babyId) == "baby-7" && std::string(active.babyName) == "Ada");
    assert(std::string(active.formulaBrand) == "Friso");
    assert(active.waterMl == 120 && active.temperatureC == 40 && active.powderGPer100Ml == 13.5f);
    equivalent(active, accept(field(kActive, "cleared", "false")), true);
    const auto defaults = accept(without(without(kActive, "baby_name"), "formula_brand"));
    assert(!defaults.babyName[0] && std::string(defaults.formulaBrand) == "Friso");
    for (const char* key : {"baby_name", "formula_brand"}) {
        const auto empty = accept(field(kActive, key, "\"\""));
        assert(!(std::strcmp(key, "baby_name") ? empty.formulaBrand[0] : empty.babyName[0]));
    }
    equivalent(active, accept(without(kActive, "updated_at")), true);
    equivalent(active, accept(field(kActive, "updated_at", "\"ancillary, not retained\"")), true);
    equivalent(active, accept(field(kActive, "updated_at", "\"\"")), true);
    const auto cleared = accept(kCleared);
    assert(cleared.cleared && cleared.profileVersion == 8 && std::string(cleared.deviceId) == kDevice);
    assert(!cleared.babyId[0] && !cleared.babyName[0] && !cleared.formulaBrand[0]);
    assert(!cleared.waterMl && !cleared.temperatureC && cleared.powderGPer100Ml == 0);
    equivalent(cleared, accept(field(kCleared, "updated_at", "\"yesterday\"")), true);
    for (const auto* source : {&kActive, &kCleared}) {
        for (const char* version : {"1", "2147483646", "2147483647"}) {
            const auto value = accept(field(*source, "profile_version", version));
            assert(value.profileVersion == std::stoul(version));
        }
    }
    // This codec retains an incoming version; ordering/conflict arbitration belongs to its caller.
    ProductContext output = cleared;
    assert(decodeProductContext(reinterpret_cast<const uint8_t*>(kActive.data()), kActive.size(),
                                kDevice, output));
    equivalent(output, active, true);
    assert(decodeProductContext(reinterpret_cast<const uint8_t*>(kCleared.data()), kCleared.size(),
                                kDevice, output));
    equivalent(output, cleared, true);
}

void types() {
    for (const char* key : {"type", "device_id", "baby_id", "water_ml", "temp",
                            "powder_g_per_100ml", "profile_version"}) reject(without(kActive, key));
    for (const char* key : {"type", "device_id", "cleared", "profile_version"}) reject(without(kCleared, key));
    for (const char* key : {"type", "device_id", "baby_id", "baby_name", "formula_brand", "updated_at"})
        for (const char* token : {"null", "false", "true", "0", "1", "1.5", "[]", "{}"})
            reject(field(kActive, key, token));
    for (const auto* source : {&kActive, &kCleared}) {
        for (const char* key : {"profile_version", "cleared"})
            for (const char* token : {"null", "\"1\"", "1.0", "1e0", "[]", "{}"})
                reject(field(*source, key, token));
        for (const char* token : {"0", "-1", "2147483648", "4294967295", "4294967296",
                                  "18446744073709551616", "-9223372036854775809", "true", "false"})
            reject(field(*source, "profile_version", token));
        for (const char* token : {"0", "1", "-1", "\"true\"", "\"false\""})
            reject(field(*source, "cleared", token));
        for (const char* key : {"ratio", "schema", "unknown"}) reject(field(*source, key, "1"));
        reject(field(*source, "type", "\"Feeding_context\""));
        reject(field(*source, "type", "\"\""));
    }
    for (const char* key : {"water_ml", "temp", "powder_g_per_100ml"})
        for (const char* token : {"null", "true", "false", "\"40\"", "[]", "{}", "NaN",
                                  "Infinity", "-Infinity", "1e999", "-1e999", "0", "-0", "-1"})
            reject(field(kActive, key, token));
    for (const char* key : {"baby_id", "baby_name", "formula_brand", "water_ml", "temp", "powder_g_per_100ml"})
        for (const char* token : {"null", "\"\"", "0"}) reject(field(kCleared, key, token));
    reject(field(kCleared, "cleared", "false"));
    reject(field(kActive, "cleared", "true"));
    reject(field(kActive, "baby_id", "\"\""));
    reject(kActive, "different-device");
    rejectRaw(nullptr, kActive.size());
    reject("");
    reject(kActive, nullptr);
    reject(kActive, "");
}

void numbers() {
    for (const char* token : {"29.999999", "500.000001", "501", "1e-999"}) reject(field(kActive, "water_ml", token));
    for (const char* token : {"34.999999", "60.000001", "61", "1e-999"}) reject(field(kActive, "temp", token));
    for (const char* token : {"0.99", "50.01", "1e-999"}) reject(field(kActive, "powder_g_per_100ml", token));
    for (const auto& item : std::vector<std::pair<std::string, unsigned>>{
             {"30", 30}, {"30.999", 30}, {"120.875", 120}, {"499.999", 499}, {"500.0", 500}, {"5E+2", 500}}) {
        assert(accept(field(kActive, "water_ml", item.first)).waterMl == item.second);
    }
    for (const auto& item : std::vector<std::pair<std::string, unsigned>>{
             {"35", 35}, {"35.999", 35}, {"40.875", 40}, {"59.999", 59}, {"60.0", 60}, {"6e1", 60}}) {
        assert(accept(field(kActive, "temp", item.first)).temperatureC == item.second);
    }
    for (const char* token : {"1", "50", "13.1", "1.234567890123", "1.35e+1"}) {
        const auto value = accept(field(kActive, "powder_g_per_100ml", token));
        assert(value.powderGPer100Ml == static_cast<float>(std::stod(token)));
    }
    equivalent(accept(kActive), accept(field(field(kActive, "water_ml", "120.999"), "temp", "40.999")), true);
    const auto rate = accept(field(kActive, "powder_g_per_100ml", "13.1"));
    equivalent(rate, accept(field(kActive, "powder_g_per_100ml", "13.10000001")), true);
    equivalent(rate, accept(field(kActive, "powder_g_per_100ml", "13.100001")), false);
}

void grammar() {
    const auto active = accept(kActive);
    equivalent(active, accept(" \r\n\t" + kActive + " \r\n\t"), true);
    for (size_t size = 0; size < kActive.size(); ++size) reject(kActive.substr(0, size));
    for (const char* text : {"{}", "[]", "null", "true", "42", "\"string\"", "{", "}",
                             "{,}", "{\"type\" \"feeding_context\"}"}) reject(text);
    for (const std::string& suffix : {std::string("x"), std::string("{}"), std::string("[]"),
                                     std::string(","), std::string("/* comment */"), std::string(1, '\0')})
        reject(kActive + suffix);
    reject("/* comment */" + kActive);
    reject("// comment\n" + kActive);
    reject("\xef\xbb\xbf" + kActive);
    reject(std::string(1, '\v') + kActive);
    reject(kActive.substr(0, kActive.size() - 1) + ",}");
    reject("{'type':'feeding_context'," + kActive.substr(kActive.find(",") + 1));
    reject("{type:\"feeding_context\"," + kActive.substr(kActive.find(",") + 1));
    for (const char* token : {"+40", "040", "00", "-040", ".40", "40.", "40e", "40e+", "--40", "0x28", "4 0"})
        reject(field(kActive, "temp", token));
    for (const char* token : {"\"\\x41\"", "\"\\u12\"", "\"\\uZZZZ\"", "\"\\a\"", "\"unfinished\\"})
        reject(field(kActive, "baby_name", token));
    for (const char* key : {"type", "device_id", "baby_id", "baby_name", "formula_brand", "water_ml",
                            "temp", "powder_g_per_100ml", "profile_version", "updated_at"}) {
        // Remove an optional field so duplicate detection cannot rely on the field cap.
        const std::string base = without(kActive, std::strcmp(key, "updated_at") ? "updated_at" : "baby_name");
        DynamicJsonDocument doc(8192);
        assert(!deserializeJson(doc, base));
        std::string token;
        serializeJson(doc[key], token);
        reject("{" + quoted(key) + ":" + token + "," + base.substr(1));
        const char hex[] = "0123456789abcdef";
        std::string escaped = "\\u00";
        escaped += hex[static_cast<unsigned char>(key[0]) >> 4];
        escaped += hex[static_cast<unsigned char>(key[0]) & 15];
        escaped += key + 1;
        reject("{\"" + escaped + "\":" + token + "," + base.substr(1));
    }
    reject("{\"cleared\":false," + kCleared.substr(1));
    reject("{\"cle\\u0061red\":true," + kCleared.substr(1));
    std::string escapedKey = kActive;
    escapedKey.replace(escapedKey.find("baby_id"), 7, "baby_\\u0069d");
    equivalent(active, accept(escapedKey), true);
}

void unicode() {
    const std::string face = "\xf0\x9f\x98\x80";
    for (const char* key : {"baby_id", "baby_name", "formula_brand", "updated_at"}) {
        equivalent(accept(field(kActive, key, quoted(face))),
                   accept(field(kActive, key, "\"\\uD83D\\uDE00\"")), true);
        for (const char* token : {"\"\\u0000\"", "\"x\\u0000tail\"", "\"\\uD800\"", "\"\\uDC00\"",
                                  "\"\\uD800x\"", "\"\\uD800\\u0041\"", "\"\\uD800\\uD800\"",
                                  "\"\\uDC00\\uD800\""}) reject(field(kActive, key, token));
        for (const std::string& invalid : {std::string("\x80"), std::string("\xc0\xaf"), std::string("\xc1\xbf"),
                std::string("\xe0\x80\xaf"), std::string("\xed\xa0\x80"), std::string("\xed\xbf\xbf"),
                std::string("\xf0\x80\x80\xaf"), std::string("\xf4\x90\x80\x80"), std::string("\xf5\x80\x80\x80"),
                std::string("\xff"), std::string("\xc2"), std::string("\xe2\x82"), std::string("\xf0\x9f\x98"),
                std::string("\xe2\x41\x80"), std::string(1, '\0')})
            reject(field(kActive, key, "\"before" + invalid + "after\""));
        for (unsigned c = 1; c < 32; ++c)
            reject(field(kActive, key, "\"a" + std::string(1, char(c)) + "b\""));
    }
    for (const char* token : {"\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"", "\"\\u0001\\u007f\"",
                              "\"\\uD7FF\\uE000\\uDBFF\\uDFFF\""}) accept(field(kActive, "baby_name", token));
    reject("{\"baby\\u0000_id\":\"bad\"," + without(kActive, "updated_at").substr(1));
    reject(field(kActive, "device_id", "\"Babytech_unit-01\\u0000suffix\""));
    reject(field(kActive, "type", "\"feeding_context\\u0000suffix\""));
}

void limits() {
    const std::string face = "\xf0\x9f\x98\x80";
    std::string maximal = field(kActive, "device_id", quoted(std::string(64, 'D')));
    for (const auto& item : std::vector<std::pair<const char*, size_t>>{
             {"baby_id", 96}, {"baby_name", 320}, {"formula_brand", 480}}) {
        const auto* key = item.first;
        const size_t cap = item.second;
        accept(field(kActive, key, quoted(std::string(cap, 'a'))));
        reject(field(kActive, key, quoted(std::string(cap + 1, 'a'))));
        std::string utf8;
        for (size_t i = 0; i < cap / 4; ++i) utf8 += face;
        accept(field(kActive, key, quoted(utf8)));
        reject(field(kActive, key, quoted(utf8 + "a")));
        maximal = field(maximal, key, quoted(utf8));
        // Wire escape length is separate from decoded UTF-8 length.
        std::string escaped = "\"";
        for (size_t i = 0; i < cap / 4; ++i) escaped += "\\ud83d\\ude00";
        accept(field(kActive, key, escaped + "\""));
        reject(field(kActive, key, escaped + "a\""));
    }
    const auto maximum = accept(maximal, std::string(64, 'D').c_str());
    const auto canonical = identity(maximum);
    assert(canonical.size() == 981 && canonical.size() == kContextIdentityMaxSize);
    assert(canonical[2] == 64 && canonical[3] == 0);
    assert(canonical[72] == 96 && canonical[73] == 0);
    assert(canonical[170] == 0x40 && canonical[171] == 1);
    assert(canonical[492] == 0xe0 && canonical[493] == 1);
    ++cases;
    for (size_t capacity = 0; capacity < canonical.size(); ++capacity) {
        std::array<uint8_t, kContextIdentityMaxSize + 2> buffer;
        buffer.fill(0x5a);
        const auto before = buffer;
        assert(!encodeContextIdentity(maximum, buffer.data() + 1, capacity));
        assert(buffer == before);
        ++cases;
    }
    std::array<uint8_t, kContextIdentityMaxSize> exactCanonical;
    assert(encodeContextIdentity(maximum, exactCanonical.data(), exactCanonical.size()) == 981);
    assert(std::equal(canonical.begin(), canonical.end(), exactCanonical.begin()));
    ++cases;
    for (const auto* source : {&kActive, &kCleared}) {
        auto exact = *source + std::string(2047 - source->size(), ' ');
        equivalent(accept(*source), accept(exact), true);
        reject(exact + " ");
        auto padded = field(*source, "updated_at", "\"\"");
        padded = field(padded, "updated_at", quoted(std::string(2047 - padded.size(), 't')));
        assert(padded.size() == 2047);
        equivalent(accept(*source), accept(padded), true);
        reject(field(padded, "updated_at", quoted(std::string(2048 - field(*source, "updated_at", "\"\"").size(), 't'))));
    }
    std::string escaped = kActive;
    for (const auto& item : std::vector<std::pair<const char*, size_t>>{{"baby_id", 96}, {"baby_name", 320}, {"formula_brand", 480}}) {
        std::string token = "\"";
        for (size_t i = 0; i < item.second; ++i) token += "\\u0061";
        escaped = field(escaped, item.first, token + "\"");
    }
    assert(escaped.size() > 2047);
    reject(escaped);
    // Exact-sized, non-NUL-terminated storage must never be scanned past length.
    const std::vector<uint8_t> exact(kActive.begin(), kActive.end());
    ProductContext output;
    assert(decodeProductContext(exact.data(), exact.size(), kDevice, output));
    ++cases;
}

void canonical() {
    const auto active = accept(kActive);
    // Hand-authored schema-1 vector: lengths and integers little-endian, 13.5f = 0x41580000.
    const std::vector<uint8_t> golden = {
        1, 0, 16, 0, 'B','a','b','y','t','e','c','h','_','u','n','i','t','-','0','1',
        7, 0, 0, 0, 6, 0, 'b','a','b','y','-','7', 3, 0, 'A','d','a',
        5, 0, 'F','r','i','s','o', 120, 0, 40, 0, 0, 0x58, 0x41
    };
    assert(identity(active) == golden);
    const auto cleared = accept(field(kCleared, "profile_version", "2147483647"));
    const std::vector<uint8_t> tombstone = {
        1, 1, 16, 0, 'B','a','b','y','t','e','c','h','_','u','n','i','t','-','0','1', 0xff, 0xff, 0xff, 0x7f
    };
    assert(identity(cleared) == tombstone);
    cases += 2;
    auto multiByte = active;
    multiByte.profileVersion = 0x01020304;
    multiByte.waterMl = 500;
    auto expected = golden;
    expected[20] = 4; expected[21] = 3; expected[22] = 2; expected[23] = 1;
    expected[44] = 0xf4; expected[45] = 1;
    assert(identity(multiByte) == expected);
    ++cases;
    const auto reordered = accept(R"( { "profile_version":7, "temp":4e1, "water_ml":120.9,
        "powder_g_per_100ml":13.50000001, "formula_brand":"Friso", "baby_name":"\u0041da",
        "baby_id":"baby-7", "device_id":"Babytech_unit-01", "type":"feeding_context" } )");
    equivalent(active, reordered, true);
    for (unsigned member = 0; member < 8; ++member) {
        auto changed = active;
        switch (member) {
            case 0: set(changed.deviceId, "Other"); break;
            case 1: ++changed.profileVersion; break;
            case 2: set(changed.babyId, "other-baby"); break;
            case 3: set(changed.babyName, "other-name"); break;
            case 4: set(changed.formulaBrand, "other-brand"); break;
            case 5: ++changed.waterMl; break;
            case 6: ++changed.temperatureC; break;
            case 7: changed.powderGPer100Ml = std::nextafter(changed.powderGPer100Ml, 50.0f); break;
        }
        equivalent(active, changed, false);
    }
    equivalent(active, cleared, false);
    auto changed = cleared;
    --changed.profileVersion;
    equivalent(cleared, changed, false);
    changed = cleared;
    set(changed.deviceId, "Other");
    equivalent(cleared, changed, false);
    changed = cleared;
    changed.powderGPer100Ml = -0.0f;
    equivalent(cleared, changed, true);
    // Fixed-array unused tails/padding are not semantic bytes.
    changed = active;
    changed.babyId[sizeof(changed.babyId) - 1] = 'x';
    changed.babyName[sizeof(changed.babyName) - 1] = 'y';
    changed.formulaBrand[sizeof(changed.formulaBrand) - 1] = 'z';
    equivalent(active, changed, true);
    for (const auto* value : {&active, &cleared}) {
        const auto expected = identity(*value);
        for (size_t capacity = 0; capacity < expected.size(); ++capacity) {
            std::array<uint8_t, kContextIdentityMaxSize + 2> buffer;
            buffer.fill(0x5a);
            const auto before = buffer;
            assert(!encodeContextIdentity(*value, buffer.data() + 1, capacity));
            assert(buffer == before);
            ++cases;
        }
        std::vector<uint8_t> exact(expected.size());
        assert(encodeContextIdentity(*value, exact.data(), exact.size()) == exact.size());
        assert(exact == expected);
        assert(!encodeContextIdentity(*value, nullptr, expected.size()));
        cases += 2;
    }
}

void rejectContext(const ProductContext& value) {
    std::array<uint8_t, kContextIdentityMaxSize + 2> output;
    output.fill(0xa5);
    const auto before = output;
    assert(!encodeContextIdentity(value, output.data(), output.size()));
    assert(output == before);
    assert(!encodeProductContext(value, output.data(), output.size()));
    assert(output == before);
    assert(!sameProductContext(value, value));
    const auto good = sentinel();
    assert(!sameProductContext(value, good) && !sameProductContext(good, value));
    ++cases;
}

void invalidCanonical() {
    rejectContext(ProductContext{});
    const auto base = accept(kActive);
    for (uint32_t version : {0u, uint32_t(INT32_MAX) + 1u, UINT32_MAX}) {
        auto value = base;
        value.profileVersion = version;
        rejectContext(value);
    }
    for (unsigned member = 0; member < 4; ++member) {
        auto value = base;
        switch (member) {
            case 0: std::memset(value.deviceId, 'd', sizeof(value.deviceId)); break;
            case 1: std::memset(value.babyId, 'b', sizeof(value.babyId)); break;
            case 2: std::memset(value.babyName, 'n', sizeof(value.babyName)); break;
            case 3: std::memset(value.formulaBrand, 'f', sizeof(value.formulaBrand)); break;
        }
        rejectContext(value);
    }
    for (uint16_t water : {uint16_t(0), uint16_t(29), uint16_t(501), uint16_t(UINT16_MAX)}) {
        auto value = base; value.waterMl = water; rejectContext(value);
    }
    for (uint8_t temp : {uint8_t(0), uint8_t(34), uint8_t(61), uint8_t(255)}) {
        auto value = base; value.temperatureC = temp; rejectContext(value);
    }
    for (float powder : {0.0f, -0.0f, -1.0f, std::nextafter(1.0f, 0.0f), std::nextafter(50.0f, 51.0f),
                          std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::quiet_NaN()}) {
        auto value = base; value.powderGPer100Ml = powder; rejectContext(value);
    }
    for (unsigned member = 0; member < 4; ++member) {
        auto value = base;
        switch (member) {
            case 0: set(value.deviceId, "bad/id"); break;
            case 1: set(value.babyId, ""); break;
            case 2: set(value.babyName, "\xed\xa0\x80"); break;
            case 3: set(value.formulaBrand, "\xf4\x90\x80\x80"); break;
        }
        rejectContext(value);
    }
    const auto cleared = accept(kCleared);
    for (unsigned member = 0; member < 6; ++member) {
        auto value = cleared;
        switch (member) {
            case 0: set(value.babyId, "b"); break;
            case 1: set(value.babyName, "n"); break;
            case 2: set(value.formulaBrand, "f"); break;
            case 3: value.waterMl = 120; break;
            case 4: value.temperatureC = 40; break;
            case 5: value.powderGPer100Ml = 13.5f; break;
        }
        rejectContext(value);
    }
}

void deviceIds() {
    for (unsigned c = 1; c < 33; ++c) {
        const std::string id = "a" + std::string(1, char(c)) + "b";
        reject(field(kActive, "device_id", quoted(id)), id.c_str());
        auto value = sentinel();
        set(value.deviceId, id);
        rejectContext(value);
    }
    reject(field(kActive, "device_id", quoted("a\x7f")), "a\x7f");
    for (const std::string& id : {std::string("0"), std::string("Z"), std::string("a_-09"), std::string(64, 'a')}) {
        const auto value = accept(field(kActive, "device_id", quoted(id)), id.c_str());
        assert(std::string(value.deviceId) == id);
    }
    for (const std::string& id : {std::string(""), std::string(65, 'a'), std::string("a/b"), std::string("a b"),
                                std::string("a.b"), std::string("a:b"), std::string("\xe5\xae\x9d"),
                                std::string("_unit"), std::string("-unit")}) {
        reject(field(kActive, "device_id", quoted(id)), id.c_str());
        auto value = sentinel();
        if (id.size() < sizeof(value.deviceId)) {
            set(value.deviceId, id);
            rejectContext(value);
        }
    }
}

void float32Boundaries() {
    // Legacy Motion checks the converted float32, not the original JSON double.
    for (const char* token : {"0.99999999", "50.000001"}) {
        const float rounded = static_cast<float>(std::stod(token));
        assert(rounded == 1.0f || rounded == 50.0f);
        assert(accept(field(kActive, "powder_g_per_100ml", token)).powderGPer100Ml == rounded);
    }
    for (const char* token : {"0.9999999", "50.000003"}) reject(field(kActive, "powder_g_per_100ml", token));
}

std::string wireRoundtrip(const ProductContext& context) {
    constexpr size_t maximum = babytech::v4::kMaxMessage;
    std::vector<uint8_t> buffer(maximum + 9, 0xa5);
    const auto before = bytes(context);
    const size_t length = encodeProductContext(context, buffer.data() + 1, buffer.size() - 1);
    assert(length && length <= maximum);
    assert(bytes(context) == before);
    assert(buffer.front() == 0xa5);
    assert(std::all_of(buffer.begin() + 1 + length, buffer.end(),
                       [](uint8_t b) { return b == 0xa5; }));
    // No terminator or hidden slack: ASan checks both exact-sized wire buffers.
    std::vector<uint8_t> exact(length, 0x5a);
    assert(encodeProductContext(context, exact.data(), exact.size()) == length);
    assert(std::equal(exact.begin(), exact.end(), buffer.begin() + 1));
    ProductContext decoded = sentinel();
    assert(decodeProductContext(exact.data(), exact.size(), context.deviceId, decoded));
    assert(sameProductContext(context, decoded) && sameProductContext(decoded, context));
    assert(identity(context) == identity(decoded));
    if (!context.cleared) assert(bytes(context.powderGPer100Ml) == bytes(decoded.powderGPer100Ml));
    assert(std::find(exact.begin(), exact.end(), uint8_t(0)) == exact.end());
    DynamicJsonDocument doc(8192);
    assert(!deserializeJson(doc, static_cast<const uint8_t*>(exact.data()), exact.size()));
    assert(doc["type"] == "feeding_context" && doc["device_id"] == context.deviceId);
    assert(doc["profile_version"].is<uint32_t>());
    assert(doc["profile_version"].as<uint32_t>() == context.profileVersion);
    assert(!doc.containsKey("updated_at"));
    if (context.cleared) {
        assert(doc.size() == 4 && doc["cleared"].is<bool>() && doc["cleared"].as<bool>());
    } else {
        assert(doc.size() == 9 && !doc.containsKey("cleared"));
        for (const char* key : {"baby_id", "baby_name", "formula_brand"})
            assert(doc[key].is<JsonString>());
        for (const char* key : {"water_ml", "temp"}) assert(doc[key].is<unsigned>());
        assert(doc["powder_g_per_100ml"].is<double>() && !doc["powder_g_per_100ml"].is<JsonString>());
    }
    ++cases;
    return {exact.begin(), exact.end()};
}

void rejectWire(const ProductContext& context, size_t capacity = 4096) {
    std::vector<uint8_t> output(capacity + 2, 0x5a);
    const auto before = output;
    const auto contextBefore = bytes(context);
    assert(!encodeProductContext(context, output.data() + 1, capacity));
    assert(output == before && bytes(context) == contextBefore);
    ++cases;
}

void wireEncoder() {
    const auto active = accept(kActive);
    const auto cleared = accept(kCleared);
    for (const auto* context : {&active, &cleared}) {
        const auto wire = wireRoundtrip(*context);
        // Input strings remain linked until the scratch JSON is complete; only
        // the final copy may overwrite an overlapping destination in the input.
        auto aliased = *context;
        const auto original = bytes(aliased);
        auto* output = reinterpret_cast<uint8_t*>(aliased.babyName);
        assert(!encodeProductContext(aliased, output, 1));
        assert(bytes(aliased) == original);
        ++cases;
        assert(wire.size() <= sizeof(aliased.babyName));
        assert(encodeProductContext(aliased, output, sizeof(aliased.babyName)) == wire.size());
        auto expected = original;
        std::memcpy(expected.data() + offsetof(ProductContext, babyName), wire.data(), wire.size());
        assert(bytes(aliased) == expected); // Includes the destination suffix and all other fields.
        ProductContext decoded;
        assert(decodeProductContext(output, wire.size(), context->deviceId, decoded));
        assert(sameProductContext(*context, decoded) && identity(*context) == identity(decoded));
        if (!context->cleared)
            assert(bytes(context->powderGPer100Ml) == bytes(decoded.powderGPer100Ml));
        ++cases;
        for (size_t capacity = 0; capacity < wire.size(); ++capacity) rejectWire(*context, capacity);
        for (size_t capacity : {size_t(0), size_t(1), wire.size(), SIZE_MAX}) {
            assert(!encodeProductContext(*context, nullptr, capacity));
            ++cases;
        }
        std::vector<uint8_t> large(babytech::v4::kMaxMessage + 1, 0x5a);
        assert(encodeProductContext(*context, large.data(), SIZE_MAX) == wire.size());
        assert(std::all_of(large.begin() + wire.size(), large.end(),
                           [](uint8_t b) { return b == 0x5a; }));
        ++cases;
    }
    auto context = active;
    set(context.babyName, "");
    set(context.formulaBrand, "");
    wireRoundtrip(context);
    for (uint32_t version : {1u, uint32_t(INT32_MAX)}) {
        context.profileVersion = version;
        for (uint16_t water : {uint16_t(30), uint16_t(500)}) {
            context.waterMl = water;
            for (uint8_t temperature : {uint8_t(35), uint8_t(60)}) {
                context.temperatureC = temperature;
                wireRoundtrip(context);
            }
        }
    }
    context = cleared;
    context.powderGPer100Ml = -0.0f; // Tombstone identity intentionally ignores the sign of zero.
    wireRoundtrip(context);
    context = active;
    set(context.babyId, "\"\\/id");
    set(context.babyName, "\"quoted\" \\slash/ \x7f");
    set(context.formulaBrand, "\xc3\xa9\xe5\xae\x9d\xf0\x9f\x98\x80");
    wireRoundtrip(context);
    for (unsigned c = 1; c < 32; ++c) {
        const std::string text = "a" + std::string(1, char(c)) + "z";
        set(context.babyId, text);
        set(context.babyName, text);
        set(context.formulaBrand, text);
        const auto wire = wireRoundtrip(context);
        assert(wire.find(char(c)) == std::string::npos);
    }
    // Exercise bulk escaping and arbitrary unused fixed-array tails together.
    std::string allControls;
    for (unsigned c = 1; c < 32; ++c) allControls += char(c);
    set(context.babyId, allControls);
    set(context.babyName, allControls + "\"\\");
    set(context.formulaBrand, allControls);
    context.babyId[sizeof(context.babyId) - 1] = char(0xff);
    context.babyName[sizeof(context.babyName) - 1] = char(0xff);
    context.formulaBrand[sizeof(context.formulaBrand) - 1] = char(0xff);
    wireRoundtrip(context);
}

void wireLimits() {
    auto context = accept(kActive);
    set(context.deviceId, std::string(64, 'D'));
    context.profileVersion = INT32_MAX;
    context.waterMl = 500;
    context.temperatureC = 60;
    for (const std::string& unit : {std::string("a"), std::string("\xc3\xa9"),
                                   std::string("\xf0\x9f\x98\x80"), std::string("\""),
                                   std::string("\\"), std::string("\n")}) {
        auto repeated = [&unit](size_t size) {
            std::string text;
            for (size_t i = 0; i < size / unit.size(); ++i) text += unit;
            return text;
        };
        set(context.babyId, repeated(96));
        set(context.babyName, repeated(320));
        set(context.formulaBrand, repeated(480));
        assert(identity(context).size() == kContextIdentityMaxSize);
        wireRoundtrip(context);
    }
    set(context.babyId, std::string(96, 'b'));
    set(context.babyName, std::string(320, 'n'));
    set(context.formulaBrand, std::string(480, 'f'));
    const size_t baseline = wireRoundtrip(context).size();
    const size_t maximum = babytech::v4::kMaxMessage;
    assert(baseline < maximum && maximum - baseline < 5 * 480);
    // A raw control becomes six bytes (+5), a quote becomes two (+1).
    // Construct exact 2046, 2047 and 2048 lengths independently of key order.
    for (size_t target : {maximum - 1, maximum, maximum + 1}) {
        const size_t extra = target - baseline;
        std::string brand(480, 'f');
        brand.replace(0, extra / 5, extra / 5, char(1));
        brand.replace(extra / 5, extra % 5, extra % 5, '"');
        set(context.formulaBrand, brand);
        assert(babytech::boardlink::validProductContext(context));
        if (target <= maximum) {
            assert(wireRoundtrip(context).size() == target);
            rejectWire(context, target - 1);
        } else {
            rejectWire(context, maximum);
            rejectWire(context, target);
            rejectWire(context, 8192);
        }
    }
    set(context.babyId, std::string(96, char(1)));
    set(context.babyName, std::string(320, char(2)));
    set(context.formulaBrand, std::string(480, char(31)));
    assert(babytech::boardlink::validProductContext(context));
    rejectWire(context, 8192);
}

void wireFloat32() {
    auto context = accept(kActive);
    auto checkBits = [&context](uint32_t bits) {
        std::memcpy(&context.powderGPer100Ml, &bits, sizeof(bits));
        assert(babytech::boardlink::validProductContext(context));
        wireRoundtrip(context);
    };
    constexpr uint32_t first = 0x3f800000; // 1.0f
    constexpr uint32_t last = 0x42480000;  // 50.0f
    // Rounding boundaries: neighbors of powers of two and every single mantissa bit.
    for (uint32_t exponent = 127; exponent <= 132; ++exponent) {
        const uint32_t base = exponent << 23;
        for (uint32_t delta : {0u, 1u, 2u, 0x3fffffu, 0x400000u, 0x7ffffeu, 0x7fffffu}) {
            const uint32_t bits = base + delta;
            if (bits <= last) checkBits(bits);
            if (base >= first + delta) checkBits(base - delta);
        }
        for (unsigned bit = 0; bit < 23; ++bit) {
            const uint32_t bits = base + (uint32_t(1) << bit);
            if (bits <= last) checkBits(bits);
        }
    }
    checkBits(first);
    checkBits(first + 1);
    checkBits(last - 1);
    checkBits(last);
    // Stratified coverage across all legal exponents plus reproducible low-bit noise.
    uint32_t random = 0x5eed1234;
    constexpr uint32_t samples = 16384;
    for (uint32_t i = 0; i < samples; ++i) {
        checkBits(first + uint32_t(uint64_t(last - first) * i / (samples - 1)));
        random = random * 1664525u + 1013904223u;
        checkBits(first + random % (last - first + 1));
    }
}

void wireAllocation() {
    for (const auto& context : {accept(kActive), accept(kCleared)}) {
        failScratchAllocation = true;
        rejectWire(context);
        failScratchAllocation = false;
        wireRoundtrip(context);
    }
}
}

int main(int argc, char** argv) {
    static_assert(ARDUINOJSON_VERSION_MAJOR == 6, "Use real ArduinoJson 6");
    struct Group { const char* name; void (*run)(); };
    const Group groups[] = {{"legacy", legacy}, {"types", types}, {"numbers", numbers},
        {"grammar", grammar}, {"unicode", unicode}, {"limits", limits}, {"canonical", canonical},
        {"invalid-canonical", invalidCanonical}, {"device-ids", deviceIds}, {"float32", float32Boundaries},
        {"encoder", wireEncoder}, {"encoder-limits", wireLimits}, {"encoder-float32", wireFloat32},
        {"encoder-allocation", wireAllocation}};
    bool matched = false;
    for (const auto& group : groups) {
        if (argc == 1 || (argc == 2 && std::strcmp(argv[1], group.name) == 0)) {
            matched = true;
            std::cout << "Running " << group.name << std::endl;
            const size_t before = cases;
            group.run();
            std::cout << group.name << ": " << cases - before << " cases passed; "
                      << cases << " cumulative" << std::endl;
        }
    }
    if (!matched) { std::cerr << "Unknown test group" << std::endl; return 2; }
    std::cout << "ProductContext: " << cases << " cases passed (ArduinoJson "
              << ARDUINOJSON_VERSION << ")" << std::endl;
    std::cout << "Host sizeof(ProductContext)=" << sizeof(ProductContext)
              << ", JSON scratch capacity=" << JSON_OBJECT_SIZE(11) + 2048 << std::endl;
}
