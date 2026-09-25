#pragma once
// Runtime state for one already-validated, immutable QueueProgram. No Arduino,
// CAN or heap dependency: the caller owns the fixed program storage and reports
// the outcome of each requested adapter action back to this state machine.

#include <stdint.h>

#include "QueueProgram.h"

namespace motion {

enum class QueueState : uint8_t { Idle = 0, Running, Done, Failed, Cancelled };

// Frozen names in GET /api/queue and the UART status projection.
inline const char* queueStateName(QueueState state) {
    switch (state) {
        case QueueState::Idle: return "idle";
        case QueueState::Running: return "running";
        case QueueState::Done: return "done";
        case QueueState::Failed: return "failed";
        case QueueState::Cancelled: return "cancelled";
    }
    return "idle";
}

class ProgramRunner {
public:
    enum class Phase : uint8_t { Idle = 0, Wait, Timed, Motion, Sync };
    enum class Action : uint8_t { None = 0, Dispatch, ObserveMotion, PollSync, TimedStop };

    void begin(const QueueProgram& program, uint32_t hash, uint32_t repeat,
               bool strictHome, uint32_t now) {
        program_ = &program;
        ++runId_;
        programHash_ = hash;
        repeat_ = repeat;
        iteration_ = 0;
        stepIndex_ = 0;
        phase_ = Phase::Idle;
        phaseAt_ = now;
        deadlineAt_ = 0;
        lastRawAt_ = 0;
        errorLine_ = 0;
        strictHome_ = strictHome;
        motionComplete_ = false;
        unconfirmedMotion_ = false;
        state_ = QueueState::Running;
        note("running");
    }

    // Exactly one adapter action may be requested per poll. In particular,
    // finishing an iteration never dispatches its first step in the same poll.
    Action tick(uint32_t now) {
        if (phase_ == Phase::Sync) return Action::PollSync;
        if (state_ != QueueState::Running) return Action::None;
        switch (phase_) {
            case Phase::Idle: {
                if (program_ == nullptr) { fail("internal_program", 0); return Action::None; }
                if (stepIndex_ >= program_->count) {
                    ++iteration_;
                    if (iteration_ >= repeat_)
                        finish(motionComplete_ ? "sync_motion_complete" :
                               program_->hasRaw ? "raw_frames_submitted" : "done");
                    else stepIndex_ = 0;
                    return Action::None;
                }
                const QueueStep& step = program_->steps[stepIndex_];
                if ((step.action == QueueAction::Hex || step.action == QueueAction::Can) &&
                    uint32_t(now - lastRawAt_) < kRawSpacingMs) return Action::None;
                return Action::Dispatch;
            }
            case Phase::Wait:
                if (int32_t(now - deadlineAt_) >= 0) advance(now);
                return Action::None;
            case Phase::Timed:
                return int32_t(now - deadlineAt_) >= 0 ? Action::TimedStop : Action::None;
            case Phase::Motion: return Action::ObserveMotion;
            case Phase::Sync: return Action::PollSync;
        }
        fail("internal_phase", 0);
        return Action::None;
    }

    void noteDispatch(QueueAction action, bool awaitCompletion) {
        if (!active()) return;
        if (action != QueueAction::Wait) motionComplete_ = false;
        if ((!awaitCompletion && (action == QueueAction::Move || action == QueueAction::Home ||
                                  action == QueueAction::Torque || action == QueueAction::Velocity)) ||
            action == QueueAction::Hex || action == QueueAction::Can)
            unconfirmedMotion_ = true;
    }
    void waitStep(uint32_t now, uint32_t duration) {
        if (!active()) return;
        phase_ = Phase::Wait;
        deadlineAt_ = now + duration;
    }
    void timedStep(uint32_t now, uint32_t duration) {
        if (!active()) return;
        phase_ = Phase::Timed;
        deadlineAt_ = now + duration;
    }
    void awaitStep(uint32_t now, const char* message) {
        if (!active()) return;
        phase_ = Phase::Motion;
        phaseAt_ = now;
        note(message);
    }
    void syncStep() { if (active()) { phase_ = Phase::Sync; note("sync_checking"); } }
    void rawSent(uint32_t now) { if (active()) lastRawAt_ = now; }
    void markAccepted(uint32_t at) { if (active()) phaseAt_ = at; }
    void completeSync(uint8_t groupSize, uint32_t now) {
        if (!active() || phase_ != Phase::Sync) return;
        stepIndex_ = static_cast<uint16_t>(stepIndex_ + groupSize + 1);
        advance(now); // skip the closing delimiter too
        motionComplete_ = !unconfirmedMotion_;
        note("sync_motion_complete");
    }
    void advance(uint32_t now) {
        if (!active()) return;
        ++stepIndex_;
        phase_ = Phase::Idle;
        phaseAt_ = now;
    }
    void finish(const char* message) {
        if (!active()) return;
        state_ = QueueState::Done;
        phase_ = Phase::Idle;
        note(message);
    }
    void fail(const char* reason, uint16_t line) {
        if (state_ != QueueState::Running) return; // preserve the first failure
        if (errorLine_ == 0) errorLine_ = line;
        state_ = QueueState::Failed;
        phase_ = Phase::Idle;
        note(reason != nullptr ? reason : "failed");
    }
    bool cancel(const char* reason) {
        if (state_ != QueueState::Running) return false;
        state_ = QueueState::Cancelled;
        phase_ = Phase::Idle;
        note(reason != nullptr ? reason : "cancelled");
        return true;
    }
    void clear() {
        program_ = nullptr;
        state_ = QueueState::Idle;
        phase_ = Phase::Idle;
        stepIndex_ = 0;
        iteration_ = 0;
        repeat_ = 1;
        errorLine_ = 0;
        phaseAt_ = deadlineAt_ = lastRawAt_ = 0;
        strictHome_ = false;
        motionComplete_ = false;
        unconfirmedMotion_ = false;
        note("control_state_cleared");
    }
    void recordValidationFailure(uint16_t line, const char* reason) {
        errorLine_ = line;
        note(reason);
    }
    void note(const char* text) {
        if (text == nullptr) text = "";
        size_t i = 0;
        for (; text[i] != '\0' && i + 1 < sizeof(message_); ++i) message_[i] = text[i];
        message_[i] = '\0';
    }

    const QueueStep* currentStep() const {
        return program_ != nullptr && stepIndex_ < program_->count ?
            &program_->steps[stepIndex_] : nullptr;
    }
    bool active() const { return state_ == QueueState::Running; }
    QueueState state() const { return state_; }
    uint32_t runId() const { return runId_; }
    uint32_t programHash() const { return programHash_; }
    uint32_t repeat() const { return repeat_; }
    uint32_t iteration() const { return iteration_; }
    uint16_t stepIndex() const { return stepIndex_; }
    Phase phase() const { return phase_; }
    uint32_t phaseAt() const { return phaseAt_; }
    uint32_t deadlineAt() const { return deadlineAt_; }
    uint32_t lastRawAt() const { return lastRawAt_; }
    uint16_t errorLine() const { return errorLine_; }
    const char* message() const { return message_; }
    bool strictHome() const { return strictHome_; }
    bool motionComplete() const { return motionComplete_; }
    bool unconfirmedMotion() const { return unconfirmedMotion_; }

private:
    static const uint32_t kRawSpacingMs = 2;
    const QueueProgram* program_ = nullptr;
    QueueState state_ = QueueState::Idle;
    uint32_t runId_ = 0;
    uint32_t programHash_ = 0;
    uint32_t repeat_ = 1;
    uint32_t iteration_ = 0;
    uint16_t stepIndex_ = 0;
    Phase phase_ = Phase::Idle;
    uint32_t phaseAt_ = 0;
    uint32_t deadlineAt_ = 0;
    uint32_t lastRawAt_ = 0;
    uint16_t errorLine_ = 0;
    bool strictHome_ = false;
    bool motionComplete_ = false;
    bool unconfirmedMotion_ = false;
    char message_[64] = "idle";
};

} // namespace motion
