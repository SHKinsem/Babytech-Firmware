#include "BoardLinkArduino.h"

#ifdef ARDUINO
#include <esp_system.h>
#include <esp_mac.h>
#include <cstring>

namespace babytech { namespace boardlink {

bool ArduinoBoardLink::Sink::idle() const {
    return uart_wait_tx_done(UART_NUM_1, 0) == ESP_OK;
}
size_t ArduinoBoardLink::Sink::available() const {
    const int available = serial_.availableForWrite();
    return available > 0 ? size_t(available) : 0;
}
size_t ArduinoBoardLink::Sink::write(const uint8_t* bytes, size_t length) {
    const size_t room = available();
    return serial_.write(bytes, length < room ? length : room);
}

bool ArduinoBoardLink::begin(v4::Role role, int rxPin, int txPin, uint32_t baud,
                             bool enableDiscovery) {
    // Reinitializing a live UART could truncate a frame. Lifecycle owner must
    // reboot before replacing credentials/pairing or restarting this adapter.
    if (started_) return false;
    link_.reset();
    parser_.reset();
    records_.reset();
    maintenance_ = BoardMaintenance{};
    pairingVerified_ = false;
    deviceId_[0] = 0;
    discoveryEnabled_ = false;
    discovery_ = BoardDiscovery{};
    v4::Pairing pairing;
    pairingState_ = loadBoardPairing(role, pairing);
    if (pairingState_ == PairingLoad::Ready) {
        pairing_ = pairing;
        pairingVerified_ = true;
        std::memcpy(deviceId_, pairing.deviceId, sizeof(deviceId_));
    } else if (!enableDiscovery) return false;
    if (rxPin < 0 || txPin < 0 || rxPin == txPin || baud != 115200) {
        pairingState_ = PairingLoad::UartError;
        return false;
    }
    uint64_t boot = (uint64_t(esp_random()) << 32) | esp_random();
    if (!boot || (pairingVerified_ && !link_.begin(pairing, boot))) {
        pairingState_ = PairingLoad::IoError;
        return false;
    }
    if (enableDiscovery) {
        char physical[13]{};
        DiscoveryPairState state = DiscoveryPairState::Missing;
        if (pairingVerified_) {
            std::memcpy(physical, pairing_.localPhysicalId, sizeof(physical));
            state = DiscoveryPairState::Ready;
        } else {
            uint8_t mac[6];
            if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
                pairingState_ = PairingLoad::IoError;
                return false;
            }
            constexpr char hex[] = "0123456789abcdef";
            for (size_t n = 0; n < sizeof(mac); ++n) {
                physical[2 * n] = hex[mac[n] >> 4];
                physical[2 * n + 1] = hex[mac[n] & 15];
            }
            switch (pairingState_) {
                case PairingLoad::Missing: break;
                case PairingLoad::Corrupt: state = DiscoveryPairState::Corrupt; break;
                case PairingLoad::IdentityMismatch: state = DiscoveryPairState::IdentityMismatch; break;
                default: state = DiscoveryPairState::IoError; break;
            }
        }
        if (!discovery_.begin(role, physical, boot, state, verifiedPairing())) {
            pairingState_ = PairingLoad::IoError;
            return false;
        }
        if (!records_.begin(role, physical, boot)) {
            pairingState_ = PairingLoad::IoError;
            return false;
        }
        if (!maintenance_.begin(role, physical, boot, verifiedPairing())) {
            pairingState_ = PairingLoad::IoError;
            return false;
        }
        discoveryEnabled_ = true;
    }
    // Both pinned SDKs construct HardwareSerial with TX ring size zero. Keep
    // that default: Arduino 2 rejects setTxBufferSize(0) and emits an error.
    if (serial_.setRxBufferSize(256) != 256) {
        pairingState_ = PairingLoad::UartError;
        return false;
    }
    serial_.begin(baud, SERIAL_8N1, rxPin, txPin);
    started_ = bool(serial_);
    if (!started_) pairingState_ = PairingLoad::UartError;
    return started_;
}

void ArduinoBoardLink::poll(uint32_t nowMs, const Status* localStatus) {
    if (!started_) return;
    // Bound one pass so a noisy peer cannot starve LVGL or CAN servicing.
    for (size_t count = 0; count < 256 && serial_.available() > 0; ++count) {
        const int byte = serial_.read();
        if (byte < 0) break;
        v4::Frame frame;
        if (!parser_.push(uint8_t(byte), nowMs, frame)) continue;
        if (frame.kind == v4::Kind::Discovery) {
            if (discoveryEnabled_) discovery_.receive(frame, nowMs);
        } else if (frame.kind == v4::Kind::MigrationRead) {
            if (discoveryEnabled_) records_.receive(frame, nowMs);
        } else if (frame.kind == v4::Kind::MigrationMaintenance) {
            if (discoveryEnabled_) maintenance_.receive(frame, nowMs);
        } else link_.receiveFrame(frame, nowMs);
    }
    if (discoveryEnabled_) {
        maintenance_.poll(nowMs);
        const auto* support = maintenance_.outgoing();
        if (support && link_.queueSupportFrame(*support)) maintenance_.queued();
        discovery_.poll(nowMs);
        const auto* outgoing = discovery_.outgoing();
        if (outgoing && link_.queueSupportFrame(*outgoing)) discovery_.queued();
        records_.poll(nowMs);
        outgoing = records_.outgoing();
        if (outgoing && link_.queueSupportFrame(*outgoing)) records_.queued();
    }
    link_.poll(nowMs, sink_, localStatus);
}

bool ArduinoBoardLink::requestRecords(const char* device, uint32_t nowMs) {
    if (!started_ || !discoveryEnabled_ || discovery_.result().state != DiscoveryState::Found) return false;
    char challenge[33]{};
    constexpr char hex[] = "0123456789abcdef";
    for (size_t word = 0; word < 4; ++word) {
        const uint32_t random = esp_random();
        for (size_t nibble = 0; nibble < 8; ++nibble)
            challenge[word * 8 + nibble] = hex[(random >> (28 - 4 * nibble)) & 15];
    }
    return records_.request(device, discovery_.result(), challenge, nowMs);
}

bool ArduinoBoardLink::requestMaintenance(const char* device, uint32_t nowMs) {
    if (!started_ || !discoveryEnabled_ || discovery_.result().state != DiscoveryState::Found) return false;
    char nonce[33]{};
    constexpr char hex[] = "0123456789abcdef";
    for (size_t word = 0; word < 4; ++word) {
        const uint32_t random = esp_random();
        for (size_t nibble = 0; nibble < 8; ++nibble)
            nonce[word * 8 + nibble] = hex[(random >> (28 - 4 * nibble)) & 15];
    }
    return maintenance_.request(device, discovery_.result(), nonce, nowMs);
}

} }
#endif
