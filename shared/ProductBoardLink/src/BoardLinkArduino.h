#pragma once

#ifdef ARDUINO
#include <Arduino.h>
#include <driver/uart.h>
#include "ReadOnlyBoardLink.h"
#include "BoardPairingStore.h"

namespace babytech { namespace boardlink {

// One instance owns UART1; use static storage and call only from the loop task.
class ArduinoBoardLink {
public:
    bool begin(v4::Role role, int rxPin, int txPin, uint32_t baud = 115200);
    void poll(uint32_t nowMs, const Status* localStatus = nullptr);
    const ReadOnlyLink& link() const { return link_; }
    PairingLoad pairingState() const { return pairingState_; }
    const char* deviceId() const { return deviceId_; }
    // Local recovery must still run if the separately initialized UART fails.
    const v4::Pairing* verifiedPairing() const { return pairingVerified_ ? &pairing_ : nullptr; }
private:
    class Sink : public v4::ByteSink {
    public:
        explicit Sink(HardwareSerial& serial) : serial_(serial) {}
        bool idle() const override;
        size_t available() const override;
        size_t write(const uint8_t* bytes, size_t length) override;
    private:
        HardwareSerial& serial_;
    };
    HardwareSerial serial_{1};
    Sink sink_{serial_};
    ReadOnlyLink link_{};
    PairingLoad pairingState_ = PairingLoad::Missing;
    v4::Pairing pairing_{};
    char deviceId_[65]{};
    bool pairingVerified_ = false;
    bool started_ = false;
};

} }
#endif
