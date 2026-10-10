#pragma once

#include <cstdint>

namespace babytech { namespace cloud {

constexpr uint32_t kSessionLifetimeMs = 24u * 60u * 60u * 1000u;
constexpr uint32_t kCommandTtlMs = 5000;

struct SessionSnapshot {
    char id[33]{};
    uint32_t generation = 0;
    uint32_t uptimeMs = 0;
};

struct ProbeReply {
    SessionSnapshot session;
    char challenge[33]{};
};

enum class Freshness { Current, Disconnected, WrongSession, InvalidTtl, Expired };

// Single owner, no I/O or clocks: the MQTT task supplies fresh random IDs after
// successful authentication/subscription. Serialize calls with its state lock.
// poll/snapshot must run regularly (well within the uint32 clock wrap period).
// This is only the network freshness gate, NOT execution/dedup authorization.
class CloudSession {
public:
    bool open(const char* randomId, uint32_t generation, uint32_t nowMs);
    void close();
    void poll(uint32_t nowMs);
    bool snapshot(uint32_t nowMs, SessionSnapshot& output);
    bool probe(const char* targetSession, const char* challenge,
               uint32_t generation, uint32_t nowMs, ProbeReply& output);
    Freshness check(const char* commandSession, uint32_t generation,
                    uint32_t sampledAtMs, uint32_t ttlMs, uint32_t nowMs);

private:
    char id_[33]{};
    uint32_t generation_ = 0;
    uint32_t openedAtMs_ = 0;
    bool active_ = false;
};

} }
