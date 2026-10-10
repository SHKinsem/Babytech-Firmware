#include "MaintenanceExport.h"
#include <cstring>

namespace babytech { namespace boardlink {
namespace {
bool validDevice(const char* text) {
    if (!text) return false;
    size_t n = 0;
    for (; n <= 64 && text[n]; ++n) {
        const char c = text[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || (n && (c == '_' || c == '-')))) return false;
    }
    return n > 0 && n <= 64;
}
bool validChallenge(const char* text) {
    if (!text) return false;
    bool nonzero = false;
    for (size_t n = 0; n < 32; ++n) {
        const char c = text[n];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        nonzero = nonzero || c != '0';
    }
    return nonzero && text[32] == 0;
}
constexpr char kStateStart[] = "\",\"state_hex\":\"";
constexpr char kLegacyStart[] = "\",\"legacy_hex\":\"";
constexpr char kEnd[] = "\"}\n";
}

ExportCommand parseMaintenanceExport(const char* line, char (&device)[65], char (&challenge)[33]) {
    if (!line || std::strncmp(line, "MAINT EXPORT", 12)) return ExportCommand::NotExport;
    if (line[12] != ' ') return ExportCommand::Invalid;
    const char* start = line + 13;
    const char* delimiter = std::strchr(start, ' ');
    if (!delimiter || delimiter == start || size_t(delimiter - start) > 64) return ExportCommand::Invalid;
    char proposed[65]{};
    std::memcpy(proposed, start, size_t(delimiter - start));
    if (!validDevice(proposed) || !validChallenge(delimiter + 1)) return ExportCommand::Invalid;
    std::memcpy(device, proposed, sizeof(device));
    std::memcpy(challenge, delimiter + 1, sizeof(challenge));
    return ExportCommand::Valid;
}

void MaintenanceExport::cancel() {
    active_ = false;
    position_ = total_ = headerSize_ = pairSize_ = stateSize_ = legacySize_ = 0;
    // Exports may include a child's name; do not retain them after transfer.
    std::memset(pair_, 0, sizeof(pair_));
    std::memset(state_, 0, sizeof(state_));
    std::memset(legacy_, 0, sizeof(legacy_));
    std::memset(header_, 0, sizeof(header_));
}

uint8_t MaintenanceExport::at(size_t position) const {
    if (position < headerSize_) return static_cast<uint8_t>(header_[position]);
    position -= headerSize_;
    const uint8_t* arrays[] = {pair_, state_, legacy_};
    const size_t sizes[] = {pairSize_, stateSize_, legacySize_};
    const char* tails[] = {kStateStart, kLegacyStart, kEnd};
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned n = 0; n < 3; ++n) {
        if (position < sizes[n] * 2) {
            const uint8_t byte = arrays[n][position / 2];
            return static_cast<uint8_t>(hex[position % 2 ? byte & 15 : byte >> 4]);
        }
        position -= sizes[n] * 2;
        const size_t tailSize = std::strlen(tails[n]);
        if (position < tailSize) return static_cast<uint8_t>(tails[n][position]);
        position -= tailSize;
    }
    return 0;
}

size_t MaintenanceExport::peek(uint8_t* output, size_t capacity) const {
    if (!active_ || !output) return 0;
    const size_t remaining = total_ - position_;
    const size_t length = remaining < capacity ? remaining : capacity;
    for (size_t n = 0; n < length; ++n) output[n] = at(position_ + n);
    return length;
}

void MaintenanceExport::consume(size_t length) {
    if (!active_ || length > total_ - position_) { cancel(); return; }
    position_ += length;
    if (position_ == total_) cancel();
}

} }

#ifdef ARDUINO
#include "BoardPairingStore.h"
#include "BrainStateRecord.h"
#include "LegacyContextStore.h"
#include <esp_mac.h>
#include <nvs.h>
#include <cstdio>
#include <memory>
#include <new>

namespace babytech { namespace boardlink {
namespace {
enum class Read { Ready, Missing, Corrupt, IoError, IdentityMismatch, Conflict, NotApplicable };
static_assert(kBrainStateMaxSize <= kMotionStateMaxSize, "shared capture buffer holds both records");
const char* readName(Read value) {
    switch (value) {
        case Read::Ready: return "ready";
        case Read::Missing: return "missing";
        case Read::Corrupt: return "corrupt";
        case Read::IdentityMismatch: return "identity_mismatch";
        case Read::Conflict: return "conflict";
        case Read::NotApplicable: return "not_applicable";
        default: return "io_error";
    }
}
Read pairRead(PairingLoad result) {
    switch (result) {
        case PairingLoad::Ready: return Read::Ready;
        case PairingLoad::Missing: return Read::Missing;
        case PairingLoad::Corrupt: return Read::Corrupt;
        case PairingLoad::IdentityMismatch: return Read::IdentityMismatch;
        default: return Read::IoError;
    }
}
bool samePair(const v4::Pairing& a, const v4::Pairing& b) {
    return a.role == b.role && !std::strcmp(a.deviceId, b.deviceId) && !std::strcmp(a.epoch, b.epoch) &&
        !std::strcmp(a.localPhysicalId, b.localPhysicalId) && !std::strcmp(a.peerPhysicalId, b.peerPhysicalId);
}
Read readBlob(const char* name, uint8_t* bytes, size_t capacity, size_t& outputLength) {
    nvs_handle_t handle;
    const auto opened = nvs_open(name, NVS_READONLY, &handle);
    if (opened == ESP_ERR_NVS_NOT_FOUND) return Read::Missing;
    if (opened != ESP_OK) return Read::IoError;
    size_t length = 0;
    auto error = nvs_get_blob(handle, "record", nullptr, &length);
    if (error != ESP_OK || !length || length > capacity) {
        nvs_close(handle);
        return error == ESP_ERR_NVS_NOT_FOUND ? Read::Missing :
            error != ESP_OK ? Read::IoError : Read::Corrupt;
    }
    const size_t expected = length;
    error = nvs_get_blob(handle, "record", bytes, &length);
    nvs_close(handle);
    if (error != ESP_OK || length != expected) return Read::IoError;
    outputLength = length;
    return Read::Ready;
}
Read inspectLegacyEvent() {
    nvs_handle_t handle;
    const auto opened = nvs_open("formulaevt", NVS_READONLY, &handle);
    if (opened == ESP_ERR_NVS_NOT_FOUND) return Read::Missing;
    if (opened != ESP_OK) return Read::IoError;
    size_t length = 0;
    const auto error = nvs_get_str(handle, "payload", nullptr, &length);
    nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return Read::Missing;
    if (error != ESP_OK) return Read::IoError;
    // "present" is deliberately not called a successfully decoded event.
    return length > 1 && length <= 2048 ? Read::Ready : Read::Corrupt;
}
}

bool MaintenanceExport::begin(v4::Role role, const char* device, const char* challenge,
                              uint64_t boot, uint32_t nowMs) {
    if (active_ || !boot || !validDevice(device) || !validChallenge(challenge) ||
        (role != v4::Role::Brain && role != v4::Role::Motion)) return false;
    cancel();
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return false;
    char physical[13];
    std::snprintf(physical, sizeof(physical), "%02x%02x%02x%02x%02x%02x",
        unsigned(mac[0]), unsigned(mac[1]), unsigned(mac[2]), unsigned(mac[3]), unsigned(mac[4]), unsigned(mac[5]));
    if (!std::strcmp(physical, "000000000000")) return false;
    v4::Pairing pair;
    Read pairStatus = pairRead(loadBoardPairing(role, pair));
    if (pairStatus == Read::Ready && (std::strcmp(pair.deviceId, device) ||
        std::strcmp(pair.localPhysicalId, physical))) pairStatus = Read::IdentityMismatch;
    if (pairStatus == Read::Ready) pairSize_ = encodePairingRecord(pair, pair_, sizeof(pair_));
    if (pairStatus == Read::Ready && !pairSize_) pairStatus = Read::Corrupt;

    Read stateStatus = readBlob(role == v4::Role::Brain ? "brainstate" : "productstate",
                               state_, sizeof(state_), stateSize_);
    if (stateStatus == Read::Ready) {
        v4::Pairing statePair;
        bool valid = false;
        if (role == v4::Role::Brain) {
            std::unique_ptr<BrainState> decoded(new (std::nothrow) BrainState);
            if (!decoded) { cancel(); return false; }
            valid = decodeBrainState(state_, stateSize_, *decoded);
            if (valid) statePair = decoded->pairing;
        } else {
            std::unique_ptr<MotionState> decoded(new (std::nothrow) MotionState);
            if (!decoded) { cancel(); return false; }
            valid = decodeMotionState(state_, stateSize_, *decoded);
            if (valid) statePair = decoded->pairing;
        }
        if (!valid) stateStatus = Read::Corrupt;
        else if (statePair.role != role || std::strcmp(statePair.deviceId, device) ||
                 std::strcmp(statePair.localPhysicalId, physical)) stateStatus = Read::IdentityMismatch;
        else if (pairStatus == Read::Ready && !samePair(pair, statePair)) stateStatus = Read::Conflict;
    }
    if (stateStatus != Read::Ready) { stateSize_ = 0; std::memset(state_, 0, sizeof(state_)); }

    Read legacyStatus = Read::NotApplicable;
    Read eventStatus = Read::NotApplicable;
    if (role == v4::Role::Motion) {
        eventStatus = inspectLegacyEvent();
        std::unique_ptr<ProductContext> legacy(new (std::nothrow) ProductContext);
        if (!legacy) { cancel(); return false; }
        const auto loaded = loadLegacyProductContext(device, *legacy);
        legacyStatus = loaded == LegacyContextLoad::Ready ? Read::Ready :
            loaded == LegacyContextLoad::Missing ? Read::Missing :
            loaded == LegacyContextLoad::Corrupt ? Read::Corrupt : Read::IoError;
        if (legacyStatus == Read::Ready) {
            legacySize_ = encodeContextIdentity(*legacy, legacy_, sizeof(legacy_));
            if (!legacySize_) legacyStatus = Read::Corrupt;
        }
    }
    const int length = std::snprintf(header_, sizeof(header_),
        "[maint-export] {\"schema\":1,\"role\":\"%s\",\"device_id\":\"%s\","
        "\"physical_id\":\"%s\",\"boot\":\"%016llx\",\"challenge\":\"%s\",\"captured_ms\":%lu,"
        "\"pair_status\":\"%s\",\"state_status\":\"%s\",\"legacy_status\":\"%s\","
        "\"legacy_event\":\"%s\",\"pair_hex\":\"",
        role == v4::Role::Brain ? "brain" : "motion", device, physical,
        static_cast<unsigned long long>(boot), challenge, static_cast<unsigned long>(nowMs),
        readName(pairStatus), readName(stateStatus), readName(legacyStatus),
        eventStatus == Read::Ready ? "present" : readName(eventStatus));
    if (length <= 0 || size_t(length) >= sizeof(header_)) { cancel(); return false; }
    headerSize_ = size_t(length);
    total_ = headerSize_ + (pairSize_ + stateSize_ + legacySize_) * 2 +
        std::strlen(kStateStart) + std::strlen(kLegacyStart) + std::strlen(kEnd);
    beganAt_ = nowMs;
    active_ = true;
    return true;
}

} }
#endif
