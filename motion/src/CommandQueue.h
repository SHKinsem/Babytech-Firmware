#pragma once
// Non-blocking board queue executor.
//
// The board owns the whole run: the browser may disconnect at any time and the
// queue keeps going. Nothing here allocates per step, nothing blocks, and the
// plan parsed at start stays immutable until the run is cancelled or finished.
//
// One supervised action may start per poll() at most, raw steps are spaced at
// least kRawSpacingMs apart, and a step only advances on evidence:
//   * enable/disable: actual F3 acknowledgement + fresh stationary feedback
//   * move:           the controller's supervised CD outcome
//   * home:           the controller's supervised 9A outcome (12/22 no-motion is
//                     reported distinctly and the queue continues)
//   * torque/velocity: the frame is sent once, the queue times the run, stops it
//                     and waits for a fresh stationary confirmation
//   * stop:           wait until the stop is confirmed stationary
//   * hex/can:        reported as SENT. A raw frame is never equivalent to a bus
//                     acknowledgement, a completed move or a stopped motor.
//
// The first rejection/fault/timeout stops everything and keeps that first error;
// a timeout never advances silently.
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

    // Validates the whole program (no CAN frame is sent on failure) and starts
    // it. 202 on success, 400 with a stable reason + lastErrorLine() for an
    // invalid program, 409 while a run or another supervised operation is live.
    Result start(const char* text, size_t length, long repeat,
                 const QueueRotationSource& rotation, uint32_t now);

    // Cancels the run (if any) and stops everything: broadcast 9C then the FE
    // broadcast stop. Returns 202 when a run was cancelled, 200 when idle.
    Result cancel(const char* reason);

    bool active() const { return state_ == QueueState::Running; }
    bool containsRaw() const { return program_.hasRaw; }
    QueueState state() const { return state_; }
    uint32_t runId() const { return runId_; }
    uint16_t lastErrorLine() const { return errorLine_; }
    const char* message() const { return message_; }

    // Complete JSON object for GET /api/queue.
    String statusJson() const;

private:
    enum Phase : uint8_t {
        kPhaseIdle = 0,
        kPhaseWatch,     // selected a new target, waiting for fresh feedback
        kPhaseEnable,    // F3 sent, waiting ack + stationary
        kPhaseMove,      // CD dispatched, waiting the supervised outcome
        kPhaseHome,      // 9A dispatched, waiting the supervised outcome
        kPhaseTimed,     // C5/C6 running, queue timer
        kPhaseStop,      // FE sent, waiting stationary confirmation
        kPhaseWait,      // plain wait step
    };

    void beginStep(uint32_t now);
    void watchTarget(uint32_t now);
    void waitEnable(uint32_t now);
    void waitMove(uint32_t now);
    void waitHome(uint32_t now);
    void waitTimed(uint32_t now);
    void waitStop(uint32_t now);
    void waitTimer(uint32_t now);
    void dispatch(uint32_t now, const QueueStep& step);
    void dispatchRaw(uint32_t now, const QueueStep& step);

    void advance(uint32_t now);
    void finish(QueueState state, const char* message);
    void fail(uint32_t now, const char* reason, uint16_t line);
    bool stopEverything();
    void setMessage(const char* text);
    const QueueStep* currentStep() const;
    bool stationary(const MotorControl::Snapshot& s) const;

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
    uint32_t evidenceAt_ = 0;         // 12/22 answer seen at (0 = not seen yet)
    uint32_t lastRawAt_ = 0;          // spacing between raw frames
    uint16_t errorLine_ = 0;          // line of the first failure / bad program
    char message_[64] = "idle";
};

}  // namespace motion
