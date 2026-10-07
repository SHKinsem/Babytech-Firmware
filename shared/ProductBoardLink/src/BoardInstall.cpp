#include "BoardInstall.h"

#include <cstring>
#include <cmath>
#include <type_traits>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

namespace babytech { namespace boardlink {
namespace {
constexpr uint8_t kRequest = 1, kResponse = 2;
constexpr size_t kNonceSize = 32, kPairOffset = 35, kEnvelopeSize = 39;
static_assert(kEnvelopeSize + kPairingRecordMaxSize + kContextIdentityMaxSize <= v4::kMaxMessage,
              "Install message must fit ordinary UART slot");

bool hex(const char* text, size_t length) {
    if (!text) return false;
    bool nonzero = false;
    for (size_t i = 0; i < length; ++i) {
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f')))
            return false;
        nonzero |= text[i] != '0';
    }
    return nonzero && !text[length];
}

size_t readLength(const uint8_t* bytes) { return size_t(bytes[0]) | (size_t(bytes[1]) << 8); }
void putLength(uint8_t* bytes, size_t length) {
    bytes[0] = uint8_t(length);
    bytes[1] = uint8_t(length >> 8);
}

uint8_t encodeResult(CommissioningResult result) {
    switch (result) {
        case CommissioningResult::Installed: return 0;
        case CommissioningResult::AlreadyInstalled: return 1;
        case CommissioningResult::Invalid: return 2;
        case CommissioningResult::IdentityMismatch: return 3;
        case CommissioningResult::Conflict: return 4;
        case CommissioningResult::Unsafe: return 5;
        case CommissioningResult::LegacyPending: return 6;
        case CommissioningResult::StorageFault: return 7;
        case CommissioningResult::StateMissing: return 8;
    }
    return 2;
}
bool decodeResult(uint8_t byte, CommissioningResult& result) {
    switch (byte) {
        case 0: result = CommissioningResult::Installed; return true;
        case 1: result = CommissioningResult::AlreadyInstalled; return true;
        case 2: result = CommissioningResult::Invalid; return true;
        case 3: result = CommissioningResult::IdentityMismatch; return true;
        case 4: result = CommissioningResult::Conflict; return true;
        case 5: result = CommissioningResult::Unsafe; return true;
        case 6: result = CommissioningResult::LegacyPending; return true;
        case 7: result = CommissioningResult::StorageFault; return true;
        case 8: result = CommissioningResult::StateMissing; return true;
        default: return false;
    }
}
void clearMessage(v4::Message& message) {
    message.kind = v4::Kind::Heartbeat;
    message.senderBoot = message.receiverBoot = 0;
    message.messageId = 0;
    message.length = 0;
    std::memset(message.payload, 0, sizeof(message.payload));
}
void clearContext(ProductContext& context) {
    static_assert(std::is_trivially_copyable<ProductContext>::value, "Fixed context scratch");
    std::memset(static_cast<void*>(&context), 0, sizeof(context));
}
bool emptyContext(const ProductContext& context) {
    return !context.deviceId[0] && !context.profileVersion && !context.cleared &&
           !context.babyId[0] && !context.babyName[0] && !context.formulaBrand[0] &&
           !context.waterMl && !context.temperatureC && context.powderGPer100Ml == 0 &&
           !std::signbit(context.powderGPer100Ml);
}
bool fingerprint(const v4::Message& message, uint8_t (&digest)[32]) {
#if MBEDTLS_VERSION_MAJOR >= 3
    return mbedtls_sha256(message.payload, message.length, digest, 0) == 0;
#else
    return mbedtls_sha256_ret(message.payload, message.length, digest, 0) == 0;
#endif
}
}

bool BoardInstall::bindReceiveBuffers(v4::Assembler& assembler, v4::Message& scratch) {
    if (initialized_)
        return receiveAssembler_ == &assembler && receiveScratch_ == &scratch;
    receiveAssembler_ = &assembler;
    receiveScratch_ = &scratch;
    return true;
}

void BoardInstall::reset() {
    if (receiveAssembler_) receiveAssembler_->reset();
    if (receiveScratch_) clearMessage(*receiveScratch_);
    clearMessage(output_);
    ownerReplay_ = Replay{};
    rejectedReplay_ = Replay{};
    static_assert(std::is_trivially_copyable<CommissioningImport>::value, "Fixed import scratch");
    // All numeric fields are zeroable, including the IEEE-754 context ratio.
    std::memset(static_cast<void*>(&decoded_), 0, sizeof(decoded_));
    decoded_.pairing.role = v4::Role::Brain;
    target_ = nullptr;
    std::memset(physical_, 0, sizeof(physical_));
    std::memset(nonce_, 0, sizeof(nonce_));
    role_ = v4::Role::Brain;
    boot_ = peerBoot_ = 0;
    nextId_ = 1;
    requestId_ = requestedAt_ = 0;
    state_ = BoardInstallState::Idle;
    result_ = CommissioningResult::Invalid;
    initialized_ = pendingOutput_ = false;
}

bool BoardInstall::begin(v4::Role role, const char* physicalId, uint64_t boot) {
    if (initialized_ || !receiveAssembler_ || !receiveScratch_ || !boot || !hex(physicalId, 12) ||
        (role != v4::Role::Brain && role != v4::Role::Motion)) return false;
    role_ = role;
    boot_ = boot;
    std::memcpy(physical_, physicalId, sizeof(physical_));
    initialized_ = true;
    return true;
}

bool BoardInstall::setTarget(BoardInstallTarget* target) {
    if (!initialized_ || !receiveAssembler_ || role_ != v4::Role::Motion ||
        pendingOutput_ || receiveAssembler_->active()) return false;
    target_ = target;
    return true;
}

bool BoardInstall::request(const CommissioningImport& request, const char* nonce,
                           uint64_t peerBoot, uint32_t nowMs) {
    if (!initialized_ || role_ != v4::Role::Brain || pendingOutput_ ||
        state_ == BoardInstallState::Pending || !nextId_ || !peerBoot || peerBoot == boot_ ||
        !hex(nonce, kNonceSize) || !v4::validPairing(request.pairing) ||
        request.pairing.role != v4::Role::Motion ||
        std::strcmp(request.pairing.peerPhysicalId, physical_) ||
        (request.hasContext ? (!validProductContext(request.context) ||
                               std::strcmp(request.context.deviceId, request.pairing.deviceId))
                            : !emptyContext(request.context))) return false;

    const size_t pairLength = encodePairingRecord(request.pairing, output_.payload + kPairOffset,
                                                 v4::kMaxMessage - kPairOffset);
    if (!pairLength) return false;
    const size_t contextOffset = kPairOffset + pairLength + 3;
    const size_t contextLength = request.hasContext
        ? encodeContextIdentity(request.context, output_.payload + contextOffset,
                                v4::kMaxMessage - contextOffset - 1) : 0;
    if (request.hasContext && !contextLength) return false;
    output_.payload[0] = kRequest;
    std::memcpy(output_.payload + 1, nonce, kNonceSize);
    putLength(output_.payload + 33, pairLength);
    output_.payload[kPairOffset + pairLength] = uint8_t(request.hasContext);
    putLength(output_.payload + kPairOffset + pairLength + 1, contextLength);
    output_.payload[contextOffset + contextLength] = 1;
    output_.kind = v4::Kind::MigrationInstall;
    output_.senderBoot = boot_;
    output_.receiverBoot = peerBoot_ = peerBoot;
    output_.messageId = requestId_ = nextId_++;
    output_.length = uint16_t(contextOffset + contextLength + 1);
    std::memcpy(nonce_, nonce, sizeof(nonce_));
    requestedAt_ = nowMs;
    pendingOutput_ = true;
    result_ = CommissioningResult::Invalid;
    state_ = BoardInstallState::Pending;
    return true;
}

bool BoardInstall::decodeRequest() {
    if (!receiveScratch_) return false;
    const auto& input = *receiveScratch_;
    if (input.length < kEnvelopeSize || input.payload[0] != kRequest) return false;
    const size_t pairLength = readLength(input.payload + 33);
    if (pairLength > kPairingRecordMaxSize || pairLength > input.length - kEnvelopeSize) return false;
    const size_t flagOffset = kPairOffset + pairLength;
    const size_t contextLength = readLength(input.payload + flagOffset + 1);
    const uint8_t hasContext = input.payload[flagOffset];
    if (hasContext > 1 || contextLength > kContextIdentityMaxSize ||
        input.length != kEnvelopeSize + pairLength + contextLength ||
        input.payload[input.length - 1] != 1 ||
        (hasContext ? !contextLength : contextLength != 0)) return false;
    std::memcpy(nonce_, input.payload + 1, kNonceSize);
    nonce_[kNonceSize] = 0;
    if (!hex(nonce_, kNonceSize) ||
        !decodePairingRecord(input.payload + kPairOffset, pairLength, decoded_.pairing) ||
        decoded_.pairing.role != v4::Role::Motion ||
        std::strcmp(decoded_.pairing.localPhysicalId, physical_)) return false;
    decoded_.hasContext = hasContext == 1;
    if (!decoded_.hasContext) clearContext(decoded_.context);
    return !decoded_.hasContext ||
        (decodeContextIdentity(input.payload + flagOffset + 3, contextLength, decoded_.context) &&
         !std::strcmp(decoded_.context.deviceId, decoded_.pairing.deviceId));
}

void BoardInstall::reply(const v4::Message& request, CommissioningResult result) {
    output_.kind = v4::Kind::MigrationInstall;
    output_.senderBoot = boot_;
    output_.receiverBoot = request.senderBoot;
    output_.messageId = request.messageId;
    output_.length = 34;
    output_.payload[0] = kResponse;
    std::memcpy(output_.payload + 1, request.payload + 1, kNonceSize);
    output_.payload[33] = encodeResult(result);
    pendingOutput_ = true;
}

void BoardInstall::receive(const v4::Frame& frame, uint32_t nowMs) {
    if (!initialized_ || !receiveAssembler_ || !receiveScratch_ ||
        frame.kind != v4::Kind::MigrationInstall || !v4::validFrame(frame) ||
        frame.receiverBoot != boot_ || frame.senderBoot == boot_) return;
    poll(nowMs);
    // A live unqueued slot is immutable; Brain's expired request is withdrawn
    // by poll(), never overwritten with new bytes while still exposed.
    if (pendingOutput_) return;
    if (role_ == v4::Role::Brain &&
        (state_ != BoardInstallState::Pending || frame.senderBoot != peerBoot_ ||
         frame.messageId != requestId_ || frame.total != 34)) return;
    if (receiveAssembler_->accept(frame, nowMs, *receiveScratch_) != v4::AssemblyResult::Complete) return;
    const auto& input = *receiveScratch_;
    if (role_ == v4::Role::Brain) {
        if (input.length != 34 || input.payload[0] != kResponse ||
            std::memcmp(input.payload + 1, nonce_, kNonceSize) ||
            !decodeResult(input.payload[33], result_)) return;
        state_ = BoardInstallState::Complete;
        return;
    }
    uint8_t digest[32];
    if (!fingerprint(input, digest)) return;
    if (input.senderBoot == ownerReplay_.boot && input.messageId <= ownerReplay_.id) {
        if (input.messageId == ownerReplay_.id && input.length == ownerReplay_.length &&
            !std::memcmp(digest, ownerReplay_.digest, sizeof(digest))) {
            result_ = ownerReplay_.result;
            state_ = target_ ? BoardInstallState::Complete : BoardInstallState::Unavailable;
            reply(input, result_);
        }
        return;
    }
    // Rejections carry no authority to establish an owner's ID watermark.
    // Cache only this exact tuple. A changed request, even at the same boot/ID,
    // must be decoded and authorized rather than borrowing a denial's fence.
    if (input.senderBoot == rejectedReplay_.boot && input.messageId == rejectedReplay_.id &&
        input.length == rejectedReplay_.length &&
        !std::memcmp(digest, rejectedReplay_.digest, sizeof(digest))) {
        result_ = rejectedReplay_.result;
        state_ = target_ ? BoardInstallState::Complete : BoardInstallState::Unavailable;
        reply(input, result_);
        return;
    }
    // Never advance the replay fence on partial/malformed/foreign-identity data.
    // A different boot's fully valid request still needs the owner's permission.
    if (!decodeRequest()) return;
    result_ = target_ ? target_->install(decoded_, nonce_, input.senderBoot)
                      : CommissioningResult::StateMissing;
    // Normalize an invalid target enum without leaking its implementation order.
    decodeResult(encodeResult(result_), result_);
    // Only a durable success can establish or advance the owner replay fence.
    Replay& replay = result_ == CommissioningResult::Installed ||
                     result_ == CommissioningResult::AlreadyInstalled
                     ? ownerReplay_ : rejectedReplay_;
    replay.boot = input.senderBoot;
    replay.id = input.messageId;
    replay.length = input.length;
    std::memcpy(replay.digest, digest, sizeof(digest));
    replay.result = result_;
    state_ = target_ ? BoardInstallState::Complete : BoardInstallState::Unavailable;
    reply(input, result_);
}

void BoardInstall::poll(uint32_t nowMs) {
    if (!initialized_ || !receiveAssembler_) return;
    receiveAssembler_->expire(nowMs);
    if (role_ == v4::Role::Brain && state_ == BoardInstallState::Pending &&
        uint32_t(nowMs - requestedAt_) >= kResponseMs) {
        receiveAssembler_->cancelMessage(v4::Kind::MigrationInstall, peerBoot_, boot_, requestId_);
        // Withdraw only our not-yet-copied slot. Already queued UART data and
        // NVS writes cannot be cancelled or assumed rolled back by this timer.
        pendingOutput_ = false;
        state_ = BoardInstallState::TimedOut;
    }
}

} }
