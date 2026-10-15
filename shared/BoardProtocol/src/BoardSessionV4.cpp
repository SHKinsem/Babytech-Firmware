#include "BoardSessionV4.h"

#include <cstring>

namespace babytech { namespace v4 {
namespace {
bool alphanumeric(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9');
}

bool validDeviceId(const char (&value)[65]) {
    if (!alphanumeric(value[0])) return false;
    for (size_t i = 1; i < sizeof(value); ++i) {
        if (!value[i]) return true;
        if (!alphanumeric(value[i]) && value[i] != '_' && value[i] != '-') return false;
    }
    return false;
}

template <size_t N> bool nonzeroHex(const char (&value)[N]) {
    if (value[N - 1]) return false;
    bool nonzero = false;
    for (size_t i = 0; i < N - 1; ++i) {
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f'))) return false;
        nonzero |= value[i] != '0';
    }
    return nonzero;
}

bool validRole(Role role) { return role == Role::Brain || role == Role::Motion; }
}  // namespace

bool validPairing(const Pairing& pairing) {
    return validRole(pairing.role) && validDeviceId(pairing.deviceId) &&
           nonzeroHex(pairing.epoch) && nonzeroHex(pairing.localPhysicalId) &&
           nonzeroHex(pairing.peerPhysicalId) &&
           std::strcmp(pairing.localPhysicalId, pairing.peerPhysicalId) != 0;
}

bool validHello(const Hello& hello) {
    return hello.protocol == 4 && validRole(hello.role) &&
           (hello.capabilities & kReadOnlyCapabilities) == kReadOnlyCapabilities &&
           validDeviceId(hello.deviceId) && nonzeroHex(hello.epoch) &&
           nonzeroHex(hello.physicalId);
}

bool Session::begin(const Pairing& pairing, uint64_t localBoot) {
    *this = Session{};
    if (!localBoot || !validPairing(pairing)) return false;
    pairing_ = pairing;
    localBoot_ = localBoot;
    paired_ = true;
    return true;
}

Hello Session::localHello() const {
    Hello hello;
    if (paired_) {
        hello.role = pairing_.role;
        std::memcpy(hello.deviceId, pairing_.deviceId, sizeof(hello.deviceId));
        std::memcpy(hello.epoch, pairing_.epoch, sizeof(hello.epoch));
        std::memcpy(hello.physicalId, pairing_.localPhysicalId, sizeof(hello.physicalId));
    }
    return hello;
}

bool Session::expectHelloAck(uint32_t messageId, uint32_t nowMs) {
    if (!paired_ || !messageId || messageId <= probeId_ || awaitingHelloAck(nowMs)) return false;
    probeId_ = messageId;
    probeAt_ = nowMs;
    probePending_ = true;
    return true;
}

bool Session::awaitingHelloAck(uint32_t nowMs) const {
    return probePending_ && uint32_t(nowMs - probeAt_) < kLinkTimeoutMs;
}

HelloResult Session::hello(const Message& envelope, const Hello& peer, uint32_t nowMs) {
    if (!paired_ || !envelope.senderBoot || !envelope.messageId || !envelope.length ||
        envelope.length > kMaxMessage ||
        (envelope.kind != Kind::Hello && envelope.kind != Kind::HelloAck) ||
        (envelope.receiverBoot != localBoot_ &&
         !(envelope.kind == Kind::Hello && !envelope.receiverBoot))) return HelloResult::Invalid;
    if (peer.protocol != 4 || (peer.capabilities & kReadOnlyCapabilities) != kReadOnlyCapabilities)
        return HelloResult::ProtocolMismatch;
    if (!validHello(peer)) return HelloResult::Invalid;
    if (peer.role == pairing_.role || std::strcmp(peer.deviceId, pairing_.deviceId) ||
        std::strcmp(peer.epoch, pairing_.epoch) ||
        std::strcmp(peer.physicalId, pairing_.peerPhysicalId)) return HelloResult::IdentityMismatch;

    // An unsolicited HELLO is only a request to answer, never proof of a live peer.
    if (envelope.kind == Kind::Hello)
        return peer.replyTo == 0 ? HelloResult::Accepted : HelloResult::Invalid;
    if (!awaitingHelloAck(nowMs) || peer.replyTo != probeId_) return HelloResult::Invalid;
    probePending_ = false;

    poll(nowMs);
    if (peerBoot_ != envelope.senderBoot) {
        if (peerBoot_) failure_ = LinkFailure::PeerRestarted;
        invalidate();
        heartbeatId_ = statusId_ = 0;
        peerBoot_ = envelope.senderBoot;
    }
    // The fresh reply is a sequence barrier, including messages never received
    // before a disconnection. Transport IDs are monotonic within a peer boot.
    if (heartbeatId_ < envelope.messageId) heartbeatId_ = envelope.messageId;
    if (statusId_ < envelope.messageId) statusId_ = envelope.messageId;
    // Repeated HELLO does not extend the heartbeat deadline or erase status IDs.
    if (!handshaken_) {
        handshakeAt_ = nowMs;
        handshaken_ = true;
    }
    return HelloResult::Accepted;
}

bool Session::matches(uint64_t senderBoot, uint64_t receiverBoot) const {
    return paired_ && handshaken_ && senderBoot == peerBoot_ && receiverBoot == localBoot_;
}

bool Session::heartbeat(const Frame& frame, uint32_t nowMs) {
    poll(nowMs);
    if (!validFrame(frame) || frame.kind != Kind::Heartbeat ||
        !matches(frame.senderBoot, frame.receiverBoot) || frame.messageId <= heartbeatId_)
        return false;
    heartbeatId_ = frame.messageId;
    lastHeartbeatAt_ = nowMs;
    heartbeatSeen_ = true;
    return true;
}

bool Session::status(const Message& message, uint32_t nowMs) {
    poll(nowMs);
    if (!connected(nowMs) || message.kind != Kind::Status ||
        !matches(message.senderBoot, message.receiverBoot) ||
        !message.length || message.length > kMaxMessage || message.messageId <= statusId_)
        return false;
    statusId_ = message.messageId;
    lastStatusAt_ = nowMs;
    statusSeen_ = true;
    return true;
}

void Session::invalidate() {
    handshaken_ = heartbeatSeen_ = statusSeen_ = false;
}

void Session::poll(uint32_t nowMs) {
    if (probePending_ && !awaitingHelloAck(nowMs)) probePending_ = false;
    if (handshaken_ && uint32_t(nowMs - (heartbeatSeen_ ? lastHeartbeatAt_ : handshakeAt_)) >=
                           kLinkTimeoutMs) {
        invalidate();
        if (failure_ == LinkFailure::None) failure_ = LinkFailure::HeartbeatExpired;
    }
}

bool Session::connected(uint32_t nowMs) const {
    return paired_ && handshaken_ && heartbeatSeen_ &&
           uint32_t(nowMs - lastHeartbeatAt_) < kLinkTimeoutMs;
}

bool Session::freshStatus(uint32_t nowMs) const {
    return connected(nowMs) && statusSeen_ && uint32_t(nowMs - lastStatusAt_) < kLinkTimeoutMs;
}

LinkFailure Session::takeFailure() {
    const LinkFailure failure = failure_;
    failure_ = LinkFailure::None;
    return failure;
}

} }  // namespace babytech::v4
