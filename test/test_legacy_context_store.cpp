#include "LegacyContextStore.h"
#include "FakeLegacyNvs.h"
#include <ArduinoJson.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

using namespace babytech::boardlink;
using fake::Bytes;
using fake::Op;
using fake::io;

namespace {
unsigned scenarios = 0;
constexpr const char* kDevice = "Babytech_01-test";
const std::string kActive = R"({"type":"feeding_context","device_id":"Babytech_01-test","baby_id":"baby-1","baby_name":"Mia","formula_brand":"Friso","water_ml":180,"temp":45,"powder_g_per_100ml":25,"profile_version":12345})";
const std::string kTombstone = R"({"type":"feeding_context","device_id":"Babytech_01-test","cleared":true,"profile_version":2147483647})";

void seed(const std::string& json = kActive) {
    Bytes bytes(json.begin(), json.end());
    bytes.push_back(0);
    io.disk["productctx"]["payload"] = {bytes, fake::Type::String};
}

ProductContext sample(bool cleared = false) {
    ProductContext out;
    std::strcpy(out.deviceId, kDevice);
    out.profileVersion = cleared ? 2147483647 : 12345;
    out.cleared = cleared;
    if (!cleared) {
        std::strcpy(out.babyId, "baby-1");
        std::strcpy(out.babyName, "Mia");
        std::strcpy(out.formulaBrand, "Friso");
        out.waterMl = 180;
        out.temperatureC = 45;
        out.powderGPer100Ml = 25;
    }
    return out;
}

void equal(const ProductContext& left, const ProductContext& right) {
    assert(std::strcmp(left.deviceId, right.deviceId) == 0);
    assert(left.profileVersion == right.profileVersion && left.cleared == right.cleared);
    assert(std::strcmp(left.babyId, right.babyId) == 0);
    assert(std::strcmp(left.babyName, right.babyName) == 0);
    assert(std::strcmp(left.formulaBrand, right.formulaBrand) == 0);
    assert(left.waterMl == right.waterMl && left.temperatureC == right.temperatureC);
    assert(left.powderGPer100Ml == right.powderGPer100Ml);
}

void load(LegacyContextLoad expected, const ProductContext& wanted = sample(),
          const char* expectedDevice = kDevice) {
    ProductContext output = sample();
    std::strcpy(output.deviceId, "untouched-output");
    std::strcpy(output.babyName, "untouched-name");
    output.profileVersion = 987;
    std::array<uint8_t, sizeof(output)> before{};
    std::memcpy(before.data(), &output, sizeof(output));
    const auto disk = io.disk;
    assert(loadLegacyProductContext(expectedDevice, output) == expected);
    if (expected == LegacyContextLoad::Ready) equal(output, wanted);
    else assert(std::memcmp(before.data(), &output, sizeof(output)) == 0);
    assert(io.disk == disk);
    fake::verify();
    assert(fake::count(Op::Close) <= fake::count(Op::Open));
}

void scenario(const std::string& name, const std::function<void()>& run) {
    fake::reset();
    for (const char* space : {"productpair", "productstate", "brainstate", "wifi-cfg",
                              "formulaevt", "actuatorcfg", "sensorcfg"})
        io.disk[space]["payload"] = {{0xff, 0, 1}, fake::Type::Blob};
    std::printf("[%u] %s\n", ++scenarios, name.c_str());
    std::fflush(stdout);
    run();
    fake::verify();
}

std::string changed(const std::function<void(JsonObject)>& change,
                    const std::string& source = kActive) {
    DynamicJsonDocument document(8192);
    assert(!deserializeJson(document, source));
    change(document.as<JsonObject>());
    std::string result;
    serializeJson(document, result);
    return result;
}

void rejected(const std::string& payload) {
    seed(payload);
    load(LegacyContextLoad::Corrupt);
    assert(fake::count(Op::Close) == 1);
}

void successAndAbsence() {
    scenario("missing namespace is read-only", [] {
        load(LegacyContextLoad::Missing);
        assert(io.calls == std::vector<Op>{Op::Open});
    });
    scenario("missing key leaves existing namespace unchanged", [] {
        io.disk["productctx"]["other"] = {{1}, fake::Type::U32};
        load(LegacyContextLoad::Missing);
        assert((io.calls == std::vector<Op>{Op::Open, Op::Query, Op::Close}));
    });
    for (bool cleared : {false, true}) {
        scenario(cleared ? "tombstone preserves maximum legacy version" : "active profile", [&] {
            seed(cleared ? kTombstone : kActive);
            load(LegacyContextLoad::Ready, sample(cleared));
            assert((io.calls == std::vector<Op>{Op::Open, Op::Query, Op::Read, Op::Close}));
            load(LegacyContextLoad::Ready, sample(cleared));
        });
    }
    scenario("full non-ASCII names, baby ID and 64-byte device ID", [] {
        const std::string codepoint = "\xF0\x9F\x8D\xBC";
        std::string name, brand, baby;
        for (unsigned i = 0; i < 120; ++i) {
            brand += codepoint;
            if (i < 80) name += codepoint;
            if (i < 24) baby += codepoint;
        }
        const std::string device(64, 'd');
        seed(changed([&](JsonObject object) {
            object["baby_id"] = baby;
            object["baby_name"] = name;
            object["formula_brand"] = brand;
            object["device_id"] = device;
            object["updated_at"] = "2026-10-06T12:34:56+08:00";
        }));
        auto wanted = sample();
        std::strcpy(wanted.babyId, baby.c_str());
        std::strcpy(wanted.babyName, name.c_str());
        std::strcpy(wanted.formulaBrand, brand.c_str());
        std::strcpy(wanted.deviceId, device.c_str());
        load(LegacyContextLoad::Ready, wanted, device.c_str());
    });
    for (const size_t size : {2047u, 2048u, 2049u}) {
        scenario("stored length including NUL " + std::to_string(size), [&] {
            seed(kActive + std::string(size - kActive.size() - 1, ' '));
            load(size <= 2048 ? LegacyContextLoad::Ready : LegacyContextLoad::Corrupt);
            assert(fake::count(Op::Read) == (size <= 2048 ? 1u : 0u));
        });
    }
}

void storageFailures() {
    for (const size_t copied : {size_t(0), size_t(1), kActive.size() / 2, kActive.size()}) {
        scenario("SDK success/full length but short copy " + std::to_string(copied), [&] {
            seed();
            fake::Fault fault{Op::Read, ESP_OK};
            fault.copyLimit = copied;
            io.faults.push_back(fault);
            load(LegacyContextLoad::Corrupt);
        });
    }
    for (auto type : {fake::Type::Blob, fake::Type::U32}) {
        scenario("NVS wrong type is an SDK error, never read as blob", [&] {
            seed();
            io.disk["productctx"]["payload"].type = type;
            load(LegacyContextLoad::IoError);
            assert(fake::count(Op::Read) == 0 && fake::count(Op::Close) == 1);
        });
    }
    for (auto op : {Op::Open, Op::Query, Op::Read}) {
        for (auto error : {ESP_FAIL, ESP_ERR_NVS_NOT_INITIALIZED, ESP_ERR_NVS_INVALID_HANDLE,
                           ESP_ERR_NVS_TYPE_MISMATCH, ESP_ERR_NVS_INVALID_LENGTH,
                           ESP_ERR_NVS_NOT_FOUND}) {
            scenario("SDK failure " + std::to_string(int(op)) + "/" + std::to_string(error), [&] {
                seed();
                io.faults.push_back({op, error});
                load(error == ESP_ERR_NVS_NOT_FOUND && op != Op::Read
                     ? LegacyContextLoad::Missing : LegacyContextLoad::IoError);
                assert(fake::count(Op::Close) == (op == Op::Open ? 0u : 1u));
            });
        }
    }
    for (size_t length : {size_t(0), kActive.size(), kActive.size() + 2, size_t(2049), SIZE_MAX}) {
        scenario("successful second get changes length " + std::to_string(length), [&] {
            seed();
            io.faults.push_back({Op::Read, ESP_OK, true, length});
            load(LegacyContextLoad::IoError);
        });
    }
    for (size_t length : {size_t(0), size_t(2049), SIZE_MAX}) {
        scenario("invalid size query " + std::to_string(length), [&] {
            seed();
            io.faults.push_back({Op::Query, ESP_OK, true, length});
            load(LegacyContextLoad::Corrupt);
            assert(fake::count(Op::Read) == 0);
        });
    }
    scenario("value grows after size query", [] {
        seed();
        io.faults.push_back({Op::Query, ESP_OK, true, kActive.size()});
        load(LegacyContextLoad::IoError);
    });
    for (const Bytes& bytes : {Bytes{}, Bytes{0}, Bytes{'{', '}'}, Bytes{'{', '}', 0, 0}}) {
        scenario("zero/empty/unterminated/embedded-NUL stored value", [&] {
            io.disk["productctx"]["payload"] = {bytes, fake::Type::String};
            load(LegacyContextLoad::Corrupt);
        });
    }
    scenario("valid JSON without stored terminator", [] {
        seed();
        io.disk["productctx"]["payload"].bytes.pop_back();
        load(LegacyContextLoad::Corrupt);
    });
    scenario("valid JSON followed by NUL and hidden content", [] {
        auto payload = kActive;
        payload.push_back('\0');
        payload += "hidden";
        rejected(payload);
    });
}

void codecFailures() {
    for (const char* field : {"type", "device_id", "baby_id", "water_ml", "temp",
                              "powder_g_per_100ml", "profile_version"}) {
        scenario(std::string("missing required ") + field, [&] {
            rejected(changed([&](JsonObject object) { object.remove(field); }));
        });
    }
    for (const char* field : {"device_id", "baby_id", "baby_name", "formula_brand"}) {
        scenario(std::string("wrong text type ") + field, [&] {
            rejected(changed([&](JsonObject object) { object[field] = false; }));
        });
    }
    for (const char* field : {"water_ml", "temp", "powder_g_per_100ml", "profile_version"}) {
        for (unsigned kind = 0; kind < 3; ++kind) {
            scenario(std::string("invalid number ") + field + "/" + std::to_string(kind), [&] {
                rejected(changed([&](JsonObject object) {
                    if (kind == 0) object[field] = true;
                    if (kind == 1) object[field] = "45";
                    if (kind == 2) object[field] = -1;
                }));
            });
        }
    }
    for (const char* field : {"baby_id", "baby_name", "formula_brand", "device_id"}) {
        scenario(std::string("overlong ") + field, [&] {
            const size_t max = std::strcmp(field, "baby_id") == 0 ? 96 :
                std::strcmp(field, "baby_name") == 0 ? 320 :
                std::strcmp(field, "formula_brand") == 0 ? 480 : 64;
            rejected(changed([&](JsonObject object) { object[field] = std::string(max + 1, 'x'); }));
        });
    }
    for (const std::string& invalid : {std::string("\xc0\xaf"), std::string("\xed\xa0\x80"),
                                      std::string("\xf4\x90\x80\x80"), std::string("\xe4\xb8")}) {
        scenario("invalid UTF-8 name", [&] {
            rejected(changed([&](JsonObject object) { object["baby_name"] = invalid; }));
        });
    }
    for (const auto& payload : {std::string{}, std::string("{}"), kActive + "x",
                                kActive.substr(0, kActive.size() - 1),
                                std::string(R"({"type":"feeding_context","device_id":"Babytech_01-test","cleared":true,"profile_version":0})")}) {
        scenario("invalid JSON or tombstone", [&] { rejected(payload); });
    }
    for (const char* expected : {"other-device", "", static_cast<const char*>(nullptr)}) {
        scenario("identity mismatch or invalid expected identity", [&] {
            seed();
            load(LegacyContextLoad::Corrupt, sample(), expected);
        });
    }
    scenario("tombstone identity mismatch", [] {
        seed(kTombstone);
        load(LegacyContextLoad::Corrupt, sample(), "other-device");
    });
    scenario("escaped NUL in otherwise valid JSON", [] {
        const auto position = kActive.find("Mia");
        auto payload = kActive;
        payload.replace(position, 3, "Mi\\u0000a");
        rejected(payload);
    });
    scenario("legacy ratio cannot replace or shadow powder rate", [] {
        rejected(changed([](JsonObject object) { object["ratio"] = 25; }));
    });
}

std::string escapedControls(const std::string& legacy) {
    std::string escaped;
    for (const unsigned char c : legacy) {
        if (c < 0x20) {
            char code[7];
            std::snprintf(code, sizeof(code), "\\u%04x", unsigned(c));
            escaped += code;
        } else escaped += char(c);
    }
    return escaped;
}

std::string withRawName(const std::string& name) {
    auto json = kActive;
    json.replace(json.find("Mia"), 3, name);
    return json;
}

void legacyRoundtrip(const std::string& name, const std::string& brand) {
    // Exercise the actual old serializer, not a replacement implementation.
    const auto legacy = changed([&](JsonObject object) {
        object["baby_name"] = name;
        object["formula_brand"] = brand;
    });
    const auto escaped = escapedControls(legacy);
    assert(escaped != legacy);
    auto wanted = sample();
    std::strcpy(wanted.babyName, name.c_str());
    std::strcpy(wanted.formulaBrand, brand.c_str());
    ProductContext decoded;
    assert(decodeProductContext(reinterpret_cast<const uint8_t*>(escaped.data()),
                                escaped.size(), kDevice, decoded));
    equal(decoded, wanted);
    assert(!decodeProductContext(reinterpret_cast<const uint8_t*>(legacy.data()),
                                 legacy.size(), kDevice, decoded));
    equal(decoded, wanted);
    seed(legacy);
    load(LegacyContextLoad::Ready, wanted);
    seed(escaped);
    load(LegacyContextLoad::Ready, wanted);
}

void legacyNormalization() {
    for (const size_t expanded : {size_t(2047), size_t(2048)}) {
        scenario("dense control expansion boundary " + std::to_string(expanded), [&] {
            const auto name = std::string(250, '\x01');
            auto legacy = withRawName(name);
            const auto normalized = escapedControls(legacy);
            assert(normalized.size() < expanded);
            legacy += std::string(expanded - normalized.size(), ' ');
            auto wanted = sample();
            std::strcpy(wanted.babyName, name.c_str());
            seed(legacy);
            load(expanded == 2047 ? LegacyContextLoad::Ready : LegacyContextLoad::Corrupt, wanted);
        });
    }
    std::string allControls;
    for (unsigned c = 1; c < 32; ++c) {
        if (c == 8 || c == 9 || c == 10 || c == 12 || c == 13) continue;
        allControls += char(c);
        scenario("old serializer uncommon control " + std::to_string(c), [&] {
            legacyRoundtrip(std::string("name") + char(c), std::string("brand") + char(c));
        });
        for (bool trailing : {false, true}) {
            scenario("uncommon control outside string " + std::to_string(c) +
                     (trailing ? " after" : " before"), [&] {
                rejected(trailing ? kActive + char(c) : std::string(1, char(c)) + kActive);
            });
        }
        scenario("unpaired backslash then raw control " + std::to_string(c), [&] {
            rejected(withRawName(std::string("A\\") + char(c) + "B"));
        });
    }
    scenario("all uncommon controls plus escaped common controls and non-ASCII", [&] {
        legacyRoundtrip(allControls + "\b\t\n\f\r\xe4\xb8\xad", "brand" + allControls);
    });
    for (unsigned slashes = 0; slashes < 6; ++slashes) {
        scenario("serialized backslash parity and escaped quotes " + std::to_string(slashes), [&] {
            const auto prefix = std::string(slashes, '\\');
            legacyRoundtrip(prefix + '"' + '\x01' + "\\/end",
                            prefix + '\x1f' + "\"quoted\"\\");
        });
    }
    scenario("escaped quote cannot hide uncommon control after real string end", [] {
        const auto payload = changed([](JsonObject object) { object["baby_name"] = "a\\\""; });
        auto malformed = payload;
        const auto end = malformed.find(",\"formula_brand\"");
        assert(end != std::string::npos);
        malformed.insert(end, 1, '\x01');
        rejected(malformed);
    });
    for (unsigned c : {0u, 8u, 9u, 10u, 12u, 13u}) {
        scenario("raw NUL/common control is not repaired " + std::to_string(c), [&] {
            rejected(withRawName(std::string("A\x01") + char(c) + "B"));
        });
    }
    scenario("legitimate JSON whitespace outside strings stays unchanged", [] {
        seed("\r\n\t " + kActive + "\r\n\t ");
        load(LegacyContextLoad::Ready);
    });
    for (unsigned c : {8u, 12u}) {
        scenario("raw non-JSON whitespace outside strings " + std::to_string(c), [&] {
            rejected(kActive + char(c));
        });
    }
    for (size_t expanded : {2047u, 2048u}) {
        scenario("normalization encoded length boundary " + std::to_string(expanded), [&] {
            auto legacy = withRawName("A\x01");
            const auto canonical = escapedControls(legacy);
            legacy += std::string(expanded - canonical.size(), ' ');
            assert(legacy.size() + 1 <= 2048);
            seed(legacy);
            auto wanted = sample();
            std::strcpy(wanted.babyName, "A\x01");
            load(expanded == 2047 ? LegacyContextLoad::Ready : LegacyContextLoad::Corrupt, wanted);
        });
    }
    scenario("original NVS size limit remains 2048 including NUL", [] {
        auto legacy = withRawName("A\x01");
        legacy += std::string(2048 - legacy.size(), ' ');
        seed(legacy);
        load(LegacyContextLoad::Corrupt);
        assert(fake::count(Op::Read) == 0);
    });
    scenario("maximum original NVS string fails if normalization would expand it", [] {
        auto legacy = withRawName("A\x01");
        legacy += std::string(2047 - legacy.size(), ' ');
        seed(legacy);
        load(LegacyContextLoad::Corrupt);
        assert(fake::count(Op::Read) == 1);
    });
    for (bool cleared : {false, true}) {
        scenario("unchanged context/tombstone retains direct decode behavior", [&] {
            seed(cleared ? kTombstone : kActive);
            load(LegacyContextLoad::Ready, sample(cleared));
        });
    }
    for (unsigned kind = 0; kind < 10; ++kind) {
        scenario("normalization does not repair other invalid JSON/schema " + std::to_string(kind), [&] {
            auto json = withRawName("A\x01");
            if (kind == 0) json.insert(json.size() - 1, ",\"profile_version\":54321");
            if (kind == 1) json.insert(json.size() - 1, ",\"unknown\":1");
            if (kind == 2) json.insert(json.size() - 1, ",");
            if (kind == 3) json += "{}";
            if (kind == 4) json.replace(json.find("180"), 3, "true");
            if (kind == 5) json.replace(json.find("feeding_context"), 15, "wrong_context");
            if (kind == 6) json.replace(json.find("A\x01"), 2, "A\x01\\q");
            if (kind == 7) json.replace(json.find("A\x01"), 2, "A\x01\\ud800");
            if (kind == 8) json.replace(json.find("A\x01"), 2, "A\x01\xc0\xaf");
            if (kind == 9) json.replace(json.find("180"), 3, "{\"nested\":1}");
            rejected(json);
        });
    }
}
}  // namespace

int main() {
    successAndAbsence();
    storageFailures();
    codecFailures();
    legacyNormalization();
    std::printf("Legacy context storage: %u scenarios passed; production codec, readonly "
                "STRING NVS, SDK faults, full context/tombstone and atomic output\n", scenarios);
}
