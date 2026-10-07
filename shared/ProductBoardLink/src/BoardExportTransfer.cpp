#include "BoardExportTransfer.h"

#include <cstring>
#include <new>

namespace babytech { namespace boardlink {
namespace {
constexpr uint8_t kRead = 1, kData = 2, kError = 3;
constexpr size_t kChunkSize = v4::kMaxFragment - 5;

bool alphanumeric(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}
size_t deviceLength(const char* text) {
    if (!text || !alphanumeric(text[0])) return 0;
    for (size_t i = 1; i <= 64; ++i) {
        if (!text[i]) return i;
        if (!alphanumeric(text[i]) && text[i] != '_' && text[i] != '-') return 0;
    }
    return 0;
}
bool hexIdentity(const char* text, size_t length) {
    if (!text) return false;
    bool nonzero = false;
    for (size_t i = 0; i < length; ++i) {
        const char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        nonzero |= c != '0';
    }
    return nonzero && !text[length];
}
bool singleFrame(const v4::Frame& frame) {
    return frame.kind == v4::Kind::MigrationRead && v4::validFrame(frame) &&
           !frame.offset && frame.length == frame.total;
}
bool samePair(const v4::Pairing& a, const v4::Pairing& b) {
    return a.role == b.role && !std::strcmp(a.deviceId, b.deviceId) &&
           !std::strcmp(a.epoch, b.epoch) && !std::strcmp(a.localPhysicalId, b.localPhysicalId) &&
           !std::strcmp(a.peerPhysicalId, b.peerPhysicalId);
}
size_t offset(const uint8_t* bytes) { return size_t(bytes[0]) | (size_t(bytes[1]) << 8); }
void putOffset(uint8_t* bytes, size_t value) {
    bytes[0] = uint8_t(value); bytes[1] = uint8_t(value >> 8);
}
}

void BoardExportTransfer::clearInput() {
    if (input_) std::memset(input_.get(), 0, kMotionExportWireMaxSize + 1);
    input_.reset();
}

void BoardExportTransfer::reset() {
    if (captureActive_ && source_) source_->cancel();
    clearInput();
    snapshot_.reset();
    source_ = nullptr;
    initialized_ = outgoingPending_ = captureActive_ = false;
    state_ = ExportTransferState::Idle;
    position_ = outgoingCount_ = 0;
    nextId_ = 1;
    lastReadId_ = 0;
    requesterBoot_ = 0;
    requesterPhysical_[0] = device_[0] = challenge_[0] = 0;
}

bool BoardExportTransfer::begin(v4::Role role, const char* physicalId, uint64_t boot) {
    if (initialized_ || (role != v4::Role::Brain && role != v4::Role::Motion) ||
        !boot || !hexIdentity(physicalId, 12)) return false;
    role_ = role;
    std::memcpy(physicalId_, physicalId, sizeof(physicalId_));
    boot_ = boot;
    initialized_ = true;
    return true;
}

bool BoardExportTransfer::setSource(BoardExportSource* source) {
    if (!initialized_ || role_ != v4::Role::Motion || captureActive_ || outgoingPending_) return false;
    source_ = source;
    return true;
}

bool BoardExportTransfer::query(uint32_t nowMs) {
    if (!nextId_) return false;
    const size_t length = deviceLength(device_);
    outgoing_ = v4::Frame{};
    outgoing_.kind = v4::Kind::MigrationRead;
    outgoing_.senderBoot = boot_;
    outgoing_.receiverBoot = peer_.peerBoot;
    outgoing_.messageId = requestId_ = nextId_++;
    outgoing_.payload[0] = kRead;
    outgoing_.payload[1] = uint8_t(length);
    std::memcpy(outgoing_.payload + 2, device_, length);
    std::memcpy(outgoing_.payload + 2 + length, physicalId_, 12);
    std::memcpy(outgoing_.payload + 14 + length, challenge_, 32);
    putOffset(outgoing_.payload + 46 + length, position_);
    outgoing_.length = outgoing_.total = uint16_t(48 + length);
    outgoingPending_ = true;
    chunkAt_ = nowMs;
    return true;
}

bool BoardExportTransfer::request(const char* device, const DiscoveryResult& peer,
                                  const char* challenge, uint32_t nowMs) {
    if (!initialized_ || role_ != v4::Role::Brain || state_ == ExportTransferState::Pending ||
        outgoingPending_ || !nextId_ || !deviceLength(device) || !hexIdentity(challenge, 32) ||
        peer.state != DiscoveryState::Found || !peer.peerBoot || peer.peerBoot == boot_ ||
        !hexIdentity(peer.physicalId, 12) || !std::strcmp(peer.physicalId, physicalId_)) return false;
    if (peer.pairingState != DiscoveryPairState::Missing && peer.pairingState != DiscoveryPairState::Ready)
        return false;
    if (peer.pairingState == DiscoveryPairState::Ready &&
        (!v4::validPairing(peer.pairing) || peer.pairing.role != v4::Role::Motion ||
         std::strcmp(peer.pairing.deviceId, device) ||
         std::strcmp(peer.pairing.localPhysicalId, peer.physicalId) ||
         std::strcmp(peer.pairing.peerPhysicalId, physicalId_))) return false;
    std::unique_ptr<char[]> buffer(new (std::nothrow) char[kMotionExportWireMaxSize + 1]{});
    if (!buffer) return false;
    clearInput();
    input_ = std::move(buffer);
    snapshot_.reset();
    std::memset(device_, 0, sizeof(device_));
    std::memcpy(device_, device, deviceLength(device));
    std::memcpy(challenge_, challenge, sizeof(challenge_));
    peer_ = peer;
    position_ = 0;
    beganAt_ = nowMs;
    state_ = ExportTransferState::Pending;
    return query(nowMs);
}

void BoardExportTransfer::fail(ExportTransferState state) {
    state_ = state;
    outgoingPending_ = false;
    clearInput();
    snapshot_.reset();
}

void BoardExportTransfer::replyError(const v4::Frame& request) {
    outgoing_ = v4::Frame{};
    outgoing_.kind = v4::Kind::MigrationRead;
    outgoing_.senderBoot = boot_;
    outgoing_.receiverBoot = request.senderBoot;
    outgoing_.messageId = request.messageId;
    outgoing_.total = outgoing_.length = 1;
    outgoing_.payload[0] = kError;
    outgoingCount_ = 0;
    outgoingPending_ = true;
}

void BoardExportTransfer::receive(const v4::Frame& frame, uint32_t nowMs) {
    if (!initialized_ || !singleFrame(frame) || frame.receiverBoot != boot_ || frame.senderBoot == boot_) return;
    poll(nowMs);
    if (role_ == v4::Role::Motion) {
        if (frame.length < 49 || frame.payload[0] != kRead) return;
        const size_t length = frame.payload[1];
        if (!length || length > 64 || frame.length != 48 + length ||
            std::memchr(frame.payload + 2, 0, length)) return;
        char device[65]{}, physical[13]{}, challenge[33]{};
        std::memcpy(device, frame.payload + 2, length);
        std::memcpy(physical, frame.payload + 2 + length, 12);
        std::memcpy(challenge, frame.payload + 14 + length, 32);
        if (deviceLength(device) != length || !hexIdentity(physical, 12) ||
            !hexIdentity(challenge, 32) || !std::strcmp(physical, physicalId_)) return;
        const size_t wanted = offset(frame.payload + 46 + length);
        const bool sameOwner = frame.senderBoot == requesterBoot_ &&
            !std::strcmp(device, device_) && !std::strcmp(physical, requesterPhysical_);
        // A fresh nonce restarts a read; it is not a chunk retransmission.
        const bool restart = captureActive_ && sameOwner && !wanted &&
            std::strcmp(challenge, challenge_) && frame.messageId > lastReadId_;
        if (outgoingPending_ && !restart) return;
        if (sameOwner && frame.messageId <= lastReadId_) { replyError(frame); return; }
        if (restart) {
            source_->cancel();
            captureActive_ = outgoingPending_ = false;
            outgoingCount_ = 0;
        }
        if (!captureActive_) {
            if (wanted || !source_ || !source_->begin(role_, device, challenge, boot_, nowMs)) {
                replyError(frame);
                return;
            }
            if (!source_->remaining() || source_->remaining() > kMotionExportWireMaxSize) {
                source_->cancel(); replyError(frame); return;
            }
            std::memcpy(device_, device, sizeof(device_));
            std::memcpy(challenge_, challenge, sizeof(challenge_));
            std::memcpy(requesterPhysical_, physical, sizeof(requesterPhysical_));
            if (!sameOwner) lastReadId_ = 0;
            requesterBoot_ = frame.senderBoot;
            beganAt_ = nowMs;
            position_ = 0;
            captureActive_ = true;
        }
        if (frame.senderBoot != requesterBoot_ || std::strcmp(device, device_) ||
            std::strcmp(physical, requesterPhysical_) || std::strcmp(challenge, challenge_) || wanted != position_) {
            replyError(frame);
            return;
        }
        const size_t remaining = source_->remaining();
        if (!remaining || remaining > kMotionExportWireMaxSize - position_) {
            source_->cancel(); captureActive_ = false; replyError(frame); return;
        }
        outgoing_ = v4::Frame{};
        outgoing_.kind = v4::Kind::MigrationRead;
        outgoing_.senderBoot = boot_;
        outgoing_.receiverBoot = frame.senderBoot;
        outgoing_.messageId = frame.messageId;
        outgoingCount_ = source_->peek(outgoing_.payload + 5, kChunkSize);
        const size_t expected = remaining < kChunkSize ? remaining : kChunkSize;
        if (outgoingCount_ != expected) {
            source_->cancel(); captureActive_ = false; replyError(frame); return;
        }
        outgoing_.payload[0] = kData;
        putOffset(outgoing_.payload + 1, position_);
        outgoing_.payload[3] = uint8_t(outgoingCount_);
        outgoing_.payload[4] = uint8_t(outgoingCount_ == remaining);
        outgoing_.total = outgoing_.length = uint16_t(5 + outgoingCount_);
        lastReadId_ = frame.messageId;
        outgoingPending_ = true;
        return;
    }

    if (state_ != ExportTransferState::Pending || frame.senderBoot != peer_.peerBoot ||
        frame.messageId != requestId_) return;
    if (frame.length == 1 && frame.payload[0] == kError) { fail(ExportTransferState::Unavailable); return; }
    if (frame.length < 6 || frame.payload[0] != kData || frame.payload[4] > 1) return;
    const size_t count = frame.payload[3];
    if (!count || count > kChunkSize || frame.length != 5 + count ||
        offset(frame.payload + 1) != position_) return;
    if (count > kMotionExportWireMaxSize - position_) { fail(ExportTransferState::Invalid); return; }
    std::memcpy(input_.get() + position_, frame.payload + 5, count);
    position_ += count;
    outgoingPending_ = false;
    if (!frame.payload[4]) {
        if (position_ == kMotionExportWireMaxSize || !query(nowMs)) fail(ExportTransferState::Invalid);
        return;
    }
    std::unique_ptr<MotionExportSnapshot> parsed(new (std::nothrow) MotionExportSnapshot);
    if (!parsed) { fail(ExportTransferState::Unavailable); return; }
    input_[position_] = 0;
    if (!decodeMotionExport(input_.get(), position_, device_, peer_.physicalId, peer_.peerBoot, challenge_, *parsed) ||
        (parsed->pairStatus == ExportRead::Ready &&
         (peer_.pairingState != DiscoveryPairState::Ready || !samePair(parsed->pairing, peer_.pairing))) ||
        (parsed->pairStatus == ExportRead::Missing && peer_.pairingState != DiscoveryPairState::Missing)) {
        fail(ExportTransferState::Invalid);
        return;
    }
    snapshot_ = std::move(parsed);
    clearInput();
    state_ = ExportTransferState::Complete;
}

void BoardExportTransfer::queued() {
    if (!outgoingPending_) return;
    outgoingPending_ = false;
    if (role_ == v4::Role::Motion && captureActive_ && outgoingCount_) {
        const bool done = outgoing_.payload[4] != 0;
        source_->consume(outgoingCount_);
        position_ += outgoingCount_;
        outgoingCount_ = 0;
        if (done) { source_->cancel(); captureActive_ = false; }
    }
}

void BoardExportTransfer::poll(uint32_t nowMs) {
    if (role_ == v4::Role::Brain && state_ == ExportTransferState::Pending &&
        (uint32_t(nowMs - beganAt_) >= kLifetimeMs || uint32_t(nowMs - chunkAt_) >= kChunkTimeoutMs))
        fail(ExportTransferState::TimedOut);
    if (role_ == v4::Role::Motion && captureActive_ && uint32_t(nowMs - beganAt_) >= kLifetimeMs) {
        if (source_) source_->cancel();
        captureActive_ = outgoingPending_ = false;
        outgoingCount_ = 0;
    }
}

} }
