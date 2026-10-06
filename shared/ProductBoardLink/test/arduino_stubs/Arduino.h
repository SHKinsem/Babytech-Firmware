#pragma once

#include <cstddef>
#include <cstdint>

constexpr uint32_t SERIAL_8N1 = 0x800001c;

class HardwareSerial {
public:
    explicit HardwareSerial(uint8_t number);
    size_t setRxBufferSize(size_t size);
    size_t setTxBufferSize(size_t size);
    void begin(unsigned long baud, uint32_t config, int8_t rxPin, int8_t txPin);
    explicit operator bool() const;
    int available();
    int read();
    int availableForWrite();
    size_t write(const uint8_t* bytes, size_t size);
};
