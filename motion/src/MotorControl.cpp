#include "MotorControl.h"

#include <string.h>

#include "x42s_can_id.h"
#include "ProtocolGate.h"

namespace motion {
namespace {

// Feedback is considered fresh for this long after the frame arrived.
constexpr uint32_t kFeedbackFreshMs = 600;
// How long an enable or a move acknowledgement may take before it is treated
// as missing. Neither timeout ever implies success.
constexpr uint32_t kEnableAckTimeoutMs = 1500;
constexpr uint32_t kMoveAckTimeoutMs = 1500;
// Added to the planned duration before a running move is declared overdue.
constexpr uint32_t kMoveDeadlineMarginMs = 3000;
// One query request is issued per poll cycle, at least this far apart. A query
// is only sent when at least this much time has actually elapsed.
constexpr uint32_t kQueryIntervalMs = 30;
// Bounded RX work per poll() call.
constexpr uint32_t kMaxFramesPerPoll = 16;
// "Approximately stopped" / "at target" tolerances.
constexpr int32_t kStopSpeedTenths = 5;        // 0.5 RPM
constexpr int32_t kTargetToleranceTenths = 5;  // 0.5 degree, capped per move
// A move only finishes after this many DISTINCT good feedback samples (two
// separate post-command position/velocity pairs).
constexpr uint8_t kRequiredDoneUpdates = 2;

constexpr int kDefaultTxPin = 4;
constexpr int kDefaultRxPin = 5;
constexpr long kDefaultBitrate = 500000;

// Latchable fault identifiers. Stable strings, translated by the frontend.
const char* const kFaultNone = "none";
const char* const kFaultBusOff = "bus_off";
const char* const kFaultFeedbackStale = "feedback_stale";
const char* const kFaultMoveTimeout = "move_timeout";
const char* const kFaultMoveAckTimeout = "move_ack_timeout";
const char* const kFaultAckRejected = "ack_rejected";
const char* const kFaultMoveTxFailed = "move_tx_failed";
const char* const kFaultDisableTxFailed = "disable_tx_failed";

bool ageWithin(uint32_t now, uint32_t stamp, uint32_t window) {
    return static_cast<uint32_t>(now - stamp) <= window;
}

// True only when `stamp` is strictly newer than `since` (wrap-safe signed
// difference), used wherever a post-command sample is required and a
// same-millisecond frame must not count as proof of the new state.
bool isStrictlyNewerThan(uint32_t stamp, uint32_t since) {
    return static_cast<int32_t>(stamp - since) > 0;
}

const char* busStateName(CanControllerState state) {
    switch (state) {
        case CanControllerState::Unavailable: return "unavailable";
        case CanControllerState::Stopped: return "stopped";
        case CanControllerState::Running: return "running";
        case CanControllerState::BusOff: return "bus_off";
        case CanControllerState::Recovering: return "recovering";
    }
    return "unavailable";
}

}  // namespace

MotorControl::MotorControl()
    : can_(kDefaultTxPin, kDefaultRxPin, kDefaultBitrate) {}

bool MotorControl::begin(int tx, int rx, long bitrate) {
    // Reset software tracking only: no motor is enabled, moved or stopped here.
    // The selected id survives a re-init so a configured node stays selected.
    job_ = MoveJob{};
    faultTag_ = kFaultNone;
    faultId_ = 0;
    faultGlobal_ = false;
    lastQueryMs_ = 0;
    querySlot_ = 0;
    queryFieldIndex_ = 0;
    for (uint16_t i = 0; i < kNodeCount; ++i) {
        nodes_[i] = NodeState{};
    }

    experimentId_ = 0;
    can_.setTraceSink(traceSink, this);
    can_.end();
    can_.configure(tx, rx, bitrate);
    canReady_ = can_.begin();
    refreshBusStatus();
    return canReady_;
}

void MotorControl::poll() {
    const uint32_t now = millis();
    refreshBusStatus();

    if (busState_ == CanControllerState::BusOff) {
        // No auto-resume: latch the fault, invalidate every pending/confirmed
        // enable and cancel the job. Nothing resumes by itself.
        if (strcmp(faultTag_, kFaultBusOff) != 0) {
            latchFault(0, kFaultBusOff, true);
        }
        const uint8_t activeId = job_.active ? job_.id : 0;
        job_ = MoveJob{};
        experimentId_ = 0;
        for (uint16_t id = 1; id < kNodeCount; ++id) {
            NodeState& node = nodes_[id];
            node.enablePending = false;
            node.enableConfirmed = false;
            node.enableDesired = false;
            if (id == activeId || id == selectedId_ || node.stopRequested) {
                node.stopRequested = true;
                node.stopRequestedMs = now;
            }
        }
        return;
    }

    drainRx(now);
    if (experimentId_) {
        int32_t pos = 0, vel = 0;
        const bool expired = static_cast<uint32_t>(now - experimentStart_) >= 5000;
        const bool stale = !freshPosition(experimentId_, now, pos) || !freshVelocity(experimentId_, now, vel);
        if (expired || stale) {
            const uint8_t id = experimentId_;
            stop(id);
            if (stale) latchFault(id, "feedback_stale", false);
        }
    }
    serviceEnableTimeouts(now);
    serviceJob(now);
    serviceStopConfirmations(now);
    serviceQueries(now);
}

void MotorControl::watch(uint8_t id) {
    if (id == 0) return;
    selectedId_ = id;
}

bool MotorControl::canReady() const {
    return canReady_ && busState_ == CanControllerState::Running;
}

void MotorControl::refreshBusStatus() {
    CanBusStatus status;
    if (!can_.getBusStatus(status)) {
        busState_ = CanControllerState::Unavailable;
        txErrorCounter_ = 0;
        return;
    }
    busState_ = status.state;
    txErrorCounter_ = status.txErrorCounter;
}

const char* MotorControl::busStateString() const { return busStateName(busState_); }

uint32_t MotorControl::txErrorCount() const { return txErrorCounter_; }

void MotorControl::drainRx(uint32_t now) {
    if (!canReady()) return;
    CanRawFrame frame;
    for (uint32_t i = 0; i < kMaxFramesPerPoll; ++i) {
        if (!can_.receive(frame, 0)) break;
        handleFrame(frame, now);
    }
}

void MotorControl::handleFrame(const CanRawFrame& frame, uint32_t now) {
    traceSink(this, frame, false);
    ++rxCount_;
    if (frame.length && frame.data[0] != kFramePosition && frame.data[0] != kFrameVelocity &&
        frame.data[0] != kFrameCurrent && frame.data[0] != kFrameFlags) {
        rxDiagnostics_[diagnosticNext_].frame = frame;
        rxDiagnostics_[diagnosticNext_].atMs = now;
        diagnosticNext_ = (diagnosticNext_ + 1) % 8;
        if (diagnosticCount_ < 8) ++diagnosticCount_;
    }
    // Only single packet extended data frames from a real motor address. This
    // rejects remote frames, multi packet traffic and extended ids whose high
    // bits are set (address bits are 8..15, packet index bits are 0..7).
    if (frame.length == 0 ||
        !x42sCanIsSinglePacketDataFrame(
            frame.identifier, frame.extended, frame.remote)) {
        return;
    }
    const uint8_t id = x42sCanAddress(frame.identifier);
    if (id == 0) return;
    if (!nodeOfInterest(id)) return;

    const uint8_t function = frame.data[0];
    if (function == kFrameEnable || function == kFrameMove ||
        function == kFrameStop) {
        if (frame.length != 3 || frame.data[2] != kProtocolChecksum) return;
        handleAck(id, function, frame.data[1], now);
        return;
    }

    FeedbackSample sample;
    if (!decodeFeedback(frame.data, frame.length, sample)) return;

    NodeState& node = nodes_[id];
    node.seenEver = true;
    node.lastSeenMs = now;
    switch (sample.field) {
        case FeedbackField::Position:
            node.positionValid = true;
            node.positionTenths = sample.value;
            node.positionMs = now;
            break;
        case FeedbackField::Velocity:
            node.velocityValid = true;
            node.velocityTenths = sample.value;
            node.velocityMs = now;
            break;
        case FeedbackField::Current:
            node.currentValid = true;
            node.currentMa = static_cast<uint16_t>(sample.value);
            node.currentMs = now;
            break;
        case FeedbackField::Flags:
            node.flagsValid = true;
            node.flags = static_cast<uint8_t>(sample.value);
            node.flagsMs = now;
            break;
        case FeedbackField::None:
        default:
            break;
    }
}

void MotorControl::handleAck(
    uint8_t id, uint8_t function, uint8_t status, uint32_t now) {
    NodeState& node = nodes_[id];
    const AckStatus ack = classifyAck(status);
    node.lastAck = ackStatusToString(ack);
    node.lastAckMs = now;

    if (ack == AckStatus::Received || ack == AckStatus::Completed) {
        if (function == kFrameEnable) {
            // An F3 ack may only confirm a run of enableControl. A late ack
            // arriving after a stop (enablePending cleared) must not re-enable.
            if (!node.enablePending) return;
            node.enablePending = false;
            node.enableConfirmed = node.enableDesired;
            if (!node.enableDesired) {
                node.stopRequested = true;
                node.stopRequestedMs = now;
            }
        } else if (function == kFrameMove) {
            if (job_.active && job_.id == id) job_.ackSeen = true;
        }
        // A stop acknowledgement still needs stationary feedback to clear.
        return;
    }

    // Parameter / format / unknown status for a command we issued is a fault.
    node.enableConfirmed = false;
    node.enableDesired = false;
    node.enablePending = false;
    node.stopRequested = true;
    node.stopRequestedMs = now;
    if (job_.active && job_.id == id) {
        sendStop(id);
        job_ = MoveJob{};
    }
    latchFault(id, kFaultAckRejected, false);
}

bool MotorControl::freshPosition(uint8_t id, uint32_t now, int32_t& out) const {
    const NodeState& node = nodes_[id];
    if (!node.positionValid || !ageWithin(now, node.positionMs, kFeedbackFreshMs)) {
        return false;
    }
    out = node.positionTenths;
    return true;
}

bool MotorControl::freshVelocity(uint8_t id, uint32_t now, int32_t& out) const {
    const NodeState& node = nodes_[id];
    if (!node.velocityValid || !ageWithin(now, node.velocityMs, kFeedbackFreshMs)) {
        return false;
    }
    out = node.velocityTenths;
    return true;
}

bool MotorControl::stationaryFeedback(
    uint8_t id, uint32_t now, uint32_t sinceMs) const {
    const NodeState& node = nodes_[id];
    if (!node.positionValid || !node.velocityValid) return false;
    if (!ageWithin(now, node.positionMs, kFeedbackFreshMs)) return false;
    if (!ageWithin(now, node.velocityMs, kFeedbackFreshMs)) return false;
    // A confirmation needs samples that strictly follow the request; a frame
    // that arrived in the same millisecond is not proof of the new state.
    if (sinceMs != 0 && !isStrictlyNewerThan(node.positionMs, sinceMs)) return false;
    if (sinceMs != 0 && !isStrictlyNewerThan(node.velocityMs, sinceMs)) return false;
    const int32_t speed = node.velocityTenths;
    return (speed < 0 ? -speed : speed) <= kStopSpeedTenths;
}

bool MotorControl::faultAppliesTo(uint8_t id) const {
    if (strcmp(faultTag_, kFaultNone) == 0) return false;
    return faultGlobal_ || faultId_ == id;
}

bool MotorControl::nodeOfInterest(uint8_t id) const {
    if (id == 0) return false;
    if (id == selectedId_ || id == experimentId_) return true;
    if (job_.active && job_.id == id) return true;
    const NodeState& node = nodes_[id];
    return node.enablePending || node.stopRequested || node.enableDesired;
}

bool MotorControl::anyStopPending() const {
    if (job_.active) return true;
    for (uint16_t id = 1; id < kNodeCount; ++id) {
        if (nodes_[id].stopRequested) return true;
    }
    return false;
}

void MotorControl::latchFault(uint8_t id, const char* tag, bool global) {
    faultTag_ = tag;
    faultId_ = id;
    faultGlobal_ = global;
}

void MotorControl::clearFault(uint8_t id) {
    if (!faultAppliesTo(id)) return;
    faultTag_ = kFaultNone;
    faultId_ = 0;
    faultGlobal_ = false;
}

void MotorControl::serviceEnableTimeouts(uint32_t now) {
    for (uint16_t id = 1; id < kNodeCount; ++id) {
        NodeState& node = nodes_[id];
        if (!node.enablePending) continue;
        if (ageWithin(now, node.enablePendingMs, kEnableAckTimeoutMs)) continue;
        // No acknowledgement in time: the requested state was never confirmed.
        node.enablePending = false;
        if (node.enableDesired) node.enableConfirmed = false;
        node.lastAck = "enable_timeout";
    }
}

void MotorControl::serviceJob(uint32_t now) {
    if (!job_.active) return;
    NodeState& node = nodes_[job_.id];

    int32_t position = 0;
    int32_t velocity = 0;
    const bool posFresh = freshPosition(job_.id, now, position);
    const bool velFresh = freshVelocity(job_.id, now, velocity);

    // Essential freshness needs BOTH position and velocity. Once either has
    // been silent past the grace window the job is a fault, not something to
    // ride out.
    if (!(posFresh && velFresh) &&
        !ageWithin(now, job_.startMs, kFeedbackFreshMs)) {
        failJob(kFaultFeedbackStale, now);
        return;
    }

    if (!job_.ackSeen && !ageWithin(now, job_.startMs, kMoveAckTimeoutMs)) {
        failJob(kFaultMoveAckTimeout, now);
        return;
    }

    // "Done" needs an accepted ack plus two DISTINCT post-command sample pairs
    // near the target with a near zero speed. The same sample evaluated twice
    // never counts; each counted pair must be strictly newer than the last.
    if (job_.ackSeen && posFresh && velFresh &&
        isStrictlyNewerThan(node.positionMs, job_.startMs) &&
        isStrictlyNewerThan(node.velocityMs, job_.startMs) &&
        (isStrictlyNewerThan(node.positionMs, job_.lastDonePosMs) &&
         isStrictlyNewerThan(node.velocityMs, job_.lastDoneVelMs))) {
        job_.lastDonePosMs = node.positionMs;
        job_.lastDoneVelMs = node.velocityMs;
        const int64_t error = static_cast<int64_t>(position) - job_.targetTenths;
        const int64_t absError = error < 0 ? -error : error;
        const int32_t absSpeed = velocity < 0 ? -velocity : velocity;
        if (absError <= job_.toleranceTenths && absSpeed <= kStopSpeedTenths) {
            if (job_.doneUpdates < 0xFF) job_.doneUpdates++;
            if (job_.doneUpdates >= kRequiredDoneUpdates) {
                node.stopRequested = false;
                job_ = MoveJob{};
                return;
            }
        } else {
            job_.doneUpdates = 0;
        }
    }

    if (!ageWithin(now, job_.startMs, job_.deadlineMs)) {
        failJob(kFaultMoveTimeout, now);
    }
}

void MotorControl::serviceStopConfirmations(uint32_t now) {
    for (uint16_t id = 1; id < kNodeCount; ++id) {
        NodeState& node = nodes_[id];
        if (!node.stopRequested) continue;
        if (job_.active && job_.id == id) continue;
        // A stop only clears on a NEW stationary position/velocity sample, so a
        // stop or disable on a fresh target still has to be seen on the wire.
        if (stationaryFeedback(static_cast<uint8_t>(id), now, node.stopRequestedMs)) {
            node.stopRequested = false;
        }
    }
}

void MotorControl::serviceQueries(uint32_t now) {
    if (!canReady()) return;
    // Only send when the interval has actually elapsed: the comparison is a
    // minimum gap, not a window the poll must land inside.
    if (static_cast<uint32_t>(now - lastQueryMs_) < kQueryIntervalMs) return;
    lastQueryMs_ = now;

    // The query set is rebuilt every time from the selected id, the active job
    // and any node with a pending enable or stop. Nothing accumulates, so
    // switching ids can never exhaust a fixed slot table.
    uint8_t targets[kMaxQueryTargets];
    uint8_t count = 0;
    const auto addTarget = [&targets, &count](uint8_t id) {
        if (id == 0 || count >= kMaxQueryTargets) return;
        for (uint8_t i = 0; i < count; ++i) {
            if (targets[i] == id) return;
        }
        targets[count++] = id;
    };
    addTarget(experimentId_);
    addTarget(selectedId_);
    if (job_.active) addTarget(job_.id);
    for (uint16_t id = 1; id < kNodeCount && count < kMaxQueryTargets; ++id) {
        if (nodes_[id].enablePending || nodes_[id].stopRequested) {
            addTarget(static_cast<uint8_t>(id));
        }
    }
    if (count == 0) return;

    if (querySlot_ >= count) querySlot_ = 0;
    const uint8_t target = targets[querySlot_];
    const X42sSysParam field = queryFieldIndex_ == 0
        ? X42sSysParam::Cpos
        : (queryFieldIndex_ == 1 ? X42sSysParam::Vel :
           (queryFieldIndex_ == 2 ? X42sSysParam::Cpha : X42sSysParam::Flag));

    // Single shot probe for every target: a node that is offline (or went
    // quiet) must not spam retries or block the rotation.
    can_.clearTransmissionError();
    can_.probeReadSysParams(target, field);
    can_.clearTransmissionError();

    // Advance the target within a field, and only step the field once every
    // target got its turn: with <=4 targets each node gets position, velocity
    // and current well inside the freshness window.
    if (++querySlot_ >= count) {
        querySlot_ = 0;
        queryFieldIndex_ = static_cast<uint8_t>((queryFieldIndex_ + 1) % 4);
    }
}

void MotorControl::failJob(const char* tag, uint32_t now) {
    const uint8_t id = job_.id;
    NodeState& node = nodes_[id];
    job_ = MoveJob{};
    node.stopRequested = true;
    node.stopRequestedMs = now;
    // Best effort single stop; success is never assumed.
    sendStop(id);
    // A fault invalidates the enable confirmation: explicit re-enable needed.
    node.enableConfirmed = false;
    node.enableDesired = false;
    node.enablePending = false;
    latchFault(id, tag, false);
}

bool MotorControl::sendStop(uint8_t id) {
    if (!canReady()) return false;
    can_.clearTransmissionError();
    can_.stopNow(id, false);
    if (can_.hasTransmissionError()) {
        can_.clearTransmissionError();
        return false;
    }
    return true;
}

Result MotorControl::enable(uint8_t id, bool state) {
    const uint32_t now = millis();
    if (id == 0) return Result{kCodeInvalid, "id_reserved"};
    refreshBusStatus();
    if (!canReady()) {
        return Result{
            kCodeUnavailable,
            busState_ == CanControllerState::BusOff ? "bus_off" : "can_unavailable"};
    }
    // Acks are only routed while a node is of interest, so select it first.
    watch(id);
    NodeState& node = nodes_[id];

    if (!state) {
        if (experimentId_ == id) experimentId_ = 0;
        const bool wasActive = job_.active && job_.id == id;
        can_.clearTransmissionError();
        can_.enableControl(id, false, false);
        if (can_.hasTransmissionError()) {
            // The disable never reached the wire. Do not silently drop the job:
            // stop best effort, latch and invalidate the enable instead.
            can_.clearTransmissionError();
            node.stopRequested = true;
            node.stopRequestedMs = now;
            sendStop(id);
            if (wasActive) job_ = MoveJob{};
            node.enablePending = false;
            node.enableDesired = false;
            node.enableConfirmed = false;
            latchFault(id, kFaultDisableTxFailed, false);
            return Result{kCodeUnavailable, "can_tx_failed"};
        }
        // Disable accepted: only now is software tracking dropped.
        if (wasActive) job_ = MoveJob{};
        node.enableDesired = false;
        node.enableConfirmed = false;
        node.enablePending = true;
        node.enablePendingMs = now;
        node.stopRequested = true;
        node.stopRequestedMs = now;
        return Result{kCodeQueued, "queued"};
    }

    // Enable true is rejected while any job is live or any stop is still
    // waiting for confirmation.
    if (experimentId_) return Result{kCodeBusy, "experiment_active"};
    if (job_.active) {
        return Result{kCodeBusy, job_.id == id ? "busy" : "another_motor_active"};
    }
    if (anyStopPending()) return Result{kCodeBusy, "stop_pending"};
    if (node.enablePending) return Result{kCodeBusy, "enable_pending"};

    // Explicit enable is the recovery path out of a latched fault.
    if (faultAppliesTo(id)) {
        if (!stationaryFeedback(id, now, 0)) {
            return Result{kCodeBusy, "fault_latched"};
        }
        clearFault(id);
    }

    if (node.enableConfirmed && node.enableDesired) {
        return Result{kCodeQueued, "already_enabled"};
    }

    can_.clearTransmissionError();
    can_.enableControl(id, true, false);
    if (can_.hasTransmissionError()) {
        // A failed transmission can never mean enabled.
        can_.clearTransmissionError();
        node.enableConfirmed = false;
        node.enablePending = false;
        return Result{kCodeUnavailable, "can_tx_failed"};
    }

    watch(id);
    node.enableDesired = true;
    node.enableConfirmed = false;
    node.enablePending = true;
    node.enablePendingMs = now;
    return Result{kCodeQueued, "queued"};
}

Result MotorControl::move(const MoveRequest& request) {
    const uint32_t now = millis();
    if (request.id == 0) return Result{kCodeInvalid, "id_reserved"};

    MovePlan plan;
    const char* error = nullptr;
    if (!buildMovePlan(request, plan, &error)) {
        return Result{kCodeInvalid, error != nullptr ? error : "invalid_request"};
    }

    refreshBusStatus();
    if (!canReady()) {
        return Result{
            kCodeUnavailable,
            busState_ == CanControllerState::BusOff ? "bus_off" : "can_unavailable"};
    }

    const uint8_t id = request.id;
    // Feedback is only tracked for the selected node, so select before the
    // freshness gates: otherwise a first request could never be satisfied.
    watch(id);
    NodeState& node = nodes_[id];
    if (faultAppliesTo(id)) return Result{kCodeBusy, faultTag_};
    if (experimentId_) return Result{kCodeBusy, "experiment_active"};
    if (job_.active) {
        return Result{kCodeBusy, job_.id == id ? "busy" : "another_motor_active"};
    }
    if (node.stopRequested) return Result{kCodeBusy, "stop_pending"};
    if (node.enablePending) return Result{kCodeBusy, "enable_pending"};
    if (!node.enableConfirmed) return Result{kCodeBusy, "not_enabled"};

    int32_t position = 0;
    int32_t velocity = 0;
    if (!freshPosition(id, now, position) || !freshVelocity(id, now, velocity)) {
        return Result{kCodeUnavailable, "feedback_unavailable"};
    }
    const int32_t absSpeed = velocity < 0 ? -velocity : velocity;
    if (absSpeed > kStopSpeedTenths) return Result{kCodeBusy, "not_stopped"};

    can_.clearTransmissionError();
    can_.positionControlWithCurrentLimit(
        id,
        plan.direction,
        plan.speedTenths,
        plan.accelWire,
        plan.decelWire,
        plan.magnitudeTenths,
        plan.motionMode,
        plan.sync,
        plan.currentMa);
    if (can_.hasTransmissionError()) {
        // A partial transmission may have reached the motor. Stop it best
        // effort and latch instead of returning a bare 503 with a live motor.
        can_.clearTransmissionError();
        sendStop(id);
        node.stopRequested = true;
        node.stopRequestedMs = now;
        node.enableConfirmed = false;
        node.enableDesired = false;
        node.enablePending = false;
        latchFault(id, kFaultMoveTxFailed, false);
        return Result{kCodeUnavailable, "can_tx_failed"};
    }

    job_ = MoveJob{};
    job_.active = true;
    job_.id = id;
    job_.startTenths = position;
    // int64 sum: the int32 position plus the delta cannot overflow.
    job_.targetTenths = static_cast<int64_t>(position) + plan.deltaTenths;
    // Tolerance is never allowed to reach the full travel: a tiny (0.1 degree)
    // move must not be declared done while the motor still sits at the start.
    const uint32_t halfMagnitude = plan.magnitudeTenths / 2;
    job_.toleranceTenths = static_cast<int32_t>(
        halfMagnitude < static_cast<uint32_t>(kTargetToleranceTenths)
            ? halfMagnitude
            : static_cast<uint32_t>(kTargetToleranceTenths));
    job_.expectedDurationMs = plan.expectedDurationMs;
    job_.startMs = now;
    job_.deadlineMs = plan.expectedDurationMs + kMoveDeadlineMarginMs;
    job_.ackSeen = false;
    job_.doneUpdates = 0;
    job_.lastDonePosMs = now;
    job_.lastDoneVelMs = now;
    return Result{kCodeQueued, "queued"};
}

Result MotorControl::stop(uint8_t id) {
    if (experimentId_ == id) experimentId_ = 0;
    const uint32_t now = millis();
    if (id == 0) return Result{kCodeInvalid, "id_reserved"};

    // Software tracking is cleared unconditionally so a stop still takes effect
    // while the bus is down; only the wire stop is gated below.
    if (job_.active && job_.id == id) job_ = MoveJob{};
    // Selecting keeps the node in the query rotation so the stop can be seen.
    watch(id);
    NodeState& node = nodes_[id];
    node.stopRequested = true;
    node.stopRequestedMs = now;
    // A stop cancels any enable in flight so a late F3 ack cannot re-enable.
    node.enablePending = false;
    node.enableDesired = false;
    node.enableConfirmed = false;

    // The state stays "stop_requested" until a NEW stationary feedback sample
    // confirms it (or a latched fault persists).
    refreshBusStatus();
    if (!canReady()) {
        return Result{
            kCodeUnavailable,
            busState_ == CanControllerState::BusOff ? "bus_off" : "can_unavailable"};
    }
    if (!sendStop(id)) return Result{kCodeUnavailable, "can_tx_failed"};
    return Result{kCodeQueued, "queued"};
}

Result MotorControl::stopAll() {
    experimentId_ = 0;
    const uint32_t now = millis();

    // Cancel the job and forget every enable first: this holds even when the
    // broadcast cannot be sent.
    job_ = MoveJob{};
    for (uint16_t id = 1; id < kNodeCount; ++id) {
        NodeState& node = nodes_[id];
        const bool touched = id == selectedId_ || node.stopRequested ||
            node.enablePending || node.enableDesired || node.enableConfirmed;
        if (!touched) continue;
        node.stopRequested = true;
        node.stopRequestedMs = now;
        node.enablePending = false;
        node.enableDesired = false;
        node.enableConfirmed = false;
    }

    refreshBusStatus();
    if (!canReady()) {
        return Result{
            kCodeUnavailable,
            busState_ == CanControllerState::BusOff ? "bus_off" : "can_unavailable"};
    }

    // id 0 is the dedicated broadcast for stopAll.
    can_.clearTransmissionError();
    can_.stopNow(0, false);
    if (can_.hasTransmissionError()) {
        can_.clearTransmissionError();
        return Result{kCodeUnavailable, "can_tx_failed"};
    }
    return Result{kCodeQueued, "queued"};
}

const char* MotorControl::stateString(uint8_t id) const {
    if (experimentId_ == id) return "experiment_running";
    const NodeState& node = nodes_[id];
    if (faultAppliesTo(id)) return "fault";
    if (node.enablePending) return "enable_pending";
    if (node.stopRequested) return "stop_requested";
    if (job_.active && job_.id == id) return "moving";
    if (node.enableConfirmed) return "idle";
    return "disabled";
}

String MotorControl::statusJson(uint8_t id) const {
    const uint32_t now = millis();
    const NodeState& node = nodes_[id];
    const bool posFresh =
        node.positionValid && ageWithin(now, node.positionMs, kFeedbackFreshMs);
    const bool velFresh =
        node.velocityValid && ageWithin(now, node.velocityMs, kFeedbackFreshMs);
    const bool curFresh =
        node.currentValid && ageWithin(now, node.currentMs, kFeedbackFreshMs);

    String json;
    json.reserve(320);
    json += "{\"id\":";
    json += static_cast<unsigned int>(id);
    json += ",\"canReady\":";
    json += canReady() ? "true" : "false";
    json += ",\"busState\":\"";
    json += busStateString();
    json += "\",\"txErrors\":";
    json += static_cast<unsigned long>(txErrorCount());
    json += ",\"activeId\":";
    json += static_cast<unsigned int>(job_.active ? job_.id : experimentId_);
    json += ",\"state\":\"";
    json += stateString(id);
    json += "\",\"fault\":\"";
    json += faultAppliesTo(id) ? faultTag_ : "none";
    json += "\",\"enabled\":";
    json += node.enableConfirmed ? "true" : "false";
    json += ",\"online\":";
    json += (posFresh && velFresh) ? "true" : "false";
    json += ",\"positionDeg\":";
    if (posFresh) {
        json += String(static_cast<float>(node.positionTenths) / 10.0f, 1);
    } else {
        json += "null";
    }
    json += ",\"speedRpm\":";
    if (velFresh) {
        json += String(static_cast<float>(node.velocityTenths) / 10.0f, 1);
    } else {
        json += "null";
    }
    json += ",\"currentMa\":";
    if (curFresh) {
        json += static_cast<unsigned int>(node.currentMa);
    } else {
        json += "null";
    }
    json += ",\"ageMs\":";
    if (node.seenEver) {
        json += static_cast<unsigned long>(now - node.lastSeenMs);
    } else {
        json += "null";
    }
    json += ",\"driverFlags\":";
    const bool flagsFresh = node.flagsValid && ageWithin(now, node.flagsMs, kFeedbackFreshMs);
    if (flagsFresh) json += static_cast<unsigned int>(node.flags); else json += "null";
    json += ",\"driverEnabled\":";
    json += flagsFresh ? ((node.flags & 1) ? "true" : "false") : "null";
    json += ",\"lastAck\":\"";
    json += node.lastAck;
    json += "\"}";
    return json;
}


bool MotorControl::hasActiveMotion() const {
    if (job_.active || experimentId_) return true;
    for (uint16_t id = 1; id < kNodeCount; ++id) {
        const NodeState& n = nodes_[id];
        if (n.enablePending || n.stopRequested || n.enableConfirmed || n.enableDesired) return true;
    }
    return false;
}

Result MotorControl::command(const uint8_t* b, uint8_t n) {
    const CommandKind kind = validateCommand(b, n);
    if (kind == CommandKind::Invalid) return Result{400, "unsupported_or_invalid_command"};
    const uint8_t id = b[0];
    if (kind == CommandKind::Enable) return enable(id, b[3] != 0);
    if (kind == CommandKind::Stop) return stop(id);
    if (kind == CommandKind::Move) {
        MoveRequest request;
        request.id = id;
        request.angleDeg = (b[2] ? -1.0f : 1.0f) * (dword(b+9) / 10.0f);
        request.speedRpm = word(b+7) / 10.0f;
        request.accelRpmS = word(b+3);
        request.decelRpmS = word(b+5);
        request.currentMa = word(b+15);
        return move(request);
    }
    refreshBusStatus();
    if (!canReady()) return Result{503, "can_unavailable"};
    watch(id);
    NodeState& node = nodes_[id];
    if (kind == CommandKind::Configure) {
        if (hasActiveMotion() || !stationaryFeedback(id, millis(), 0) ||
            !node.flagsValid || !ageWithin(millis(), node.flagsMs, kFeedbackFreshMs) || (node.flags & 1))
            return Result{409, "disable_and_wait_for_stationary_feedback"};
    }
    if (kind == CommandKind::Experiment) {
        if (job_.active || experimentId_ || anyStopPending() || node.enablePending ||
            faultAppliesTo(id) || !node.enableConfirmed || !stationaryFeedback(id, millis(), 0))
            return Result{409, "enable_and_wait_for_stationary_feedback"};
    }
    can_.clearTransmissionError();
    const bool queued = can_.sendValidatedCommand(b, n);
    if (kind == CommandKind::Interrupt) { stop(id); }
    if (!queued) {
        if (kind == CommandKind::Experiment || kind == CommandKind::Configure) {
            stop(id);
            latchFault(id, "command_tx_failed", false);
        }
        return Result{503, "can_tx_failed"};
    }
    if (kind == CommandKind::Experiment) {
        experimentId_ = id;
        experimentStart_ = millis();
    }
    if (kind == CommandKind::Configure) {
        node.positionValid = node.velocityValid = node.flagsValid = false;
        node.enableConfirmed = false;
    }
    return Result{202, kind == CommandKind::Experiment ? "queued_auto_stop_5s" : "queued"};
}

void MotorControl::traceSink(void* context, const CanRawFrame& frame, bool tx) {
    MotorControl* self = static_cast<MotorControl*>(context);
    TraceEntry& entry = self->trace_[self->traceNext_];
    entry.frame = frame;
    entry.tx = tx;
    entry.atMs = millis();
    entry.sequence = ++self->traceSequence_;
    self->traceNext_ = (self->traceNext_ + 1) % 48;
    if (self->traceCount_ < 48) ++self->traceCount_;
}

String MotorControl::traceJson() const {
    String json("{\"uptimeMs\":"); json += static_cast<unsigned long>(millis());
    json += ",\"sequence\":"; json += static_cast<unsigned long>(traceSequence_);
    json += ",\"frames\":[";
    for (uint8_t i = 0; i < traceCount_; ++i) {
        if (i) json += ',';
        const TraceEntry& e = trace_[(traceNext_ + 48 - traceCount_ + i) % 48];
        json += "{\"seq\":"; json += static_cast<unsigned long>(e.sequence);
        json += ",\"atMs\":"; json += static_cast<unsigned long>(e.atMs);
        json += ",\"dir\":\""; json += e.tx ? "TX" : "RX";
        json += "\",\"id\":"; json += static_cast<unsigned long>(e.frame.identifier);
        json += ",\"extended\":"; json += e.frame.extended ? "true" : "false";
        json += ",\"remote\":"; json += e.frame.remote ? "true" : "false";
        json += ",\"data\":[";
        for (uint8_t j = 0; j < e.frame.length && j < 8; ++j) {
            if (j) json += ',';
            json += static_cast<unsigned int>(e.frame.data[j]);
        }
        json += "]}";
    }
    json += "]}";
    return json;
}

String MotorControl::canDebugJson() const {
    String json("{\"rxTotal\":"); json += static_cast<unsigned long>(rxCount_);
    json += ",\"commandFrames\":[";
    for (uint8_t i = 0; i < diagnosticCount_; ++i) {
        if (i) json += ',';
        const RxDiagnostic& d = rxDiagnostics_[(diagnosticNext_ + 8 - diagnosticCount_ + i) % 8];
        json += "{\"id\":"; json += static_cast<unsigned long>(d.frame.identifier);
        json += ",\"extended\":"; json += d.frame.extended ? "true" : "false";
        json += ",\"remote\":"; json += d.frame.remote ? "true" : "false";
        json += ",\"atMs\":"; json += static_cast<unsigned long>(d.atMs);
        json += ",\"data\":[";
        for (uint8_t b = 0; b < d.frame.length && b < 8; ++b) {
            if (b) json += ',';
            json += static_cast<unsigned int>(d.frame.data[b]);
        }
        json += "]}";
    }
    json += "]}"; return json;
}

bool MotorControl::anyMotorOnline() const {
    const uint32_t now = millis();
    for (uint16_t id = 1; id < kNodeCount; ++id) {
        const NodeState& node = nodes_[id];
        if (!node.positionValid || !node.velocityValid) continue;
        if (!ageWithin(now, node.positionMs, kFeedbackFreshMs)) continue;
        if (!ageWithin(now, node.velocityMs, kFeedbackFreshMs)) continue;
        return true;
    }
    return false;
}

}  // namespace motion
