#pragma once

#include "BoardProtocolV4.h"

namespace babytech { namespace v4 {

enum class Role : uint8_t { Brain = 1, Motion = 2 };
constexpr uint32_t kReadOnlyCapabilities = 1;

struct Pairing {
    Role role = Role::Brain;
    char deviceId[65]{};
    char epoch[33]{};
    char localPhysicalId[13]{};
    char peerPhysicalId[13]{};
};

struct Hello {
    uint8_t protocol = 4;
    Role role = Role::Brain;
    uint32_t capabilities = kReadOnlyCapabilities;
    uint32_t replyTo = 0;
    char deviceId[65]{};
    char epoch[33]{};
    char physicalId[13]{};
};

bool validPairing(const Pairing& pairing);
bool validHello(const Hello& hello);

enum class HelloResult { Accepted, Invalid, ProtocolMismatch, IdentityMismatch };
enum class LinkFailure { None, PeerRestarted, HeartbeatExpired };

// Passive link state only: the owner handles failures through supervised Stop.
class Session {
public:
    bool begin(const Pairing& pairing, uint64_t localBoot);
    Hello localHello() const;
    bool expectHelloAck(uint32_t messageId, uint32_t nowMs);
    bool awaitingHelloAck(uint32_t nowMs) const;
    HelloResult hello(const Message& envelope, const Hello& peer, uint32_t nowMs);
    bool heartbeat(const Frame& frame, uint32_t nowMs);
    bool status(const Message& message, uint32_t nowMs);
    void poll(uint32_t nowMs);
    bool matches(uint64_t senderBoot, uint64_t receiverBoot) const;
    bool connected(uint32_t nowMs) const;
    bool freshStatus(uint32_t nowMs) const;
    // Local receipt time of the last accepted STATUS, not peer uptime.
    // Only meaningful with an accepted status; zero is a valid millis value.
    uint32_t lastStatusReceivedAtMs() const { return lastStatusAt_; }
    bool canExchange() const { return paired_ && handshaken_; }
    uint64_t localBoot() const { return localBoot_; }
    uint64_t peerBoot() const { return peerBoot_; }
    LinkFailure takeFailure();
private:
    void invalidate();
    Pairing pairing_{};
    uint64_t localBoot_ = 0;
    uint64_t peerBoot_ = 0;
    uint32_t lastHeartbeatAt_ = 0;
    uint32_t handshakeAt_ = 0;
    uint32_t lastStatusAt_ = 0;
    uint32_t heartbeatId_ = 0;
    uint32_t statusId_ = 0;
    uint32_t probeId_ = 0;
    uint32_t probeAt_ = 0;
    bool probePending_ = false;
    bool paired_ = false;
    bool handshaken_ = false;
    bool heartbeatSeen_ = false;
    bool statusSeen_ = false;
    LinkFailure failure_ = LinkFailure::None;
};

} }
