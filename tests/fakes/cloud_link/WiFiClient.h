#pragma once
#include <cstdint>

class IPAddress {
public:
    explicit IPAddress(uint32_t value = 0) : value_(value) {}
    IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
        : value_((uint32_t(a) << 24) | (uint32_t(b) << 16) | (uint32_t(c) << 8) | d) {}
    bool operator==(const IPAddress& other) const { return value_ == other.value_; }
    bool operator!=(const IPAddress& other) const { return !(*this == other); }
private:
    uint32_t value_;
};

class WiFiClient {
public:
    IPAddress localIP() const { return local; }
    IPAddress local{0xC0A80401};
};
