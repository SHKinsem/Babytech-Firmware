#include "BoardPairingStore.h"

#ifdef ARDUINO
#include <esp_mac.h>
#include <nvs.h>
#include <cstring>

namespace babytech { namespace boardlink {
namespace {
PairingLoad readRecord(nvs_handle_t handle, v4::Pairing& output) {
    size_t length = 0;
    esp_err_t error = nvs_get_blob(handle, "record", nullptr, &length);
    if (error == ESP_ERR_NVS_NOT_FOUND) return PairingLoad::Missing;
    if (error != ESP_OK) return PairingLoad::IoError;
    if (!length || length > kPairingRecordMaxSize) return PairingLoad::Corrupt;
    uint8_t bytes[kPairingRecordMaxSize];
    const size_t expectedLength = length;
    error = nvs_get_blob(handle, "record", bytes, &length);
    if (error != ESP_OK || length != expectedLength) return PairingLoad::IoError;
    return decodePairingRecord(bytes, length, output) ? PairingLoad::Ready : PairingLoad::Corrupt;
}

PairingLoad matchesHardware(v4::Role role, const v4::Pairing& pairing) {
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return PairingLoad::IoError;
    char physicalId[13];
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(mac); ++i) {
        physicalId[i * 2] = hex[mac[i] >> 4];
        physicalId[i * 2 + 1] = hex[mac[i] & 15];
    }
    physicalId[12] = 0;
    return pairing.role == role && !std::strcmp(pairing.localPhysicalId, physicalId)
        ? PairingLoad::Ready : PairingLoad::IdentityMismatch;
}

bool samePairing(const v4::Pairing& left, const v4::Pairing& right) {
    return left.role == right.role && !std::strcmp(left.deviceId, right.deviceId) &&
        !std::strcmp(left.epoch, right.epoch) &&
        !std::strcmp(left.localPhysicalId, right.localPhysicalId) &&
        !std::strcmp(left.peerPhysicalId, right.peerPhysicalId);
}
}

PairingLoad verifyBoardPairing(v4::Role role, const v4::Pairing& pairing) {
    if (!v4::validPairing(pairing)) return PairingLoad::Corrupt;
    return matchesHardware(role, pairing);
}

PairingLoad loadBoardPairing(v4::Role role, v4::Pairing& output) {
    nvs_handle_t handle;
    const esp_err_t error = nvs_open("productpair", NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return PairingLoad::Missing;
    if (error != ESP_OK) return PairingLoad::IoError;
    v4::Pairing pairing;
    const PairingLoad read = readRecord(handle, pairing);
    nvs_close(handle);
    if (read != PairingLoad::Ready) return read;
    const PairingLoad identity = matchesHardware(role, pairing);
    if (identity != PairingLoad::Ready) return identity;
    output = pairing;
    return PairingLoad::Ready;
}

PairingInstall PairingInstaller::installFirst(v4::Role role, const v4::Pairing& pairing) {
    if (faulted_) return PairingInstall::StorageFault;
    uint8_t bytes[kPairingRecordMaxSize];
    const size_t length = encodePairingRecord(pairing, bytes, sizeof(bytes));
    if (!length) return PairingInstall::Invalid;
    const auto identity = matchesHardware(role, pairing);
    if (identity == PairingLoad::IdentityMismatch) return PairingInstall::IdentityMismatch;
    if (identity != PairingLoad::Ready) {
        faulted_ = true;
        return PairingInstall::StorageFault;
    }
    v4::Pairing existing;
    const PairingLoad loaded = loadBoardPairing(role, existing);
    if (loaded == PairingLoad::Ready)
        return samePairing(existing, pairing) ? PairingInstall::AlreadyInstalled : PairingInstall::Conflict;
    if (loaded == PairingLoad::IdentityMismatch) return PairingInstall::Conflict;
    if (loaded != PairingLoad::Missing) {
        faulted_ = true;
        return PairingInstall::StorageFault;
    }
    nvs_handle_t handle;
    if (nvs_open("productpair", NVS_READWRITE, &handle) != ESP_OK) {
        faulted_ = true;
        return PairingInstall::StorageFault;
    }
    // Recheck through the write handle: never replace a record that appeared
    // between inspection and opening. This is not a multi-writer transaction.
    const PairingLoad rechecked = readRecord(handle, existing);
    if (rechecked != PairingLoad::Missing) {
        nvs_close(handle);
        if (rechecked == PairingLoad::Ready)
            return samePairing(existing, pairing) ? PairingInstall::AlreadyInstalled : PairingInstall::Conflict;
        faulted_ = true;
        return PairingInstall::StorageFault;
    }
    const esp_err_t written = nvs_set_blob(handle, "record", bytes, length);
    const esp_err_t committed = written == ESP_OK ? nvs_commit(handle) : written;
    nvs_close(handle);
    if (written != ESP_OK || committed != ESP_OK ||
        loadBoardPairing(role, existing) != PairingLoad::Ready || !samePairing(existing, pairing)) {
        faulted_ = true;
        return PairingInstall::StorageFault;
    }
    return PairingInstall::Installed;
}

} }
#endif
