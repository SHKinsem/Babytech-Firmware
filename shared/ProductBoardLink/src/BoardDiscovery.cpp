#include "BoardDiscovery.h"

#include <cstring>

namespace babytech { namespace boardlink {
namespace {
constexpr uint8_t kQuery = 1, kReply = 2;
constexpr size_t kPhysicalLength = 12, kReplyHeader = 15, kMaxPairLength = 134;
static_assert(2 + 64 + kPhysicalLength <= v4::kMaxFragment, "Discovery query budget");
static_assert(kReplyHeader + kMaxPairLength <= v4::kMaxFragment, "Discovery reply budget");

bool alphanumeric(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9');
}

size_t deviceLength(const char* value) {
    if (!value || !alphanumeric(value[0])) return 0;
    for (size_t i = 1; i <= 64; ++i) {
        if (!value[i]) return i;
        if (!alphanumeric(value[i]) && value[i] != '_' && value[i] != '-') return 0;
    }
    return 0;
}

bool validPhysical(const char* value) {
    if (!value) return false;
    bool nonzero = false;
    for (size_t i = 0; i < kPhysicalLength; ++i) {
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f'))) return false;
        nonzero |= value[i] != '0';
    }
    return !value[kPhysicalLength] && nonzero;
}

bool validState(DiscoveryPairState state) {
    return uint8_t(state) <= uint8_t(DiscoveryPairState::IdentityMismatch);
}

bool completeFrame(const v4::Frame& frame) {
    return frame.kind == v4::Kind::Discovery && v4::validFrame(frame) &&
           frame.offset == 0 && frame.total == frame.length;
}

bool decodeQuery(const v4::Frame& frame, char (&device)[65], char (&physical)[13]) {
    if (frame.receiverBoot || frame.length < 15 || frame.payload[0] != kQuery) return false;
    const size_t length = frame.payload[1];
    if (!length || length > 64 || frame.length != 2 + length + kPhysicalLength ||
        std::memchr(frame.payload + 2, 0, length)) return false;
    std::memcpy(device, frame.payload + 2, length);
    std::memcpy(physical, frame.payload + 2 + length, kPhysicalLength);
    return deviceLength(device) == length && validPhysical(physical);
}

bool decodeReply(const v4::Frame& frame, DiscoveryResult& result) {
    if (frame.length < kReplyHeader || frame.payload[0] != kReply) return false;
    const auto state = static_cast<DiscoveryPairState>(frame.payload[13]);
    const size_t length = frame.payload[14];
    if (!validState(state) || frame.length != kReplyHeader + length ||
        length > kMaxPairLength) return false;
    std::memcpy(result.physicalId, frame.payload + 1, kPhysicalLength);
    if (!validPhysical(result.physicalId)) return false;
    if (state == DiscoveryPairState::Ready) {
        if (!decodePairingRecord(frame.payload + kReplyHeader, length, result.pairing) ||
            result.pairing.role != v4::Role::Motion ||
            std::strcmp(result.pairing.localPhysicalId, result.physicalId)) return false;
    } else if (length) return false;
    result.peerBoot = frame.senderBoot;
    result.pairingState = state;
    return true;
}
}  // namespace

bool BoardDiscovery::begin(v4::Role role, const char* physicalId, uint64_t boot,
                           DiscoveryPairState state, const v4::Pairing* pairing) {
    if (initialized_ || (role != v4::Role::Brain && role != v4::Role::Motion) ||
        !boot || !validPhysical(physicalId) || !validState(state)) return false;
    if (state == DiscoveryPairState::Ready) {
        if (!pairing || !v4::validPairing(*pairing) || pairing->role != role ||
            std::strcmp(pairing->localPhysicalId, physicalId)) return false;
    } else if (pairing) return false;

    role_ = role;
    pairingState_ = state;
    if (pairing) pairing_ = *pairing;
    std::memcpy(physicalId_, physicalId, sizeof(physicalId_));
    boot_ = boot;
    initialized_ = true;
    return true;
}

bool BoardDiscovery::request(const char* deviceId, uint32_t nowMs) {
    if (!initialized_ || role_ != v4::Role::Brain || !nextId_ || outgoingPending_ ||
        result_.state == DiscoveryState::Pending) return false;
    const size_t length = deviceLength(deviceId);
    if (!length || (pairingState_ == DiscoveryPairState::Ready &&
                    std::strcmp(pairing_.deviceId, deviceId))) return false;

    v4::Frame candidate{};
    candidate.kind = v4::Kind::Discovery;
    candidate.senderBoot = boot_;
    candidate.receiverBoot = 0;
    candidate.messageId = nextId_;
    candidate.total = candidate.length = uint16_t(2 + length + kPhysicalLength);
    candidate.payload[0] = kQuery;
    candidate.payload[1] = uint8_t(length);
    std::memcpy(candidate.payload + 2, deviceId, length);
    std::memcpy(candidate.payload + 2 + length, physicalId_, kPhysicalLength);
    if (!completeFrame(candidate)) return false;

    std::memset(requestedDeviceId_, 0, sizeof(requestedDeviceId_));
    std::memcpy(requestedDeviceId_, deviceId, length);
    requestId_ = nextId_++;
    requestedAt_ = outgoingAt_ = nowMs;
    outgoing_ = candidate;
    outgoingPending_ = true;
    result_ = DiscoveryResult{};
    result_.state = DiscoveryState::Pending;
    return true;
}

void BoardDiscovery::receive(const v4::Frame& frame, uint32_t nowMs) {
    if (!initialized_ || !completeFrame(frame) || frame.senderBoot == boot_) return;
    if (role_ == v4::Role::Motion) {
        char device[65]{}, physical[13]{};
        if (!decodeQuery(frame, device, physical) || !std::strcmp(physical, physicalId_)) return;
        v4::Frame candidate{};
        candidate.kind = v4::Kind::Discovery;
        candidate.senderBoot = boot_;
        candidate.receiverBoot = frame.senderBoot;
        candidate.messageId = frame.messageId;
        candidate.payload[0] = kReply;
        std::memcpy(candidate.payload + 1, physicalId_, kPhysicalLength);
        candidate.payload[13] = uint8_t(pairingState_);
        size_t length = 0;
        if (pairingState_ == DiscoveryPairState::Ready) {
            length = encodePairingRecord(pairing_, candidate.payload + kReplyHeader,
                                        kMaxPairLength);
            if (!length) return;
        }
        candidate.payload[14] = uint8_t(length);
        candidate.total = candidate.length = uint16_t(kReplyHeader + length);
        if (!completeFrame(candidate)) return;
        poll(nowMs);
        if (outgoingPending_) return;
        outgoing_ = candidate;
        outgoingAt_ = nowMs;
        outgoingPending_ = true;
        return;
    }

    if (result_.state != DiscoveryState::Pending || frame.receiverBoot != boot_ ||
        frame.messageId != requestId_) return;
    DiscoveryResult candidate{};
    if (!decodeReply(frame, candidate) || !std::strcmp(candidate.physicalId, physicalId_)) return;
    poll(nowMs);
    if (result_.state != DiscoveryState::Pending) return;

    if (pairingState_ != DiscoveryPairState::Missing && pairingState_ != DiscoveryPairState::Ready) {
        candidate.state = DiscoveryState::Unavailable;
    } else if (candidate.pairingState != DiscoveryPairState::Missing &&
               candidate.pairingState != DiscoveryPairState::Ready) {
        candidate.state = DiscoveryState::Unavailable;
    } else if (pairingState_ == DiscoveryPairState::Ready) {
        const v4::Pairing& peer = candidate.pairing;
        candidate.state = candidate.pairingState == DiscoveryPairState::Ready &&
            !std::strcmp(peer.deviceId, pairing_.deviceId) &&
            !std::strcmp(peer.epoch, pairing_.epoch) &&
            !std::strcmp(peer.localPhysicalId, pairing_.peerPhysicalId) &&
            !std::strcmp(peer.peerPhysicalId, physicalId_)
                ? DiscoveryState::Found : DiscoveryState::Conflict;
    } else if (candidate.pairingState == DiscoveryPairState::Ready &&
               (std::strcmp(candidate.pairing.deviceId, requestedDeviceId_) ||
                std::strcmp(candidate.pairing.peerPhysicalId, physicalId_))) {
        candidate.state = DiscoveryState::Conflict;
    } else {
        candidate.state = DiscoveryState::Found;
    }
    result_ = candidate;
    outgoingPending_ = false;
}

void BoardDiscovery::poll(uint32_t nowMs) {
    if (outgoingPending_ && uint32_t(nowMs - outgoingAt_) >= kTimeoutMs)
        outgoingPending_ = false;
    if (result_.state == DiscoveryState::Pending &&
        uint32_t(nowMs - requestedAt_) >= kTimeoutMs)
        result_.state = DiscoveryState::TimedOut;
}

} }  // namespace babytech::boardlink
