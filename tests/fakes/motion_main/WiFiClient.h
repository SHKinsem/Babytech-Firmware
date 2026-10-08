#pragma once

#include <Arduino.h>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

class IPAddress {
public:
    explicit IPAddress(uint32_t value = 0) : value_(value) {}
    IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
        : value_((uint32_t(a) << 24) | (uint32_t(b) << 16) |
                 (uint32_t(c) << 8) | uint32_t(d)) {}
    bool operator==(const IPAddress& other) const { return value_ == other.value_; }
    bool operator!=(const IPAddress& other) const { return !(*this == other); }
    uint8_t operator[](size_t index) const {
        return index < 4 ? static_cast<uint8_t>(value_ >> (24 - 8 * index)) : 0;
    }
    String toString() const {
        std::string text;
        for (size_t i = 0; i < 4; ++i) {
            if (i) text += '.';
            text += std::to_string((*this)[i]);
        }
        return String(text.c_str());
    }

private:
    uint32_t value_;
};

// In-memory SDK boundary only. No sockets and no FakeCloudIo dependency.
class WiFiClient {
public:
    IPAddress localIP() const { return local; }
    IPAddress remoteIP() const { return remote; }
    uint8_t connected() const { return isConnected ? 1 : 0; }
    explicit operator bool() const { return isConnected; }
    int connect(const char* host, uint16_t port) {
        attemptedHost = host ? host : "";
        attemptedPort = port;
        ++connectCalls;
        isConnected = connectOk;
        return isConnected ? 1 : 0;
    }
    int connect(IPAddress ip, uint16_t port) {
        remote = ip;
        return connect(ip.toString().c_str(), port);
    }
    void stop() { isConnected = false; ++stopCalls; }
    int available() const { return static_cast<int>(incoming.size()); }
    int peek() const { return incoming.empty() ? -1 : incoming.front(); }
    int read() {
        if (incoming.empty()) return -1;
        const uint8_t byte = incoming.front();
        incoming.pop_front();
        return byte;
    }
    int read(uint8_t* bytes, size_t capacity) {
        if (!bytes) return 0;
        size_t count = 0;
        while (count < capacity && !incoming.empty()) bytes[count++] = static_cast<uint8_t>(read());
        return static_cast<int>(count);
    }
    size_t write(const uint8_t* bytes, size_t length) {
        if (!isConnected || !writeOk || (!bytes && length)) return 0;
        if (length) written.insert(written.end(), bytes, bytes + length);
        return length;
    }
    size_t write(uint8_t byte) { return write(&byte, 1); }
    void flush() { incoming.clear(); }

    IPAddress local{192, 168, 4, 1};
    IPAddress remote{192, 168, 4, 2};
    bool isConnected = false;
    bool connectOk = false;
    bool writeOk = true;
    unsigned connectCalls = 0;
    unsigned stopCalls = 0;
    std::string attemptedHost;
    uint16_t attemptedPort = 0;
    std::deque<uint8_t> incoming;
    std::vector<uint8_t> written;
};
