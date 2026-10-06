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
    bool enqueueControl(const Frame& frame);
    bool pump(ByteSink& sink);
    void cancelOrdinary();
    void reset();
    bool pending() const;
    bool ordinaryPending() const { return ordinaryPending_; }
    bool healthy() const { return healthy_; }
private:
    enum class Slot { None, Stop, Heartbeat, Control, Ordinary };
    bool prepare();
    void finishFrame();
    Message ordinary_{};
    Frame stop_{}, heartbeat_{}, controls_[2]{};
    uint8_t wire_[kMaxFrame]{};
    size_t wireSize_ = 0;
    size_t wireOffset_ = 0;
    uint16_t ordinaryOffset_ = 0;
    uint16_t framePayloadSize_ = 0;
    uint8_t controlHead_ = 0;
    uint8_t controlCount_ = 0;
    Slot slot_ = Slot::None;
    bool ordinaryPending_ = false;
    bool stopPending_ = false;
    bool heartbeatPending_ = false;
    bool cancelAfterFrame_ = false;
    bool healthy_ = true;
};

} }
