#pragma once
// Board queue: ordered command sender with opt-in completion observation.
// Plain commands advance after transmission. Only move/home with a trailing
// await enter kPhaseMotion; wait and legacy timed torque/velocity use timers.
// Await observes driver evidence without invoking manual controller policy:
// no implicit enable, retry, timeout stop, or automatic fault latching.
// Missing feedback leaves await pending; explicit driver rejection fails it.
// Cancel remains an explicit broadcast abort/stop operation.
//
// Memory: the plan is a fixed array of 64 QueueStep values (about 6 KB) held by
// the queue object in .bss, plus one function-local static scratch buffer of the
// same size used for validation. No whole-program value is ever placed on a
// task stack, and nothing is allocated per step: the ESP32 loop task keeps its
// default 8 KB stack.

#include <stddef.h>
#include <stdint.h>

#include "Arduino.h"
#include "DebugLimits.h"
#include "MotorControl.h"
#include "QueueProgram.h"

namespace motion {

enum class QueueState : uint8_t { Idle = 0, Running, Done, Failed, Cancelled };

// Frozen names for the frontend/queue status contract (GET /api/queue):
//   state: "idle" | "running" | "done" | "failed" | "cancelled"
//   message while running: the current step note ("running" when there is none,
//   e.g. "home_no_motion" after a 12/22 answer); terminal states carry
//   "done"/"cancelled" or the stable failure reason.
const char* queueStateName(QueueState state);

class CommandQueue {
public:
    explicit CommandQueue(MotorControl& motor) : motor_(motor) {}

    // Call every loop after MotorControl::poll().
    void poll(uint32_t now);

    // Parses the whole program (no CAN frame is sent, and no state is cleared,
    // when the text is invalid) and starts sending it. 202 on success, 400 with
    // a stable reason + lastErrorLine() for an invalid program, 409 while a run
    // is still going, 503 when the bus cannot transmit at all.
    Result start(const char* text, size_t length, long repeat,
                 const QueueRotationSource& rotation, uint32_t now);

    // Cancels the run (if any) and stops everything: broadcast 9C then the FE
    // broadcast stop. Returns 202 when a run was cancelled, 200 when idle.
    Result cancel(const char* reason);

    // Explicit operator reset: cancel pending steps and attempt stop exactly
    // once, then clear queue/controller ownership even if stop cannot be sent.
    // The result reports stop transmission only, never physical completion.
    Result clearControlState();

    bool active() const { return state_ == QueueState::Running; }
    bool containsRaw() const { return program_.hasRaw; }
    QueueState state() const { return state_; }
    uint32_t runId() const { return runId_; }
    uint16_t lastErrorLine() const { return errorLine_; }
    const char* message() const { return message_; }

    // Complete JSON object for GET /api/queue.
    String statusJson() const;

private:
    // Timers and explicit move/home completion observation are nonblocking.
    enum Phase : uint8_t {
        kPhaseIdle = 0,   // ready to send the next step
        kPhaseWait,       // a user-written `wait MS`
        kPhaseTimed,      // legacy timed torque/velocity: send FE at the deadline
        kPhaseMotion,
    };

    void beginStep(uint32_t now);
    void dispatchStep(uint32_t now, const QueueStep& step);
    bool encodeAndSend(const QueueStep& step);
    bool sendStopFrame(uint8_t id);
    bool stopEverything();
    void observeMotion(uint32_t now);
    uint32_t observedAckAt_ = 0, queryAt_ = 0, donePosAt_ = 0, doneVelAt_ = 0;
    uint8_t queryIndex_ = 0, doneSamples_ = 0;
    uint32_t homeProofAt_ = 0;
    bool accepted_ = false, homeSeenRunning_ = false, homeComplete_ = false;

    void advance(uint32_t now);
    void finish(QueueState state, const char* message);
    void fail(uint32_t now, const char* reason, uint16_t line);
    void setMessage(const char* text);
    const QueueStep* currentStep() const;

    MotorControl& motor_;
    QueueProgram program_{};
    QueueState state_ = QueueState::Idle;
    uint32_t runId_ = 0;
    uint32_t repeat_ = 1;
    uint32_t iteration_ = 0;          // 0-based index of the running iteration
    uint16_t stepIndex_ = 0;          // 0-based index into program_.steps
    uint8_t phase_ = kPhaseIdle;
    uint32_t phaseAt_ = 0;            // when the current phase started
    uint32_t deadlineAt_ = 0;         // wait/timed phase deadline
    uint32_t lastRawAt_ = 0;          // spacing between raw frames
    uint16_t errorLine_ = 0;          // line of the first failure / bad program
    char message_[64] = "idle";
};

}  // namespace motion
