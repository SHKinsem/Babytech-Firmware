#pragma once
#include "BoardMotion.h"
#include "CommandQueue.h"

// Cancel the HTTP queue only when the UART endpoint accepts a real STOP.
// Keeping this at the backend boundary also honors endpoint deduplication,
// payload validation and boot/session checks without reimplementing them.
class QueueBoardMotion : public BoardMotion {
public:
    QueueBoardMotion(motion::MotorControl& motor, motion::CommandQueue& queue, motion::DeviceAPI& api)
        : BoardMotion(motor, api), queue_(queue), api_(api) {}
    bool busy() const override { return queue_.active() || BoardMotion::busy(); }
    babytech::v2::Reason enable(uint8_t id, bool enabled) override {
        // A disable preempts the queue so later steps cannot re-enable motion.
        if (!enabled && queue_.active()) api_.cancelProgram("uart_disable");
        return BoardMotion::enable(id, enabled);
    }
    void stop() override {
        if (queue_.active()) api_.cancelProgram("uart_stop");
        BoardMotion::stop();
    }
private:
    motion::CommandQueue& queue_;
    motion::DeviceAPI& api_;
};
