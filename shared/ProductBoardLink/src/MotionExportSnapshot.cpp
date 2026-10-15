#include "MotionExportSnapshot.h"
#include "FlatJsonGuard.h"

#include <ArduinoJson.h>
#include <cstring>
#include <memory>
#include <new>

static_assert(ARDUINOJSON_VERSION_MAJOR == 6, "Motion export requires ArduinoJson 6");

namespace babytech { namespace boardlink {
namespace {
constexpr char kPrefix[] = "[maint-export] ";
constexpr size_t kFields = 14;
constexpr const char* kKeys[] = {
    "schema", "role", "device_id", "physical_id", "boot", "challenge", "captured_ms",
    "pair_status", "state_status", "legacy_status", "legacy_event",
    "pair_hex", "state_hex", "legacy_hex"
};

int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
bool nonzeroHex(const char* value, size_t length) {
    if (!value) return false;
    bool nonzero = false;
    for (size_t n = 0; n < length; ++n) {
        if (nibble(value[n]) < 0) return false;
        nonzero = nonzero || value[n] != '0';
    }
    return nonzero && value[length] == 0;
}
bool deviceId(const char* value) {
    if (!value) return false;
    size_t n = 0;
    for (; n < 65 && value[n]; ++n) {
        const char c = value[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || (n && (c == '_' || c == '-')))) return false;
    }
    return n > 0 && n <= 64;
}
bool equalText(JsonVariantConst value, const char* expected) {
    if (!value.is<JsonString>()) return false;
    const auto text = value.as<JsonString>();
    const size_t length = std::strlen(expected);
    return text.size() == length && !std::memcmp(text.c_str(), expected, length);
}
enum class Field { Pair, State, Legacy, Event };
bool readStatus(JsonVariantConst value, Field field, ExportRead& output) {
    struct Status { const char* name; ExportRead value; };
    static constexpr Status statuses[] = {
        {"ready", ExportRead::Ready}, {"missing", ExportRead::Missing},
        {"corrupt", ExportRead::Corrupt}, {"io_error", ExportRead::IoError},
        {"identity_mismatch", ExportRead::IdentityMismatch}, {"conflict", ExportRead::Conflict},
        {"present", ExportRead::Present}
    };
    for (const auto& status : statuses) {
        if (!equalText(value, status.name)) continue;
        const auto result = status.value;
        if ((result == ExportRead::Ready && field == Field::Event) ||
            (result == ExportRead::Present && field != Field::Event) ||
            (result == ExportRead::Conflict && field != Field::State) ||
            (result == ExportRead::IdentityMismatch && field != Field::Pair && field != Field::State))
            return false;
        output = result;
        return true;
    }
    return false;
}
bool unhex(JsonVariantConst value, ExportRead status, uint8_t* bytes,
           size_t capacity, size_t& length) {
    if (!value.is<JsonString>()) return false;
    const auto text = value.as<JsonString>();
    length = text.size() / 2;
    if (status != ExportRead::Ready) return text.size() == 0;
    if (!text.size() || text.size() % 2 || length > capacity) return false;
    for (size_t n = 0; n < length; ++n) {
        const int high = nibble(text.c_str()[2 * n]);
        const int low = nibble(text.c_str()[2 * n + 1]);
        if (high < 0 || low < 0) return false;
        bytes[n] = uint8_t(high * 16 + low);
    }
    return true;
}
bool matches(const v4::Pairing& pair, const char* device, const char* physical) {
    return pair.role == v4::Role::Motion && !std::strcmp(pair.deviceId, device) &&
        !std::strcmp(pair.localPhysicalId, physical);
}
bool samePair(const v4::Pairing& a, const v4::Pairing& b) {
    return a.role == b.role && !std::strcmp(a.deviceId, b.deviceId) &&
        !std::strcmp(a.localPhysicalId, b.localPhysicalId) &&
        !std::strcmp(a.peerPhysicalId, b.peerPhysicalId) && !std::strcmp(a.epoch, b.epoch);
}
}

bool decodeMotionExport(char* bytes, size_t length, const char* expectedDevice,
                        const char* expectedPhysical, uint64_t expectedBoot,
                        const char* expectedChallenge, MotionExportSnapshot& output) {
    constexpr size_t prefixLength = sizeof(kPrefix) - 1;
    if (!bytes || length <= prefixLength || length > kMotionExportWireMaxSize ||
        bytes[length - 1] != '\n' || std::memcmp(bytes, kPrefix, prefixLength) ||
        std::memchr(bytes, 0, length) || !deviceId(expectedDevice) ||
        !nonzeroHex(expectedPhysical, 12) || !expectedBoot || !nonzeroHex(expectedChallenge, 32))
        return false;
    char* json = bytes + prefixLength;
    const size_t jsonLength = length - prefixLength - 1;
    // ArduinoJson accepts relaxed syntax and stops at the first object. Check
    // the complete wire grammar before its zero-copy parser mutates the input.
    size_t fields = 0;
    if (std::memchr(json, '\n', jsonLength) || std::memchr(json, '\r', jsonLength) ||
        !detail::FlatJsonGuard(reinterpret_cast<const uint8_t*>(json), jsonLength, kFields).object(fields) ||
        fields != kFields) return false;
    DynamicJsonDocument doc(JSON_OBJECT_SIZE(kFields));
    if (deserializeJson(doc, json, jsonLength, DeserializationOption::NestingLimit(1)) ||
        doc.overflowed() || !doc.is<JsonObject>() || doc.size() != fields) return false;
    const auto object = doc.as<JsonObjectConst>();
    for (const char* key : kKeys) if (!object.containsKey(key)) return false;
    if (!object["schema"].is<uint32_t>() || object["schema"].as<uint32_t>() != 1 ||
        !object["captured_ms"].is<uint32_t>() || !equalText(object["role"], "motion") ||
        !equalText(object["device_id"], expectedDevice) ||
        !equalText(object["physical_id"], expectedPhysical) ||
        !equalText(object["challenge"], expectedChallenge) || !object["boot"].is<JsonString>())
        return false;
    const auto bootText = object["boot"].as<JsonString>();
    if (bootText.size() != 16 || !nonzeroHex(bootText.c_str(), 16)) return false;
    uint64_t boot = 0;
    for (size_t n = 0; n < 16; ++n) boot = (boot << 4) | unsigned(nibble(bootText.c_str()[n]));
    if (boot != expectedBoot) return false;

    std::unique_ptr<MotionExportSnapshot> candidate(new (std::nothrow) MotionExportSnapshot);
    if (!candidate) return false;
    auto& parsed = *candidate;
    if (!readStatus(object["pair_status"], Field::Pair, parsed.pairStatus) ||
        !readStatus(object["state_status"], Field::State, parsed.stateStatus) ||
        !readStatus(object["legacy_status"], Field::Legacy, parsed.legacyStatus) ||
        !readStatus(object["legacy_event"], Field::Event, parsed.legacyEvent)) return false;
    static_assert(kPairingRecordMaxSize <= kMotionStateMaxSize &&
                  kContextIdentityMaxSize <= kMotionStateMaxSize, "shared hex scratch budget");
    std::unique_ptr<uint8_t[]> scratch(new (std::nothrow) uint8_t[kMotionStateMaxSize]);
    if (!scratch) return false;
    size_t size = 0;
    if (!unhex(object["pair_hex"], parsed.pairStatus, scratch.get(), kPairingRecordMaxSize, size) ||
        (parsed.pairStatus == ExportRead::Ready &&
         (!decodePairingRecord(scratch.get(), size, parsed.pairing) ||
          !matches(parsed.pairing, expectedDevice, expectedPhysical)))) return false;
    if (!unhex(object["state_hex"], parsed.stateStatus, scratch.get(), kMotionStateMaxSize, size) ||
        (parsed.stateStatus == ExportRead::Ready &&
         (!decodeMotionState(scratch.get(), size, parsed.state) ||
          !matches(parsed.state.pairing, expectedDevice, expectedPhysical)))) return false;
    if (parsed.pairStatus == ExportRead::Ready && parsed.stateStatus == ExportRead::Ready &&
        !samePair(parsed.pairing, parsed.state.pairing)) return false;
    if (!unhex(object["legacy_hex"], parsed.legacyStatus, scratch.get(), kContextIdentityMaxSize, size) ||
        (parsed.legacyStatus == ExportRead::Ready &&
         (!decodeContextIdentity(scratch.get(), size, parsed.legacy) ||
          std::strcmp(parsed.legacy.deviceId, expectedDevice)))) return false;
    std::strcpy(parsed.deviceId, expectedDevice);
    std::strcpy(parsed.physicalId, expectedPhysical);
    std::strcpy(parsed.challenge, expectedChallenge);
    parsed.boot = boot;
    parsed.capturedAtMs = object["captured_ms"].as<uint32_t>();
    output = parsed;
    return true;
}

} }
