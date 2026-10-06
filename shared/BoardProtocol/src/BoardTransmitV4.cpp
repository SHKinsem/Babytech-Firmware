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
            }
            break;
        case Slot::None: break;
    }
    slot_ = Slot::None;
    wireOffset_ = wireSize_ = 0;
}

bool Transmitter::pump(ByteSink& sink) {
    if (!healthy_) return false;
    if (slot_ != Slot::None && !wireOffset_) {
        // A zero-byte write has not put a frame on the wire; reselect priority.
        slot_ = Slot::None;
        wireSize_ = 0;
    }
    // Do not prefill the UART with another frame while one is on the wire.
    if (slot_ == Slot::None && (!sink.idle() || !prepare())) return false;
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
    if (slot_ == Slot::Ordinary) {
        slot_ = Slot::None;
        wireOffset_ = wireSize_ = 0;
    }
}

void Transmitter::reset() { *this = Transmitter{}; }

bool Transmitter::pending() const {
    return ordinaryPending_ || stopPending_ || heartbeatPending_ || controlCount_ != 0;
}

} }
