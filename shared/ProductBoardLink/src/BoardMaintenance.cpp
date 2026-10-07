#include "BoardMaintenance.h"

#include <cstring>

namespace babytech { namespace boardlink {
namespace {
constexpr uint8_t kAcquire = 1, kRenew = 2, kRelease = 3, kReply = 4;
constexpr uint8_t kInactive = 0, kActive = 1, kUnsafe = 2, kBusy = 3;
bool asciiId(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}
size_t deviceLength(const char* text) {
    if (!text || !asciiId(text[0])) return 0;
    for (size_t i = 1; i <= 64; ++i) {
        if (!text[i]) return i;
        if (!asciiId(text[i]) && text[i] != '_' && text[i] != '-') return 0;
    }
    return 0;
}
bool hex(const char* text, size_t length) {
    if (!text) return false;
    bool nonzero = false;
    for (size_t i = 0; i < length; ++i) {
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
        nonzero |= text[i] != '0';
    }
    return nonzero && !text[length];
}
bool frameValid(const v4::Frame& frame) {
    return frame.kind == v4::Kind::MigrationMaintenance && v4::validFrame(frame) &&
           !frame.offset && frame.total == frame.length;
}
}

bool BoardMaintenance::begin(v4::Role role, const char* physicalId, uint64_t boot,
                             const v4::Pairing* pairing) {
    if (initialized_ || !boot || !hex(physicalId, 12) ||
        (role != v4::Role::Brain && role != v4::Role::Motion)) return false;
    if (pairing && (!v4::validPairing(*pairing) || pairing->role != role ||
                   std::strcmp(pairing->localPhysicalId, physicalId))) return false;
    role_ = role;
    boot_ = boot;
    std::memcpy(physical_, physicalId, sizeof(physical_));
    paired_ = pairing != nullptr;
    if (pairing) pairing_ = *pairing;
    initialized_ = true;
    return true;
}

bool BoardMaintenance::setTarget(BoardMaintenanceTarget* target) {
    if (!initialized_ || role_ != v4::Role::Motion || held_ || pendingOutput_) return false;
    target_ = target;
    return true;
}

bool BoardMaintenance::send(uint8_t operation, uint32_t nowMs) {
    if (!nextId_) return false;
    const auto length = deviceLength(device_);
    output_ = v4::Frame{};
    output_.kind = v4::Kind::MigrationMaintenance;
    output_.senderBoot = boot_;
    output_.receiverBoot = peerBoot_;
    output_.messageId = requestId_ = nextId_++;
    output_.payload[0] = operation;
    output_.payload[1] = uint8_t(length);
    std::memcpy(output_.payload + 2, device_, length);
    std::memcpy(output_.payload + 2 + length, physical_, 12);
    std::memcpy(output_.payload + 14 + length, nonce_, 32);
    output_.length = output_.total = uint16_t(46 + length);
    pendingOutput_ = awaiting_ = true;
    requestedAt_ = nowMs;
    return true;
}

bool BoardMaintenance::request(const char* device, const DiscoveryResult& peer,
                               const char* nonce, uint32_t nowMs) {
    if (!initialized_ || role_ != v4::Role::Brain || awaiting_ || pendingOutput_ ||
        state_ == BoardMaintenanceState::Active || state_ == BoardMaintenanceState::Releasing ||
        !nextId_ || !deviceLength(device) || !hex(nonce, 32) ||
        peer.state != DiscoveryState::Found || !peer.peerBoot || peer.peerBoot == boot_ ||
        !hex(peer.physicalId, 12) || !std::strcmp(peer.physicalId, physical_)) return false;
    if (peer.pairingState != DiscoveryPairState::Missing && peer.pairingState != DiscoveryPairState::Ready)
        return false;
    if (peer.pairingState == DiscoveryPairState::Ready &&
        (!v4::validPairing(peer.pairing) || peer.pairing.role != v4::Role::Motion ||
         std::strcmp(peer.pairing.deviceId, device) || std::strcmp(peer.pairing.peerPhysicalId, physical_) ||
         std::strcmp(peer.pairing.localPhysicalId, peer.physicalId))) return false;
    if (paired_ && (std::strcmp(pairing_.deviceId, device) ||
                    std::strcmp(pairing_.peerPhysicalId, peer.physicalId) ||
                    peer.pairingState != DiscoveryPairState::Ready ||
                    std::strcmp(pairing_.epoch, peer.pairing.epoch))) return false;
    std::memset(device_, 0, sizeof(device_));
    std::memcpy(device_, device, deviceLength(device));
    std::memcpy(nonce_, nonce, sizeof(nonce_));
    peerBoot_ = peer.peerBoot;
    state_ = BoardMaintenanceState::Pending;
    return send(kAcquire, nowMs);
}

bool BoardMaintenance::release(uint32_t nowMs) {
    if (!initialized_ || role_ != v4::Role::Brain || !nextId_ ||
        (state_ != BoardMaintenanceState::Pending && state_ != BoardMaintenanceState::Active)) return false;
    // Releasing can replace a queued acquisition/renewal, but cannot alter bytes
    // already copied to TX. Its newer ID supersedes those replies.
    state_ = BoardMaintenanceState::Releasing;
    return send(kRelease, nowMs);
}

bool BoardMaintenance::owns(const char* device, const char* nonce, uint64_t requesterBoot) const {
    return active() && !expired_ && device && nonce && requesterBoot == peerBoot_ &&
           !std::strcmp(device, device_) && !std::strcmp(nonce, nonce_);
}

void BoardMaintenance::reply(const v4::Frame& request, uint8_t result, const char* nonce) {
    output_ = v4::Frame{};
    output_.kind = v4::Kind::MigrationMaintenance;
    output_.senderBoot = boot_;
    output_.receiverBoot = request.senderBoot;
    output_.messageId = request.messageId;
    output_.payload[0] = kReply;
    output_.payload[1] = result;
    std::memcpy(output_.payload + 2, nonce, 32);
    output_.length = output_.total = 34;
    pendingOutput_ = true;
}

bool BoardMaintenance::owns(const char* device, const char* nonce, uint64_t requesterBoot,
                            const char* requesterPhysicalId) const {
    return requesterPhysicalId && !std::strcmp(requesterPhysicalId, requester_) &&
           owns(device, nonce, requesterBoot);
}

void BoardMaintenance::receive(const v4::Frame& frame, uint32_t nowMs) {
    if (!initialized_ || !frameValid(frame) || frame.receiverBoot != boot_ || frame.senderBoot == boot_) return;
    poll(nowMs);
    if (role_ == v4::Role::Brain) {
        if (!awaiting_ || frame.senderBoot != peerBoot_ || frame.messageId != requestId_ ||
            frame.length != 34 || frame.payload[0] != kReply || frame.payload[1] > kBusy ||
            std::memcmp(frame.payload + 2, nonce_, 32)) return;
        awaiting_ = pendingOutput_ = false;
        if (frame.payload[1] == kActive && state_ != BoardMaintenanceState::Releasing) {
            state_ = BoardMaintenanceState::Active;
            renewedAt_ = nowMs;
            // The request began before Motion granted the lease. Using ACK
            // arrival would incorrectly give transport delay a new lifetime.
            confirmedAt_ = requestedAt_;
        } else if (frame.payload[1] == kInactive) state_ = BoardMaintenanceState::Released;
        else if (frame.payload[1] == kUnsafe) state_ = BoardMaintenanceState::Unsafe;
        else if (frame.payload[1] == kBusy) state_ = BoardMaintenanceState::Busy;
        else state_ = BoardMaintenanceState::Unavailable;
        return;
    }
    if (pendingOutput_ || frame.length < 47 || frame.payload[0] < kAcquire || frame.payload[0] > kRelease) return;
    const size_t length = frame.payload[1];
    if (!length || length > 64 || frame.length != 46 + length ||
        std::memchr(frame.payload + 2, 0, length)) return;
    char device[65]{}, physical[13]{}, nonce[33]{};
    std::memcpy(device, frame.payload + 2, length);
    std::memcpy(physical, frame.payload + 2 + length, 12);
    std::memcpy(nonce, frame.payload + 14 + length, 32);
    if (deviceLength(device) != length || !hex(physical, 12) || !hex(nonce, 32) ||
        !std::strcmp(physical, physical_)) return;
    if (paired_ && (std::strcmp(pairing_.deviceId, device) ||
                    std::strcmp(pairing_.peerPhysicalId, physical))) return;
    const bool sameOwner = frame.senderBoot == peerBoot_ &&
        !std::strcmp(requester_, physical) && !std::strcmp(device_, device);
    const bool sameSession = sameOwner && !std::strcmp(nonce_, nonce);
    if ((sameOwner && frame.messageId <= lastPeerId_) ||
        (held_ && !sameSession)) {
        reply(frame, kBusy, nonce); return;
    }
    uint8_t result = kInactive;
    const auto operation = frame.payload[0];
    if (held_ && expired_ && operation != kRelease) {
        reply(frame, kUnsafe, nonce);
        return;
    }
    if (!held_ && operation != kAcquire) {
        // END may overtake an acquisition that has not arrived. Remember its
        // fence, but never let foreign idle traffic replace an existing owner.
        if (operation == kRelease && (!peerBoot_ || sameOwner)) {
            peerBoot_ = frame.senderBoot;
            lastPeerId_ = frame.messageId;
            std::memcpy(device_, device, sizeof(device_));
            std::memcpy(requester_, physical, sizeof(requester_));
            std::memcpy(nonce_, nonce, sizeof(nonce_));
        } else if (sameSession) lastPeerId_ = frame.messageId;
        reply(frame, kInactive, nonce);
        return;
    }
    if (operation == kAcquire) {
        if (held_) result = kActive;
        else if (sameSession) { reply(frame, kBusy, nonce); return; }
        else if (!target_ || !target_->safeToAcquire()) { reply(frame, kUnsafe, nonce); return; }
        else { held_ = true; expired_ = false; result = kActive; }
    } else if (operation == kRenew) {
        if (held_ && sameOwner && !std::strcmp(nonce_, nonce)) result = kActive;
    } else if (held_) {
        if (!target_ || !target_->safeToRelease()) result = kUnsafe;
        else held_ = false;
    }
    if (!sameOwner) lastPeerId_ = 0;
    peerBoot_ = frame.senderBoot;
    lastPeerId_ = frame.messageId;
    std::memcpy(device_, device, sizeof(device_));
    std::memcpy(requester_, physical, sizeof(requester_));
    std::memcpy(nonce_, nonce, sizeof(nonce_));
    if (result == kActive) renewedAt_ = nowMs;
    reply(frame, result, nonce);
}

void BoardMaintenance::poll(uint32_t nowMs) {
    if (!initialized_) return;
    if (role_ == v4::Role::Motion) {
        if (held_ && uint32_t(nowMs - renewedAt_) >= kLeaseMs) {
            expired_ = true;
            if (target_ && target_->safeToRelease()) held_ = false;
        }
        return;
    }
    if (awaiting_ && uint32_t(nowMs - requestedAt_) >= kResponseMs) {
        awaiting_ = pendingOutput_ = false;
        state_ = BoardMaintenanceState::TimedOut;
    }
    if (state_ == BoardMaintenanceState::Active &&
        uint32_t(nowMs - confirmedAt_) >= kLeaseMs) {
        awaiting_ = pendingOutput_ = false;
        state_ = BoardMaintenanceState::TimedOut;
        return;
    }
    if (state_ == BoardMaintenanceState::Active && !awaiting_ &&
        uint32_t(nowMs - renewedAt_) >= kRenewMs && !send(kRenew, nowMs))
        state_ = BoardMaintenanceState::Unavailable;
}

} }
