#pragma once

// The one software owner of a manually requested move or homing run. CAN I/O,
// node observations, stop confirmation and fault latching stay with MotorControl;
// this class judges only evidence for the operation that currently owns the slot.
#include <stdint.h>
#include "DeviceOperation.h"

namespace motion {

class ManualOperationSupervisor {
public:
    // Shared policy constants used by the manual evidence checks and the node
    // freshness/gating adapter. Keep their numeric values in one place.
    enum : uint32_t { kFeedbackFreshMs = 600, kAckTimeoutMs = 1500 };
    enum : int32_t { kStoppedSpeedTenths = 5 };
    enum class Kind : uint8_t { None, Move, Home };
    enum class MoveOutcome : uint8_t { None, Running, Done, Cancelled, Failed };
    enum class HomeOutcome : uint8_t { None, Running, Done, NoMotion, Cancelled, Failed };
    using Fault = DeviceOperationFault;
    using OperationResult = DeviceOperationResult;
    enum : uint8_t { kTerminalHistoryCapacity = 8 };
    explicit ManualOperationSupervisor(uint64_t initialCounter = 0)
        : nextOperationId_(initialCounter), sessionBaseId_(initialCounter) {}
    struct Verdict {
        Kind kind = Kind::None;
        Fault fault = Fault::None;
        bool done = false;
    };
    struct MoveStart {
        uint8_t id = 0;
        uint8_t opcode = 0;
        DeviceOperationKind operationKind = DeviceOperationKind::Move;
        int64_t startTenths = 0;
        int64_t targetTenths = 0;
        int32_t toleranceTenths = 0;
        uint32_t expectedDurationMs = 0;
        uint32_t startMs = 0;
        uint32_t deadlineMs = 0;
        bool targetProof = false;
    };
    struct Observation {
        uint32_t now = 0;
        bool enabled = false;
        bool positionFresh = false;
        bool velocityFresh = false;
        int32_t position = 0;
        int32_t velocity = 0;
        uint32_t positionMs = 0;
        uint32_t velocityMs = 0;
        bool targetValid = false;
        int32_t target = 0;
        uint32_t targetMs = 0;
        bool homeFlagsFresh = false;
        uint8_t homeFlags = 0;
        uint32_t homeFlagsMs = 0;
    };
    struct MoveFailure {
        uint8_t id = 0;
        int64_t target = 0;
        int32_t position = 0, velocity = 0;
        uint32_t elapsed = 0, deadline = 0;
        bool positionValid = false, velocityValid = false, enabled = false;
    };

    Kind kind() const { return kind_; }
    bool active() const { return kind_ != Kind::None; }
    bool moveActive() const { return kind_ == Kind::Move; }
    bool homeActive() const { return kind_ == Kind::Home; }
    uint8_t activeId() const { return moveActive() ? move_.id : (homeActive() ? homeId_ : 0); }
    uint8_t moveId() const { return move_.id; }
    uint8_t moveOpcode() const { return move_.opcode; }
    uint32_t moveStartMs() const { return move_.startMs; }
    bool moveNeedsTargetProof() const { return move_.targetProof; }
    uint8_t homeId() const { return homeId_; }
    uint8_t homeMode() const { return homeMode_; }
    uint32_t homeStartMs() const { return home_.startMs; }
    MoveOutcome moveOutcome() const { return moveOutcome_; }
    HomeOutcome homeOutcome() const { return homeOutcome_; }
    const MoveFailure& moveFailure() const { return moveFailure_; }

    // One active manual operation and a bounded terminal ring. Queries are
    // memory-only and never affect CAN or query scheduling.
    bool canStartOperation() const { return nextOperationId_ != UINT64_MAX; }
    uint64_t activeOperationId() const { return active() ? activeOperationId_ : 0; }
    OperationResult readOperation(uint64_t operationId) const {
        OperationResult result;
        result.operationId = operationId;
        if (operationId == 0) return result;
        if (operationId <= sessionBaseId_) {
            result.state = DeviceOperationState::Expired;
            return result;
        }
        if (operationId > nextOperationId_) return result;
        if (active() && operationId == activeOperationId_) {
            result.motorId = activeId();
            result.kind = activeOperationKind_;
            result.state = DeviceOperationState::Running;
            result.protocolAck = moveActive() ? move_.ackSeen : home_.ackSeen;
            result.receiveAckObserved = moveActive()
                ? move_.receiveAckObserved : home_.receiveAckObserved;
            result.driverReachedObserved = moveActive()
                ? move_.driverReachedObserved : home_.completedSeen;
            return result;
        }
        for (uint8_t i = 0; i < terminalCount_; ++i) {
            if (terminal_[i].operationId == operationId) return terminal_[i];
        }
        result.state = DeviceOperationState::Evicted;
        return result;
    }

    // A CAN reinitialization starts a new local session. Issued IDs are never
    // reused by this object; old-session IDs are explicitly expired.
    void beginSession() {
        clearOwnership();
        for (uint8_t i = 0; i < kTerminalHistoryCapacity; ++i) terminal_[i] = OperationResult{};
        terminalCount_ = terminalNext_ = 0;
        sessionBaseId_ = nextOperationId_;
    }

    // Queue ownership removes the old public manual outcome but records why
    // the active handle stopped being supervised.
    void supersede() {
        if (moveActive()) finishMove(MoveOutcome::None, Fault::None,
                                     DeviceOperationState::Superseded);
        else if (homeActive()) finishHome(HomeOutcome::None, Fault::None,
                                          DeviceOperationState::Superseded);
        clearOwnership();
    }

    // Compatibility reset is a queue takeover. beginSession() performs the
    // distinct lifecycle reset that clears the result history.
    void reset() { supersede(); }
private:
    void clearOwnership() {
        kind_ = Kind::None;
        move_ = Move{};
        home_ = Home{};
        moveOutcome_ = MoveOutcome::None;
        homeOutcome_ = HomeOutcome::None;
        homeId_ = homeMode_ = 0;
        activeOperationId_ = 0;
        activeOperationKind_ = DeviceOperationKind::None;
    }
public:
    bool startMove(const MoveStart& start) {
        if (active() || !canStartOperation()) return false;
        move_ = Move{};
        move_.id = start.id;
        move_.opcode = start.opcode;
        move_.startTenths = start.startTenths;
        move_.targetTenths = start.targetTenths;
        move_.toleranceTenths = start.toleranceTenths;
        move_.expectedDurationMs = start.expectedDurationMs;
        move_.startMs = start.startMs;
        move_.deadlineMs = start.deadlineMs;
        move_.targetProof = start.targetProof;
        move_.lastDonePosMs = start.startMs;
        move_.lastDoneVelMs = start.startMs;
        kind_ = Kind::Move;
        moveOutcome_ = MoveOutcome::Running;
        activeOperationId_ = ++nextOperationId_;
        activeOperationKind_ = start.operationKind;
        return true;
    }
    bool startHome(uint8_t id, uint8_t mode, uint32_t now, uint32_t deadlineMs) {
        if (active() || !canStartOperation()) return false;
        home_ = Home{};
        home_.startMs = now;
        home_.deadlineMs = deadlineMs;
        home_.lastDonePosMs = now;
        home_.lastDoneVelMs = now;
        homeId_ = id;
        homeMode_ = mode;
        kind_ = Kind::Home;
        homeOutcome_ = HomeOutcome::Running;
        activeOperationId_ = ++nextOperationId_;
        activeOperationKind_ = DeviceOperationKind::Home;
        return true;
    }
    bool ownsAck(uint8_t id, uint8_t opcode) const {
        return (moveActive() && move_.id == id && move_.opcode == opcode) ||
            (homeActive() && homeId_ == id && opcode == 0x9A);
    }
    void acceptedAck(uint8_t id, uint8_t opcode, bool completed, uint32_t now) {
        if (!ownsAck(id, opcode)) return;
        if (moveActive()) {
            move_.ackSeen = true;
            if (completed) move_.driverReachedObserved = true;
            else move_.receiveAckObserved = true;
        } else {
            home_.ackSeen = true;
            if (completed) {
                home_.completedSeen = true;
                home_.completedMs = now;
            } else home_.receiveAckObserved = true;
        }
    }
    void noMotion(uint8_t id) {
        if (homeActive() && homeId_ == id) finishHome(HomeOutcome::NoMotion);
    }
    void cancelMove(uint8_t id) {
        if (moveActive() && move_.id == id) finishMove(MoveOutcome::Cancelled);
    }
    void cancelHome(uint8_t id) {
        if (homeActive() && homeId_ == id) finishHome(HomeOutcome::Cancelled);
    }
    void failActiveWithoutSnapshot(Fault fault = Fault::None) {
        if (moveActive()) finishMove(MoveOutcome::Failed, fault);
        else if (homeActive()) finishHome(HomeOutcome::Failed, fault);
    }
    void failMove(const Observation& observation, Fault fault = Fault::None) {
        if (!moveActive()) return;
        moveFailure_.id = move_.id;
        moveFailure_.target = move_.targetTenths;
        moveFailure_.position = observation.position;
        moveFailure_.velocity = observation.velocity;
        moveFailure_.positionValid = observation.positionFresh;
        moveFailure_.velocityValid = observation.velocityFresh;
        moveFailure_.elapsed = observation.now - move_.startMs;
        moveFailure_.deadline = move_.deadlineMs;
        moveFailure_.enabled = observation.enabled;
        finishMove(MoveOutcome::Failed, fault);
    }
    void failHome(uint8_t id, Fault fault = Fault::None) {
        if (homeActive() && homeId_ == id) finishHome(HomeOutcome::Failed, fault);
    }
    void rawInvalidation(uint8_t id) {
        if (id == 0) {
            if (moveActive()) finishMove(MoveOutcome::None, Fault::None,
                                         DeviceOperationState::Invalidated);
            else if (homeActive()) finishHome(HomeOutcome::None, Fault::None,
                                              DeviceOperationState::Invalidated);
            clearOwnership();
        } else if (moveActive() && move_.id == id) {
            finishMove(MoveOutcome::None, Fault::None, DeviceOperationState::Invalidated);
        } else if (homeActive() && homeId_ == id) {
            finishHome(HomeOutcome::None, Fault::None, DeviceOperationState::Invalidated);
        }
    }

    Verdict poll(const Observation& o) {
        Verdict verdict;
        if (moveActive()) {
            verdict = pollMove(o);
            if (verdict.done) finishMove(MoveOutcome::Done);
            else if (verdict.fault != Fault::None) failMove(o, verdict.fault);
        } else if (homeActive()) {
            verdict = pollHome(o);
            if (verdict.done) finishHome(HomeOutcome::Done);
            else if (verdict.fault != Fault::None) finishHome(HomeOutcome::Failed, verdict.fault);
        }
        return verdict;
    }

private:
    enum : uint8_t {
        kRequiredMovePairs = 2, kRequiredHomeExplicitPairs = 1,
        kRequiredHomeInferredPairs = 2,
        kHomeRunning = 0x04, kHomeFailed = 0x08,
        kHomeOverTemp = 0x10, kHomeOverCurrent = 0x20
    };
    struct Move {
        uint8_t id = 0, opcode = 0;
        int64_t startTenths = 0, targetTenths = 0;
        int32_t toleranceTenths = 0;
        uint32_t expectedDurationMs = 0, startMs = 0, deadlineMs = 0;
        bool ackSeen = false, receiveAckObserved = false;
        bool driverReachedObserved = false, targetProof = false;
        uint8_t doneUpdates = 0;
        uint32_t lastDonePosMs = 0, lastDoneVelMs = 0;
    };
    struct Home {
        uint32_t startMs = 0, deadlineMs = 0;
        bool ackSeen = false, receiveAckObserved = false, runningSeen = false;
        bool stoppedSeen = false, completedSeen = false;
        uint32_t stoppedMs = 0, completedMs = 0;
        uint8_t doneUpdates = 0;
        uint32_t lastDonePosMs = 0, lastDoneVelMs = 0;
    };
    static bool within(uint32_t now, uint32_t stamp, uint32_t window) {
        return static_cast<uint32_t>(now - stamp) <= window;
    }
    static bool newer(uint32_t stamp, uint32_t since) {
        return static_cast<int32_t>(stamp - since) > 0;
    }
    static int64_t absolute(int64_t value) { return value < 0 ? -value : value; }
    void recordTerminal(DeviceOperationState state, Fault fault, bool protocolAck,
                        bool receiveAckObserved, bool driverReachedObserved,
                        bool reached) {
        OperationResult result;
        result.operationId = activeOperationId_;
        result.motorId = activeId();
        result.kind = activeOperationKind_;
        result.state = state;
        result.fault = fault;
        result.protocolAck = protocolAck;
        result.receiveAckObserved = receiveAckObserved;
        result.driverReachedObserved = driverReachedObserved;
        result.reached = reached;
        terminal_[terminalNext_] = result;
        terminalNext_ = static_cast<uint8_t>((terminalNext_ + 1) % kTerminalHistoryCapacity);
        if (terminalCount_ < kTerminalHistoryCapacity) ++terminalCount_;
    }
    void finishMove(MoveOutcome outcome, Fault fault = Fault::None,
                    DeviceOperationState overrideState = DeviceOperationState::Unknown) {
        DeviceOperationState state = overrideState;
        if (state == DeviceOperationState::Unknown) {
            switch (outcome) {
                case MoveOutcome::Done: state = DeviceOperationState::Reached; break;
                case MoveOutcome::Cancelled: state = DeviceOperationState::Cancelled; break;
                case MoveOutcome::Failed: state = DeviceOperationState::Failed; break;
                default: return;
            }
        }
        recordTerminal(state, fault, move_.ackSeen, move_.receiveAckObserved,
                       move_.driverReachedObserved, outcome == MoveOutcome::Done);
        kind_ = Kind::None;
        move_ = Move{};
        moveOutcome_ = outcome;
        activeOperationId_ = 0;
        activeOperationKind_ = DeviceOperationKind::None;
    }
    void finishHome(HomeOutcome outcome, Fault fault = Fault::None,
                    DeviceOperationState overrideState = DeviceOperationState::Unknown) {
        DeviceOperationState state = overrideState;
        if (state == DeviceOperationState::Unknown) {
            switch (outcome) {
                case HomeOutcome::Done: state = DeviceOperationState::Reached; break;
                case HomeOutcome::NoMotion: state = DeviceOperationState::NoMotion; break;
                case HomeOutcome::Cancelled: state = DeviceOperationState::Cancelled; break;
                case HomeOutcome::Failed: state = DeviceOperationState::Failed; break;
                default: return;
            }
        }
        recordTerminal(state, fault, home_.ackSeen, home_.receiveAckObserved,
                       home_.completedSeen, outcome == HomeOutcome::Done);
        kind_ = Kind::None;
        home_ = Home{};
        homeOutcome_ = outcome;
        activeOperationId_ = 0;
        activeOperationKind_ = DeviceOperationKind::None;
    }
    Verdict failure(Kind kind, Fault fault) const {
        Verdict verdict;
        verdict.kind = kind;
        verdict.fault = fault;
        return verdict;
    }
    Verdict completion(Kind kind) const {
        Verdict verdict;
        verdict.kind = kind;
        verdict.done = true;
        return verdict;
    }
    Verdict pollMove(const Observation& o) {
        if (!o.enabled) return failure(Kind::Move, Fault::DriverDisabled);
        if (!(o.positionFresh && o.velocityFresh) &&
            !within(o.now, move_.startMs, kFeedbackFreshMs))
            return failure(Kind::Move, Fault::FeedbackStale);
        if (!move_.ackSeen && !within(o.now, move_.startMs, kAckTimeoutMs))
            return failure(Kind::Move, Fault::MoveAckTimeout);
        const bool targetCounts = !move_.targetProof ||
            (o.targetValid && newer(o.targetMs, move_.startMs));
        if (move_.ackSeen && o.positionFresh && o.velocityFresh && targetCounts &&
            newer(o.positionMs, move_.startMs) && newer(o.velocityMs, move_.startMs) &&
            newer(o.positionMs, move_.lastDonePosMs) &&
            newer(o.velocityMs, move_.lastDoneVelMs)) {
            move_.lastDonePosMs = o.positionMs;
            move_.lastDoneVelMs = o.velocityMs;
            const bool targetMatches = !move_.targetProof ||
                absolute(static_cast<int64_t>(o.target) - move_.targetTenths) <= move_.toleranceTenths;
            const int64_t positionError = static_cast<int64_t>(o.position) - move_.targetTenths;
            if (absolute(positionError) <= move_.toleranceTenths &&
                absolute(o.velocity) <= kStoppedSpeedTenths && targetMatches) {
                if (move_.doneUpdates < 0xFF) ++move_.doneUpdates;
                if (move_.doneUpdates >= kRequiredMovePairs) return completion(Kind::Move);
            } else move_.doneUpdates = 0;
        }
        if (!within(o.now, move_.startMs, move_.deadlineMs))
            return failure(Kind::Move, Fault::MoveTimeout);
        return Verdict{};
    }
    Verdict pollHome(const Observation& o) {
        if (!(o.positionFresh && o.velocityFresh) &&
            !within(o.now, home_.startMs, kFeedbackFreshMs))
            return failure(Kind::Home, Fault::FeedbackStale);
        if (!home_.ackSeen && !within(o.now, home_.startMs, kAckTimeoutMs))
            return failure(Kind::Home, Fault::HomeAckTimeout);
        const bool postStartFlags = o.homeFlagsFresh && newer(o.homeFlagsMs, home_.startMs);
        if (postStartFlags) {
            if (o.homeFlags & kHomeFailed) return failure(Kind::Home, Fault::HomeFailed);
            if (o.homeFlags & (kHomeOverTemp | kHomeOverCurrent))
                return failure(Kind::Home, Fault::HomeProtection);
            if (o.homeFlags & kHomeRunning) {
                home_.runningSeen = true;
                home_.stoppedSeen = false;
                home_.doneUpdates = 0;
            } else if (home_.runningSeen && !home_.stoppedSeen) {
                home_.stoppedSeen = true;
                home_.stoppedMs = o.homeFlagsMs;
            }
        } else if (!home_.completedSeen && home_.ackSeen &&
                   !within(o.now, home_.startMs, kFeedbackFreshMs)) {
            return failure(Kind::Home, Fault::HomeStatusMissing);
        }
        const bool proofValid = home_.completedSeen ||
            (o.homeFlagsFresh && home_.stoppedSeen);
        const uint32_t proofMs = home_.completedSeen ? home_.completedMs : home_.stoppedMs;
        if (proofValid && o.positionFresh && o.velocityFresh &&
            newer(o.positionMs, proofMs) && newer(o.velocityMs, proofMs) &&
            newer(o.positionMs, home_.lastDonePosMs) &&
            newer(o.velocityMs, home_.lastDoneVelMs)) {
            home_.lastDonePosMs = o.positionMs;
            home_.lastDoneVelMs = o.velocityMs;
            if (absolute(o.velocity) <= kStoppedSpeedTenths) {
                if (home_.doneUpdates < 0xFF) ++home_.doneUpdates;
                const uint8_t required = home_.completedSeen
                    ? kRequiredHomeExplicitPairs : kRequiredHomeInferredPairs;
                if (home_.doneUpdates >= required) return completion(Kind::Home);
            } else home_.doneUpdates = 0;
        }
        if (!within(o.now, home_.startMs, home_.deadlineMs))
            return failure(Kind::Home, Fault::HomeTimeout);
        return Verdict{};
    }

    Kind kind_ = Kind::None;
    uint64_t nextOperationId_ = 0;
    uint64_t sessionBaseId_ = 0;
    uint64_t activeOperationId_ = 0;
    DeviceOperationKind activeOperationKind_ = DeviceOperationKind::None;
    OperationResult terminal_[kTerminalHistoryCapacity];
    uint8_t terminalNext_ = 0, terminalCount_ = 0;
    Move move_;
    Home home_;
    MoveOutcome moveOutcome_ = MoveOutcome::None;
    HomeOutcome homeOutcome_ = HomeOutcome::None;
    uint8_t homeId_ = 0, homeMode_ = 0;
    MoveFailure moveFailure_;
};

}  // namespace motion
