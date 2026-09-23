#pragma once
#include "BoardMotion.h"
#include "CommandQueue.h"

// Cancel the HTTP queue only when the UART endpoint accepts a real STOP.
// Keeping this at the backend boundary also honors endpoint deduplication,
// payload validation and boot/session checks without reimplementing them.
class QueueBoardMotion : public BoardMotion {
public:
    QueueBoardMotion(motion::MotorControl& motor, motion::CommandQueue& queue)
        : BoardMotion(motor), queue_(queue) {}
    bool busy() const override { return queue_.active() || BoardMotion::busy(); }
    void stop() override {
        if (queue_.active()) queue_.cancel("uart_stop");
        BoardMotion::stop();
    }
private:
    motion::CommandQueue& queue_;
};
