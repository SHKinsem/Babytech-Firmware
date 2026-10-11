#pragma once

#include "BoardProtocolV4.h"

namespace babytech { namespace v4 {

class ByteSink {
public:
    virtual ~ByteSink() {}
    virtual bool idle() const = 0;
    virtual size_t available() const = 0;
    virtual size_t write(const uint8_t* bytes, size_t size) = 0;
};

// UART buffering only; transport ACKs and business acceptance are separate.
class Transmitter {
public:
    bool enqueue(const Message& message);
    // Deadlines are exclusive and frozen at acceptance, not at first pump.
    bool enqueueTimedOrdinary(const Message& message, uint32_t nowMs, uint32_t ttlMs,
                              uint32_t firstFrameBudgetMs = kCommandFirstFrameBudgetMs);
    bool enqueueControl(const Frame& frame);
    // Clockless pumping refuses guarded ordinary bytes; controls remain usable.
    bool pump(ByteSink& sink);
    // nowMs must be fresh; write() must be nonblocking. This guards local writes,
    // not physical UART drain time or remote business acceptance.
    bool pump(uint32_t nowMs, ByteSink& sink);
    void cancelOrdinary();
    // Unlike cancelOrdinary(), poison any partial frame before draining it.
    // Already completed frames cannot be revoked. Does not change control slots.
    void invalidateOrdinary();
    // Cancel only this owner's message, not a later ordinary message in the slot.
    void invalidateOrdinary(Kind kind, uint32_t messageId);
    void reset();
    bool pending() const;
    bool ordinaryPending() const { return ordinaryPending_; }
    // Sticky until any successful enqueue (including control) or reset.
    bool ordinaryTimedOut() const { return ordinaryTimedOut_; }
    bool healthy() const { return healthy_; }
private:
    enum class Slot { None, Stop, Heartbeat, Control, Ordinary };
    bool prepare();
    bool pumpInternal(ByteSink& sink, bool hasClock, uint32_t nowMs);
    void finishFrame();
    Message ordinary_{};
    Frame stop_{}, heartbeat_{}, controls_[2]{};
    uint8_t wire_[kMaxFrame]{};
    size_t wireSize_ = 0;
    size_t wireOffset_ = 0;
    uint16_t ordinaryOffset_ = 0;
    uint16_t framePayloadSize_ = 0;
    uint32_t ordinaryStartedAt_ = 0;
    uint32_t ordinaryTtlMs_ = 0;
    uint32_t firstFrameBudgetMs_ = 0;
    uint8_t controlHead_ = 0;
    uint8_t controlCount_ = 0;
    Slot slot_ = Slot::None;
    bool ordinaryPending_ = false;
    bool stopPending_ = false;
    bool heartbeatPending_ = false;
    bool cancelAfterFrame_ = false;
    bool ordinaryTimed_ = false;
    bool ordinaryTimedOut_ = false;
    bool frameInvalidated_ = false;
    bool healthy_ = true;
};

} }
