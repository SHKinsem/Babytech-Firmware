#pragma once
#include "WiFiClient.h"
#include <algorithm>
#include <vector>

class WiFiUDP {
public:
    WiFiUDP() { instances.push_back(this); }
    ~WiFiUDP() { instances.erase(std::remove(instances.begin(), instances.end(), this), instances.end()); }
    static inline std::vector<WiFiUDP*> instances;
    int begin(IPAddress address, uint16_t port) { bound = address; boundPort = port; return 1; }
    void stop() { boundPort = 0; }
    int parsePacket() { return int(incoming.size()); }
    int read(uint8_t* output, size_t size) {
        const size_t count = std::min(size, incoming.size());
        std::copy(incoming.begin(), incoming.begin() + count, output);
        incoming.clear();
        return int(count);
    }
    void clear() { incoming.clear(); }
    IPAddress remoteIP() const { return IPAddress(192, 168, 4, 2); }
    uint16_t remotePort() const { return 12345; }
    void beginPacket(IPAddress, uint16_t) { outgoing.clear(); }
    size_t write(const uint8_t* data, size_t size) { outgoing.assign(data, data + size); return size; }
    void endPacket() {}
    IPAddress bound;
    uint16_t boundPort = 0;
    std::vector<uint8_t> incoming, outgoing;
};
