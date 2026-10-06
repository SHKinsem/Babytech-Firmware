#include "BoardLinkArduino.h"

#ifdef ARDUINO
#include <esp_system.h>
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

bool ArduinoBoardLink::begin(v4::Role role, int rxPin, int txPin, uint32_t baud) {
    // Reinitializing a live UART could truncate a frame. Lifecycle owner must
    // reboot before replacing credentials/pairing or restarting this adapter.
    if (started_) return false;
    v4::Pairing pairing;
    pairingState_ = loadBoardPairing(role, pairing);
    if (pairingState_ != PairingLoad::Ready) return false;
    pairing_ = pairing;
    pairingVerified_ = true;
    std::memcpy(deviceId_, pairing.deviceId, sizeof(deviceId_));
    if (rxPin < 0 || txPin < 0 || rxPin == txPin || baud != 115200) {
        pairingState_ = PairingLoad::UartError;
        return false;
    }
    uint64_t boot = (uint64_t(esp_random()) << 32) | esp_random();
    if (!boot || !link_.begin(pairing, boot)) {
        pairingState_ = PairingLoad::IoError;
        return false;
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
        link_.receive(uint8_t(byte), nowMs);
    }
    link_.poll(nowMs, sink_, localStatus);
}

} }
#endif
