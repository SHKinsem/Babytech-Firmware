#include "BoardTransmitV4.h"

namespace babytech { namespace v4 {

bool Transmitter::enqueue(const Message& message) {
    if (!healthy_) return false;
    Frame first;
    if (!fragment(message, 0, first)) return false;
    if (isControl(message.kind)) return enqueueControl(first);
    if (ordinaryPending_) return false;
    ordinary_ = message;
    ordinaryOffset_ = 0;
    ordinaryPending_ = true;
    ordinaryTimed_ = false;
    ordinaryTimedOut_ = false;
    return true;
}

bool Transmitter::enqueueTimedOrdinary(const Message& message, uint32_t nowMs,
                                      uint32_t ttlMs, uint32_t firstFrameBudgetMs) {
    if (message.kind != Kind::Command || !ttlMs || ttlMs > 5000 ||
        !firstFrameBudgetMs || ttlMs <= firstFrameBudgetMs) return false;
    if (!enqueue(message)) return false;
    ordinaryStartedAt_ = nowMs;
    ordinaryTtlMs_ = ttlMs;
    firstFrameBudgetMs_ = firstFrameBudgetMs;
    ordinaryTimed_ = true;
    return true;
}

bool Transmitter::enqueueControl(const Frame& frame) {
    if (!healthy_ || !isControl(frame.kind) || !validFrame(frame)) return false;
    if (frame.kind == Kind::Stop) {
        // A full reserved slot is backpressure, not an accepted/dropped Stop.
        if (stopPending_) return false;
        stop_ = frame;
        stopPending_ = true;
    } else if (frame.kind == Kind::Heartbeat) {
        if (heartbeatPending_) return false;
        heartbeat_ = frame;
        heartbeatPending_ = true;
    } else {
        if (controlCount_ == 2) return false;
        controls_[(controlHead_ + controlCount_) % 2] = frame;
        ++controlCount_;
    }
    ordinaryTimedOut_ = false;
    return true;
}

bool Transmitter::prepare() {
    Frame frame;
    if (stopPending_) { slot_ = Slot::Stop; frame = stop_; }
    else if (heartbeatPending_) { slot_ = Slot::Heartbeat; frame = heartbeat_; }
    else if (controlCount_) { slot_ = Slot::Control; frame = controls_[controlHead_]; }
    else if (ordinaryPending_) {
        slot_ = Slot::Ordinary;
        if (!fragment(ordinary_, ordinaryOffset_, frame)) { healthy_ = false; return false; }
    } else return false;
    framePayloadSize_ = frame.length;
    wireSize_ = encode(frame, wire_, sizeof(wire_));
    wireOffset_ = 0;
    if (!wireSize_) { healthy_ = false; return false; }
    return true;
}

void Transmitter::finishFrame() {
    switch (slot_) {
        case Slot::Stop: stopPending_ = false; break;
        case Slot::Heartbeat: heartbeatPending_ = false; break;
        case Slot::Control:
            controlHead_ = (controlHead_ + 1) % 2;
            --controlCount_;
            break;
        case Slot::Ordinary:
            ordinaryOffset_ += framePayloadSize_;
            if (cancelAfterFrame_ || ordinaryOffset_ == ordinary_.length) {
                ordinaryPending_ = false;
                cancelAfterFrame_ = false;
                ordinaryTimed_ = false;
            }
            break;
        case Slot::None: break;
    }
    slot_ = Slot::None;
    wireOffset_ = wireSize_ = 0;
    frameInvalidated_ = false;
}

bool Transmitter::pump(ByteSink& sink) {
    return pumpInternal(sink, false, 0);
}

bool Transmitter::pump(uint32_t nowMs, ByteSink& sink) {
    return pumpInternal(sink, true, nowMs);
}

bool Transmitter::pumpInternal(ByteSink& sink, bool hasClock, uint32_t nowMs) {
    if (!healthy_) return false;
    if (hasClock && ordinaryPending_ && ordinaryTimed_) {
        const uint32_t elapsed = uint32_t(nowMs - ordinaryStartedAt_);
        if (elapsed >= ordinaryTtlMs_ ||
            (!ordinaryOffset_ && elapsed >= firstFrameBudgetMs_)) {
            ordinaryTimedOut_ = true;
            invalidateOrdinary();
        }
    }
    if (slot_ != Slot::None && !wireOffset_) {
        // A zero-byte write has not put a frame on the wire; reselect priority.
        slot_ = Slot::None;
        wireSize_ = 0;
    }
    // Do not prefill the UART with another frame while one is on the wire.
    if (slot_ == Slot::None && (!sink.idle() || !prepare())) return false;
    if (slot_ == Slot::Ordinary && ordinaryTimed_ && !hasClock) return false;
    const size_t room = sink.available();
    const size_t remaining = wireSize_ - wireOffset_;
    const size_t count = room < remaining ? room : remaining;
    if (!count) return false;
    const size_t written = sink.write(wire_ + wireOffset_, count);
    if (written > count) { healthy_ = false; return false; }
    wireOffset_ += written;
    if (wireOffset_ == wireSize_) finishFrame();
    return written != 0;
}

void Transmitter::cancelOrdinary() {
    if (!ordinaryPending_) return;
    if (slot_ == Slot::Ordinary && wireOffset_) {
        // Finish a partial frame before Stop; never interleave bytes of two frames.
        cancelAfterFrame_ = true;
        return;
    }
    ordinaryPending_ = false;
    cancelAfterFrame_ = false;
    ordinaryTimed_ = false;
    if (slot_ == Slot::Ordinary) {
        slot_ = Slot::None;
        wireOffset_ = wireSize_ = 0;
    }
}

void Transmitter::invalidateOrdinary() {
    if (!ordinaryPending_) return;
    if (slot_ == Slot::Ordinary && wireOffset_) {
        // The final CRC byte is still mutable even if its first byte was sent.
        // Flip once only: repeated invalidation must not restore a valid CRC.
        if (!frameInvalidated_) {
            wire_[wireSize_ - 1] ^= 1;
            frameInvalidated_ = true;
        }
    }
    cancelOrdinary();
    // A poisoned residual frame is no longer an authorization and can drain
    // without a clock, allowing the reserved Stop slot to follow it.
    ordinaryTimed_ = false;
}

void Transmitter::reset() { *this = Transmitter{}; }

bool Transmitter::pending() const {
    return ordinaryPending_ || stopPending_ || heartbeatPending_ || controlCount_ != 0;
}

} }
