#pragma once

#include <MotionStateStore.h>
#include <ProductEventMessages.h>
#include <cstring>

namespace motion {

// One loop owner. Motion's existing durable results remain the outbox; the
// copied receipt only defers Flash until after urgent hardware/UART servicing.
template<class Link>
class MotionResultDelivery {
public:
    MotionResultDelivery(Link& link, babytech::boardlink::MotionStateStore& store)
        : link_(link), store_(store) {}

    bool receipt(const babytech::boardlink::CloudReceipt& receipt) {
        if (!store_.ready() || std::strncmp(receipt.deviceId, store_.state().pairing.deviceId,
                                           sizeof(receipt.deviceId)) || !find(receipt.eventId)) return false;
        if (receiptPending_)
            return !std::strncmp(receipt_.eventId, receipt.eventId, sizeof(receipt.eventId));
        receipt_ = receipt;
        receiptPending_ = true;
        return true;
    }

    void poll(uint32_t nowMs, bool stationary, bool maintenance = false) {
        if (maintenance || !store_.ready()) return;
        if (receiptPending_) {
            const auto* slot = find(receipt_.eventId);
            if (!slot) receiptPending_ = false;
            else {
                const auto written = store_.acknowledge(receipt_.eventId, slot->completed, stationary);
                if (written == babytech::boardlink::MotionWrite::Stored ||
                    written == babytech::boardlink::MotionWrite::Unchanged) receiptPending_ = false;
            }
            if (!store_.ready()) return;
        }
        if (attempted_ && uint32_t(nowMs - attemptedAt_) < kAttemptIntervalMs) return;
        const auto& state = store_.state();
        const size_t count = state.pendingResultCount +
            (state.slot.kind == babytech::boardlink::MotionSlotKind::Terminal ? 1 : 0);
        if (!count) return;
        if (next_ >= count) next_ = 0;
        const auto& slot = next_ < state.pendingResultCount ? state.pendingResults[next_] : state.slot;
        // Busy UART capacity must not consume the retry interval: periodic
        // STATUS otherwise phase-locks every attempt onto an occupied slot.
        // Rotate admitted items (even if Cloud later rejects them), or an
        // failed slot conversion, without changing any persisted evidence.
        const bool encodable = babytech::boardlink::terminalEventFromSlot(state.pairing, slot, event_);
        if (encodable && !link_.publishTerminal(event_, nowMs)) return;
        attempted_ = true;
        attemptedAt_ = nowMs;
        next_ = (next_ + 1) % count;
    }

private:
    const babytech::boardlink::MotionExecutionSlot* find(const char (&eventId)[59]) const {
        const auto& state = store_.state();
        for (size_t i = 0; i < state.pendingResultCount; ++i)
            if (!std::strncmp(state.pendingResults[i].eventId, eventId, sizeof(eventId)))
                return &state.pendingResults[i];
        if (state.slot.kind == babytech::boardlink::MotionSlotKind::Terminal &&
            !std::strncmp(state.slot.eventId, eventId, sizeof(eventId))) return &state.slot;
        return nullptr;
    }
    static constexpr uint32_t kAttemptIntervalMs = 1000;
    Link& link_;
    babytech::boardlink::MotionStateStore& store_;
    babytech::boardlink::TerminalEvent event_{};
    babytech::boardlink::CloudReceipt receipt_{};
    uint32_t attemptedAt_ = 0;
    size_t next_ = 0;
    bool attempted_ = false;
    bool receiptPending_ = false;
};

} // namespace motion
