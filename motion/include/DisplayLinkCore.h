#pragma once
#include "DemoFlowController.h"
#include "display_protocol.h"

namespace motion {
// Pure protocol endpoint: bounded caller-owned IO and a short ACK history.
class DisplayLinkCore {
public:
    explicit DisplayLinkCore(DemoFlowController& flow) : flow_(flow) {}
    size_t receive(uint8_t byte, uint32_t now, uint8_t* output, size_t capacity) {
        if (uint32_t(now - byteAt_) > 100) parser_.reset();
        byteAt_ = now;
        babytech::display::DisplayFrame frame;
        if (!parser_.push(byte, frame)) return 0;
        babytech::display::DisplayIntent intent;
        if (!babytech::display::decodeDisplayIntentPayload(frame, intent)) return 0;
        for (const auto& saved : history_) if (saved.valid && saved.sequence == frame.sequence)
            return ack(frame.sequence, saved.accepted, output, capacity);
        const bool accepted = flow_.start(now);
        history_[cursor_] = {true, frame.sequence, accepted};
        cursor_ = (cursor_ + 1) % history_.size();
        return ack(frame.sequence, accepted, output, capacity);
    }
    size_t state(uint32_t now, uint8_t* output, size_t capacity) {
        auto snapshot = flow_.snapshot();
        if (sent_ && babytech::display::displaySnapshotsEqual(snapshot, last_) && uint32_t(now - stateAt_) < 1000) return 0;
        uint8_t payload[babytech::display::kDisplayMaxPayloadSize];
        const auto n = babytech::display::encodeDisplaySnapshotPayload(snapshot, payload, sizeof(payload));
        const auto size = babytech::display::encodeDisplayFrame(babytech::display::DisplayMessageType::State,
            ++sequence_, payload, n, output, capacity);
        if (size) { last_ = snapshot; stateAt_ = now; sent_ = true; }
        return size;
    }
private:
    size_t ack(uint32_t seq, bool accepted, uint8_t* output, size_t capacity) {
        babytech::display::DisplayAck response;
        response.accepted = accepted;
        babytech::display::setDisplayAckReason(response, accepted ? "accepted" : "not_ready");
        uint8_t payload[babytech::display::kDisplayMaxPayloadSize];
        const auto n = babytech::display::encodeDisplayAckPayload(response, payload, sizeof(payload));
        return babytech::display::encodeDisplayFrame(babytech::display::DisplayMessageType::Ack, seq,
            payload, n, output, capacity);
    }
    struct Saved { bool valid = false; uint32_t sequence = 0; bool accepted = false; };
    std::array<Saved, 8> history_{};
    size_t cursor_ = 0;
    DemoFlowController& flow_;
    babytech::display::DisplayFrameParser parser_;
    babytech::display::DisplaySnapshot last_;
    bool sent_ = false;
    uint32_t sequence_ = 0, stateAt_ = 0, byteAt_ = 0;
};
}
