#pragma once

#include <cstdint>
#include <cstring>

#include "X42sProtocol.h"

namespace motion {

// One manual 0x4C write, its acknowledgement, and the budgeted 0x22 readback.
// This object owns transaction evidence only. The caller owns CAN I/O, query
// scheduling, command policy, and the decision to start a transaction.
class ConfigTransaction {
public:
    enum class FrameResult : uint8_t { Ignored, Consumed, ReadbackComplete };

    // `logical` is a validated 20-byte 0x4C command already sent on the bus.
    void start(const uint8_t* logical, uint32_t now) {
        const uint32_t next = sequence_ + 1;
        *this = ConfigTransaction{};
        sequence_ = next;
        id_ = logical[0];
        state_ = State::WaitAck;
        started_ = now;
        std::memcpy(expected_, logical + 4, sizeof(expected_));
    }

    FrameResult acceptFrame(const CanRawFrame& frame, uint32_t now) {
        if (!pending() || !frame.extended || frame.remote ||
            frame.length < 1 || frame.length > 8 ||
            (frame.identifier >> 8) != id_) return FrameResult::Ignored;

        const uint8_t packet = static_cast<uint8_t>(frame.identifier);
        if (frame.data[0] == 0x4C) {
            if (packet != 0 || frame.length != 3 || frame.data[2] != 0x6B ||
                state_ != State::WaitAck) return FrameResult::Consumed;
            ack_ = frame.data[1];
            if (ack_ != 0x02) {
                state_ = State::Rejected;
                return FrameResult::Consumed;
            }
            state_ = State::WaitReadback;
            readAt_ = now;
            // The caller may request the read only after observing an empty RX
            // queue; until it is sent, 0x22 packets cannot become evidence.
            return FrameResult::Consumed;
        }
        if (frame.data[0] != 0x22 || state_ != State::WaitReadback)
            return FrameResult::Ignored;
        if (!readIssued_) return FrameResult::Consumed;

        // Logical reply: 15 parameter bytes and 0x6B, split into 7/7/2.
        // Duplicates, gaps, and truncated packets do not advance assembly.
        const uint8_t take = packet < 2 ? 7 : 2;
        if (packet != packet_ || packet > 2 || frame.length != take + 1)
            return FrameResult::Consumed;
        std::memcpy(actual_ + received_, frame.data + 1, take);
        received_ += take;
        ++packet_;
        if (received_ != sizeof(actual_)) return FrameResult::Consumed;
        if (actual_[15] != 0x6B) {
            packet_ = received_ = 0;
            return FrameResult::Consumed;
        }
        state_ = std::memcmp(expected_, actual_, sizeof(expected_)) == 0
            ? State::Verified : State::Mismatch;
        return FrameResult::ReadbackComplete;
    }

    bool wantsReadQuery() const {
        return state_ == State::WaitReadback && !readIssued_;
    }

    // Returns true only for this transaction's 0x22 query, so the caller can
    // release that demand whether the send succeeded or failed.
    bool readQuerySent(uint8_t id, uint8_t field, bool sent, uint32_t now) {
        if (id != id_ || field != 0x22 || !wantsReadQuery()) return false;
        readIssued_ = true;
        readAt_ = now;
        if (!sent) state_ = State::ReadTxFailed;
        return true;
    }

    void poll(uint32_t now) {
        if (state_ == State::WaitAck && uint32_t(now - started_) > kTimeoutMs)
            state_ = State::AckTimeout;
        if (state_ == State::WaitReadback && uint32_t(now - readAt_) > kTimeoutMs)
            state_ = State::ReadbackTimeout;
    }

    void cancel() {
        if (pending()) state_ = State::Cancelled;
    }

    bool pending() const { return state_ == State::WaitAck || state_ == State::WaitReadback; }
    bool failed() const { return state_ >= State::Rejected; }
    uint32_t sequence() const { return sequence_; }
    uint32_t started() const { return started_; }
    uint8_t id() const { return id_; }
    uint8_t ack() const { return ack_; }
    const uint8_t* expected() const { return expected_; }
    const uint8_t* actual() const { return actual_; }
    uint8_t received() const { return received_; }

    const char* message() const {
        switch (state_) {
            case State::WaitAck: return "config_wait_ack";
            case State::WaitReadback: return "config_wait_readback";
            case State::Verified: return "config_verified";
            case State::Rejected: return "config_rejected";
            case State::AckTimeout: return "config_ack_timeout";
            case State::ReadbackTimeout: return "config_readback_timeout";
            case State::Mismatch: return "config_mismatch";
            case State::ReadTxFailed: return "config_read_tx_failed";
            case State::Cancelled: return "config_cancelled";
            default: return "none";
        }
    }

private:
    static const uint32_t kTimeoutMs = 3000;
    enum class State : uint8_t {
        None, WaitAck, WaitReadback, Verified, Rejected, AckTimeout,
        ReadbackTimeout, Mismatch, ReadTxFailed, Cancelled
    };
    State state_ = State::None;
    bool readIssued_ = false;
    uint32_t sequence_ = 0, started_ = 0, readAt_ = 0;
    uint8_t id_ = 0, ack_ = 0, packet_ = 0, received_ = 0;
    uint8_t expected_[15] = {}, actual_[16] = {};
};

}  // namespace motion
