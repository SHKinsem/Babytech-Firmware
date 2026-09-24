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
// Bounded RX work per poll() call.
constexpr uint32_t kMaxFramesPerPoll = 16;
// "Approximately stopped" / "at target" tolerances.
constexpr int32_t kStopSpeedTenths = 5;        // 0.5 RPM
constexpr int32_t kTargetToleranceTenths = 5;  // 0.5 degree, capped per move
// A zero-travel direct command has no travel to halve, so it is judged with the
// single feedback count the driver can actually resolve (0.1 degree).
constexpr int32_t kNoOpToleranceTenths = 1;
// A move only finishes after this many DISTINCT good feedback samples (two
// separate post-command position/velocity pairs).
constexpr uint8_t kRequiredDoneUpdates = 2;
// Bounded on-demand refresh of the driver's target sample (0x33): the poll stops
// after this long, within the single query scheduler's budget.
constexpr uint32_t kTargetPollWindowMs = 1500;

// Homing frames and status bits (manual V1.0.5 pp61-63). 0x9A triggers homing,
// 0x9C interrupts it and 0x3B is the homing status byte.
constexpr uint8_t kFrameHome = 0x9A;
constexpr uint8_t kFrameHomeInterrupt = 0x9C;
constexpr uint8_t kFrameHomeStatus = 0x3B;
constexpr uint8_t kHomeStatusRunning = 0x04;      // Org_SF: homing in progress
constexpr uint8_t kHomeStatusFailed = 0x08;       // Org_CF: homing failed
constexpr uint8_t kHomeStatusOverTemp = 0x10;     // Otp_TF
constexpr uint8_t kHomeStatusOverCurrent = 0x20;  // Ocp_TF
// Manual p40: "triggering homing while already at the origin, or with the left /
// right limit already triggered: the motor does not move". 12/22 are a finished
// attempt without motion, never a completion and never a fault.
constexpr uint8_t kStatusHomeAlreadyAtOriginA = 0x12;
constexpr uint8_t kStatusHomeAlreadyAtOriginB = 0x22;
// Clearing bit2 is weaker evidence than an explicit 9A/9F completion, so the
// inferred path needs the same two distinct post-start sample pairs a move
// needs, while an explicit device completion is enough with one.
constexpr uint8_t kRequiredHomeInferredUpdates = kRequiredDoneUpdates;
constexpr uint8_t kRequiredHomeExplicitUpdates = 1;

constexpr int kDefaultTxPin = 4;
constexpr int kDefaultRxPin = 5;
constexpr long kDefaultBitrate = 500000;

// A raw CAN data frame carries at most 8 bytes (classic CAN).
constexpr uint8_t kMaxCanDataBytes = 8;

// Latchable fault identifiers. Stable strings, translated by the frontend.
const char* const kFaultNone = "none";
const char* const kFaultBusOff = "bus_off";
const char* const kFaultFeedbackStale = "feedback_stale";
const char* const kFaultMoveTimeout = "move_timeout";
const char* const kFaultMoveAckTimeout = "move_ack_timeout";
const char* const kFaultAckRejected = "ack_rejected";
const char* const kFaultMoveTxFailed = "move_tx_failed";
const char* const kFaultDisableTxFailed = "disable_tx_failed";
// Homing faults. Distinct tags so a failed homing run is never reported as a
// failed move (and vice versa).
const char* const kFaultHomeAckTimeout = "home_ack_timeout";
const char* const kFaultHomeTimeout = "home_timeout";
const char* const kFaultHomeFailed = "home_failed";
const char* const kFaultHomeProtection = "home_protection";
const char* const kFaultHomeTxFailed = "home_tx_failed";
const char* const kFaultHomeStatusMissing = "home_status_missing";

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
    : bus_(kDefaultTxPin, kDefaultRxPin, kDefaultBitrate) {}

bool MotorControl::begin(int tx, int rx, long bitrate) {
    // Reset software tracking only: no motor is enabled, moved or stopped here.
    // The selected id survives a re-init so a configured node stays selected.
    job_ = MoveJob{};
    home_ = HomeJob{};
    homeOutcome_ = HomeOutcome::None;
    homeId_ = 0;
    homeMode_ = 0;
    faultTag_ = kFaultNone;
    faultId_ = 0;
    faultGlobal_ = false;
    targetPollId_ = 0;
    targetPollArmedMs_ = 0;
    for (uint16_t i = 0; i < kNodeCount; ++i) {
        nodes_[i] = NodeState{};
    }

    experimentId_ = 0;
    bus_.setTraceSink(traceSink, this);
    bus_.end();
    bus_.configure(tx, rx, bitrate);
    canReady_ = bus_.begin();
    refreshBusStatus();
    return canReady_;
}

void MotorControl::poll(bool dispatchAutomaticQueries) {
    const uint32_t now = millis();
    queueDiagnostics_.poll(now);
    refreshBusStatus();

    if (busState_ == CanControllerState::BusOff) {
        // No auto-resume: latch the fault, invalidate every pending/confirmed
        // enable and cancel the jobs. Nothing resumes by itself.
        if (strcmp(faultTag_, kFaultBusOff) != 0) {
            latchFault(0, kFaultBusOff, true);
        }
        const uint8_t activeId = job_.active ? job_.id : 0;
        if (job_.active) moveOutcome_ = MoveOutcome::Failed;
        job_ = MoveJob{};
        if (home_.active) homeOutcome_ = HomeOutcome::Failed;
        home_ = HomeJob{};
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
    pollConfig(now);
    if (experimentId_) {
        int32_t pos = 0, vel = 0;
        const bool expired = limits_.experimentDurationMs != 0 &&
            static_cast<uint32_t>(now - experimentStart_) >= limits_.experimentDurationMs;
        const bool stale = !freshPosition(experimentId_, now, pos) || !freshVelocity(experimentId_, now, vel);
        if (expired || stale) {
            const uint8_t id = experimentId_;
            stop(id);
            if (stale) latchFault(id, "feedback_stale", false);
        }
    }
    serviceEnableTimeouts(now);
    serviceJob(now);
    serviceHome(now);
    serviceStopConfirmations(now);
    serviceQueries(now);
    if (dispatchAutomaticQueries) dispatchQueries();
}

void MotorControl::watch(uint8_t id) {
    if (id == 0) return;
    selectedId_ = id;
    if (!autoQueriesEnabled_) return;
    const uint8_t fields[]={0x36,0x35,0x3A};
    for (uint8_t field:fields)
        queries_.demand(id,field,CanQueryScheduler::Page,600,2000,0,millis());
}

void MotorControl::setAutoQueriesEnabled(bool enabled) {
    autoQueriesEnabled_ = enabled;
    if (!enabled) queries_.release(CanQueryScheduler::Page);
    else if (selectedId_) watch(selectedId_);
}

bool MotorControl::canReady() const {
    return canReady_ && busState_ == CanControllerState::Running;
}

void MotorControl::refreshBusStatus() {
    CanBusStatus status;
    if (!bus_.getBusStatus(status)) {
        busState_ = CanControllerState::Unavailable;
        txErrorCounter_ = 0;
        return;
    }
    busState_ = status.state;
    txErrorCounter_ = status.txErrorCounter;
    txFailedCount_ = status.txFailedCount;
    rxMissedCount_ = status.rxMissedCount;
    rxOverrunCount_ = status.rxOverrunCount;
}

const char* MotorControl::busStateString() const { return busStateName(busState_); }

uint32_t MotorControl::txErrorCount() const { return txErrorCounter_; }

void MotorControl::drainRx(uint32_t now) {
    if (!canReady()) return;
    CanRawFrame frame;
    for (uint32_t i = 0; i < kMaxFramesPerPoll; ++i) {
        if (!bus_.receive(frame, 0)) break;
        handleFrame(frame, now);
    }
}

void MotorControl::handleFrame(const CanRawFrame& frame, uint32_t now) {
    traceSink(this, frame, false);
    ++rxCount_;
    if (configFrame(frame, now)) return;
    if (frame.length && frame.data[0] != kFramePosition && frame.data[0] != kFrameTarget &&
        frame.data[0] != kFrameVelocity &&
        frame.data[0] != kFrameCurrent && frame.data[0] != kFrameFlags &&
        frame.data[0] != kFrameHomeStatus) {
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
    if(syncObserve_[id] && frame.length==3 && frame.data[0]==0xCD && frame.data[2]==kProtocolChecksum) {
        auto& node=nodes_[id];++node.syncAckSequence;node.syncAckMs=now;
        if(node.syncAck!=0xE2 && node.syncAck!=0xEE &&
           (frame.data[1]!=0x9F || node.syncAck!=2)) node.syncAck=frame.data[1];
    }
    if (frame.length == 3 && frame.data[2] == kProtocolChecksum &&
        QueueDiagnostics::functionBit(frame.data[0])) {
        queueDiagnostics_.response(id, frame.data[0], frame.data[1], now);
    }
    if (!nodeOfInterest(id)) return;

    const uint8_t function = frame.data[0];
    // Control replies. 0xFB/0xCB replies are the documented FB/CB answers
    // (02/12/22/9F/E2/EE, manual p40) and 0xFB/0xCB with 9F is also the
    // documented position-reached report (manual p36). Neither is a completion
    // claim on its own.
    if (function == kFrameEnable || function == kFrameMove ||
        function == kFrameDirect || function == kFrameDirectLimit ||
        function == kFrameStop || function == kFrameHome || function == 0x50) {
        if (frame.length != 3 || frame.data[2] != kProtocolChecksum) return;
        handleAck(id, function, frame.data[1], now);
        return;
    }

    NodeState& node = nodes_[id];

    // 0x3B homing status: its own bit table, kept out of the 0x3A motor flags.
    if (function == kFrameHomeStatus) {
        if (frame.length != 3 || frame.data[2] != kProtocolChecksum) return;
        queries_.receive(id,function,now);
        node.seenEver = true;
        node.lastSeenMs = now;
        node.homeFlagsValid = true;
        node.homeFlags = frame.data[1];
        node.homeFlagsMs = now;
        if (id == queueObserveId_ && node.queueExpectedFunction == kFrameHome) {
            if (node.homeFlags & 0x38) node.queueHomeFailed = true;
            if (node.homeFlags & 4) {
                node.queueHomeRunning = true;
                node.queueHomeComplete = false;
            } else if (node.queueHomeRunning && !node.queueHomeComplete) {
                node.queueHomeComplete = true;
                node.queueHomeProofMs = now;
            }
        }
        return;
    }

    FeedbackSample sample;
    if (!decodeFeedback(frame.data, frame.length, sample)) return;
    queries_.receive(id,function,now);

    node.seenEver = true;
    node.lastSeenMs = now;
    switch (sample.field) {
        case FeedbackField::Position:
            node.positionValid = true;
            node.positionTenths = sample.value;
            node.positionMs = now;
            break;
        case FeedbackField::Target:
            node.targetValid = true;
            node.targetTenths = sample.value;
            node.targetMs = now;
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
            // A real disabled flag invalidates an old software confirmation.
            if (!(node.flags & 1)) node.enableConfirmed = false;
            if (node.enablePending && node.enableAck &&
                isStrictlyNewerThan(now, node.enablePendingMs) &&
                bool(node.flags & 1) == node.enableDesired) {
                node.enableConfirmed = node.enableDesired;
                node.enablePending = false;
            }
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
    if (status == 0xE2 || status == 0xEE) node.demoRejected = true;
    if (id == queueObserveId_ && function == node.queueExpectedFunction) {
        // Unrelated late ACKs must not replace this action's evidence. Preserve
        // rejection until consumed even if another ACK arrives in the RX batch.
        if (!node.queueAckPending ||
            (node.queueAckStatus != 0xE2 && node.queueAckStatus != 0xEE)) {
            node.queueAckFunction = function;
            node.queueAckStatus = status;
            node.queueAckMs = now;
            node.queueAckPending = true;
        }
        if (function == kFrameHome && (status == 0x9F || status == 0x12 || status == 0x22)) {
            if (!node.queueHomeComplete) node.queueHomeProofMs = now;
            node.queueHomeComplete = true;
        }
    }

    // 0x12/0x22 are the documented "homing was triggered while already at the
    // origin or with a limit already triggered, the motor does not move"
    // answers (manual p40). For a 9A trigger that is a finished attempt without
    // motion: not a completion claim, and not a fault. For any other function
    // the byte keeps its existing meaning (an unexpected status = fault).
    if (function == kFrameHome &&
        (status == kStatusHomeAlreadyAtOriginA || status == kStatusHomeAlreadyAtOriginB)) {
        node.lastAck = "home_no_motion";
        node.lastAckMs = now;
        if (home_.active && homeId_ == id) endHome(HomeOutcome::NoMotion);
        return;
    }

    node.lastAck = ackStatusToString(ack);
    node.lastAckMs = now;

    if (ack == AckStatus::Received || ack == AckStatus::Completed) {
        if (function == kFrameEnable) {
            // An F3 ack may only confirm a tracked enable request. A late ack
            // arriving after a stop (enablePending cleared) must not re-enable.
            if (!node.enablePending) return;
            node.enableAck = true; node.enableTimedOut = false;
            // F3 02 means accepted, not actual enable. Require a post-request
            // 3A flag too; it may have arrived before this acknowledgement.
            if (node.flagsValid && isStrictlyNewerThan(node.flagsMs, node.enablePendingMs) &&
                ageWithin(now, node.flagsMs, kFeedbackFreshMs) &&
                bool(node.flags & 1) == node.enableDesired) {
                node.enableConfirmed = node.enableDesired;
                node.enablePending = false;
            }
            if (!node.enableDesired) {
                node.stopRequested = true;
                node.stopRequestedMs = now;
            }
        } else if (function == kFrameMove || function == kFrameDirect ||
                   function == kFrameDirectLimit) {
            // Only the opcode the job was started with may confirm it. A 02 of
            // another command (or an unsolicited reached-report of another
            // opcode) never sets ackSeen for this job.
            if (job_.active && job_.id == id && job_.opcode == function) {
                job_.ackSeen = true;
            }
        } else if (function == kFrameHome && home_.active && homeId_ == id) {
            // 0x02 only says the trigger was received. 0x9F is the documented
            // active completion reply (manual p40) and is the one ack that may
            // stand in for the 3B running-bit proof, still only together with
            // fresh post-start stationary feedback.
            home_.ackSeen = true;
            if (ack == AckStatus::Completed) home_.completedMs = now;
        }
        // A stop acknowledgement still needs stationary feedback to clear.
        return;
    }

    // An unsolicited/late rejection is diagnostic evidence, not ownership of
    // a current action. In particular, replies left in the RX queue after an
    // explicit state clear must not immediately recreate a phantom stop/fault.
    const bool owned = (function == kFrameEnable && node.enablePending) ||
        (function == kFrameStop && node.stopRequested) ||
        (function == kFrameHome && home_.active && homeId_ == id) ||
        (job_.active && job_.id == id && job_.opcode == function);
    if (!owned) return;

    // Parameter / format / unknown status for the command we own is a fault.
    node.enableConfirmed = false;
    node.enableDesired = false;
    node.enablePending = false;
    node.stopRequested = true;
    node.stopRequestedMs = now;
    if (job_.active && job_.id == id) {
        sendStop(id);
        moveOutcome_ = MoveOutcome::Failed;
        job_ = MoveJob{};
    }
    if (home_.active && homeId_ == id) {
        // A rejected trigger/abort answer fails the run: the 9C+FE abort is
        // best effort and the fault invalidates the enable. failHome latches
        // ack_rejected itself.
        failHome(kFaultAckRejected, now);
        return;
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

bool MotorControl::freshTarget(uint8_t id, uint32_t now, int32_t& out) const {
    const NodeState& node = nodes_[id];
    if (!node.targetValid || !ageWithin(now, node.targetMs, kFeedbackFreshMs)) {
        return false;
    }
    out = node.targetTenths;
    return true;
}

void MotorControl::invalidateTarget(uint8_t id) {
    for (uint16_t nodeId = 1; nodeId < kNodeCount; ++nodeId) {
        if (id != 0 && nodeId != id) continue;
        NodeState& node = nodes_[nodeId];
        node.targetValid = false;
        node.targetMs = 0;
    }
    // A refresh that was armed for the previous bus state is obsolete too.
    if (id == 0 || targetPollId_ == id) {
        targetPollId_ = 0;
    }
}

void MotorControl::armTargetPoll(uint8_t id, uint32_t now) {
    targetPollId_ = id;
    targetPollArmedMs_ = now;
    queries_.demand(id,0x33,CanQueryScheduler::Controller,500,
                    kTargetPollWindowMs,2,now);
}

bool MotorControl::freshHomeFlags(uint8_t id, uint32_t now, uint8_t& out) const {
    const NodeState& node = nodes_[id];
    if (!node.homeFlagsValid || !ageWithin(now, node.homeFlagsMs, kFeedbackFreshMs)) {
        return false;
    }
    out = node.homeFlags;
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
    if (demoWatched_[id]) return true;
    if (id == selectedId_ || id == experimentId_ || id == queueObserveId_ || syncObserve_[id]) return true;
    const uint8_t fields[]={0x36,0x35,0x3A,0x33,0x3B};
    for (uint8_t field : fields)
        if (queries_.evidence(id,field).pending) return true;
    if (job_.active && job_.id == id) return true;
    if (home_.active && homeId_ == id) return true;
    const NodeState& node = nodes_[id];
    return node.enablePending || node.stopRequested || node.enableDesired;
}

bool MotorControl::anyStopPending() const {
    for (uint16_t id = 1; id < kNodeCount; ++id) {
        if (nodes_[id].stopRequested) return true;
    }
    return false;
}

void MotorControl::latchFault(uint8_t id, const char* tag, bool global) {
    faultTag_ = tag;
    faultId_ = id;
    faultGlobal_ = global;
    // A latched fault invalidates the enable confirmation and cancels any
    // enable still in flight, so a late F3 ack cannot re-enable a faulted node:
    // only a fresh explicit enable (after the fault clears) may do that.
    for (uint16_t nodeId = 1; nodeId < kNodeCount; ++nodeId) {
        if (!global && nodeId != id) continue;
        NodeState& node = nodes_[nodeId];
        node.enablePending = false;
        node.enableConfirmed = false;
        syncEnableDesired(static_cast<uint8_t>(nodeId));
    }
}

void MotorControl::syncEnableDesired(uint8_t id) {
    NodeState& node = nodes_[id];
    if (!node.enablePending) node.enableDesired = node.enableConfirmed;
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
        node.enableAck = false; node.enableTimedOut = true;
    }
}

void MotorControl::serviceJob(uint32_t now) {
    if (!job_.active) return;
    NodeState& node = nodes_[job_.id];
    if (!node.enableConfirmed) { failJob("driver_disabled", now); return; }

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

    // A direct (FB/CB) job is additionally proven by the driver's OWN target
    // sample: the target it reports must be the one this job resolved, and the
    // sample must follow this command; it is retained until target invalidation.
    // Position and velocity still need two distinct fresh pairs. A 02 ack alone
    // is never completion, and neither is a single stationary pair.
    int32_t reportedTarget = node.targetTenths;
    const bool targetCounts = !job_.targetProof ||
        (node.targetValid && isStrictlyNewerThan(node.targetMs, job_.startMs));

    // "Done" needs an accepted ack plus two DISTINCT post-command sample pairs
    // near the target with a near zero speed. The same sample evaluated twice
    // never counts; each counted pair must be strictly newer than the last.
    if (job_.ackSeen && posFresh && velFresh && targetCounts &&
        isStrictlyNewerThan(node.positionMs, job_.startMs) &&
        isStrictlyNewerThan(node.velocityMs, job_.startMs) &&
        (isStrictlyNewerThan(node.positionMs, job_.lastDonePosMs) &&
         isStrictlyNewerThan(node.velocityMs, job_.lastDoneVelMs))) {
        job_.lastDonePosMs = node.positionMs;
        job_.lastDoneVelMs = node.velocityMs;
        const int64_t error = static_cast<int64_t>(position) - job_.targetTenths;
        const int64_t absError = error < 0 ? -error : error;
        const int64_t targetError =
            static_cast<int64_t>(reportedTarget) - job_.targetTenths;
        const int64_t absTargetError = targetError < 0 ? -targetError : targetError;
        const bool targetMatches =
            !job_.targetProof || absTargetError <= job_.toleranceTenths;
        const int32_t absSpeed = velocity < 0 ? -velocity : velocity;
        if (absError <= job_.toleranceTenths && absSpeed <= kStopSpeedTenths &&
            targetMatches) {
            if (job_.doneUpdates < 0xFF) job_.doneUpdates++;
            if (job_.doneUpdates >= kRequiredDoneUpdates) {
                moveOutcome_ = MoveOutcome::Done;
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

void MotorControl::serviceHome(uint32_t now) {
    if (!home_.active) return;
    const uint8_t id = homeId_;
    NodeState& node = nodes_[id];

    int32_t position = 0;
    int32_t velocity = 0;
    const bool posFresh = freshPosition(id, now, position);
    const bool velFresh = freshVelocity(id, now, velocity);
    // A homing run has no requested target angle, so only the freshness of the
    // position matters here; the value itself is never compared to anything.
    (void)position;

    // Same grace rule as a move: once position and velocity have both been
    // silent past the window, the supervised run is a fault, not something to
    // ride out.
    if (!(posFresh && velFresh) && !ageWithin(now, home_.startMs, kFeedbackFreshMs)) {
        failHome(kFaultFeedbackStale, now);
        return;
    }
    if (!home_.ackSeen && !ageWithin(now, home_.startMs, kMoveAckTimeoutMs)) {
        failHome(kFaultHomeAckTimeout, now);
        return;
    }

    // 0x3B homing status. Only samples strictly after the trigger count, and
    // bit2 must have been OBSERVED set before its clearing can mean anything:
    // the power-on default is also "not homing" (00), so a lone 00 says nothing
    // about this run.
    uint8_t flags = 0;
    const bool flagsFresh = freshHomeFlags(id, now, flags);
    const bool postStartFlags =
        flagsFresh && isStrictlyNewerThan(node.homeFlagsMs, home_.startMs);
    if (postStartFlags) {
        if (flags & kHomeStatusFailed) { failHome(kFaultHomeFailed, now); return; }
        if (flags & (kHomeStatusOverTemp | kHomeStatusOverCurrent)) {
            failHome(kFaultHomeProtection, now);
            return;
        }
        if (flags & kHomeStatusRunning) {
            // Homing is running right now: any earlier "stopped" observation is
            // withdrawn. The inferred proof is a state transition, never a latch.
            home_.runningSeen = true;
            home_.stoppedMs = 0;
            home_.doneUpdates = 0;
        } else if (home_.runningSeen && home_.stoppedMs == 0) {
            // First post-running "not homing, no failure" observation. Keeping
            // the FIRST timestamp (not the newest refresh of the same state) is
            // what lets two distinct stationary samples accumulate afterwards.
            home_.stoppedMs = node.homeFlagsMs;
        }
    } else if (home_.completedMs == 0 && home_.ackSeen &&
               !ageWithin(now, home_.startMs, kFeedbackFreshMs)) {
        // An acknowledged run with no homing status evidence cannot be judged:
        // fail instead of riding the deadline out. The explicit 9A/9F completion
        // is the only path that does not need 0x3B at all.
        failHome(kFaultHomeStatusMissing, now);
        return;
    }

    // The inferred proof is only current while the status byte is fresh, so a
    // stale "not homing" reading can never complete a later run.
    const uint32_t proofMs = home_.completedMs != 0
        ? home_.completedMs
        : (flagsFresh ? home_.stoppedMs : 0);

    // Completion needs the run proof — an explicit documented 9A/9F completion,
    // or the running bit observed set and then cleared — AND fresh stationary
    // feedback that is strictly NEWER than the proof. The inferred path needs two
    // distinct sample pairs (like a move); an explicit device completion is
    // enough with one.
    if (proofMs != 0 && posFresh && velFresh &&
        isStrictlyNewerThan(node.positionMs, proofMs) &&
        isStrictlyNewerThan(node.velocityMs, proofMs) &&
        isStrictlyNewerThan(node.positionMs, home_.lastDonePosMs) &&
        isStrictlyNewerThan(node.velocityMs, home_.lastDoneVelMs)) {
        home_.lastDonePosMs = node.positionMs;
        home_.lastDoneVelMs = node.velocityMs;
        const int32_t absSpeed = velocity < 0 ? -velocity : velocity;
        if (absSpeed <= kStopSpeedTenths) {
            if (home_.doneUpdates < 0xFF) home_.doneUpdates++;
            const uint8_t required = home_.completedMs != 0
                ? kRequiredHomeExplicitUpdates
                : kRequiredHomeInferredUpdates;
            if (home_.doneUpdates >= required) {
                endHome(HomeOutcome::Done);
                node.stopRequested = false;
                // Homing can redefine the origin, so the pre-homing position is
                // not reused as if it were the new one: the next query
                // refreshes position and velocity.
                node.positionValid = false;
                node.velocityValid = false;
                return;
            }
        } else {
            home_.doneUpdates = 0;
        }
    }

    // The overall budget is the configured move duration: no extra hardcoded
    // homing timeout exists.
    if (!ageWithin(now, home_.startMs, home_.deadlineMs)) {
        failHome(kFaultHomeTimeout, now);
    }
}

void MotorControl::serviceStopConfirmations(uint32_t now) {    for (uint16_t id = 1; id < kNodeCount; ++id) {
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
    queries_.release(CanQueryScheduler::Controller);
    const auto demand = [this,now](uint8_t id,uint8_t field,uint32_t period,uint8_t priority) {
        queries_.demand(id,field,CanQueryScheduler::Controller,period,1000,priority,now);
    };
    for (uint16_t id=1; id<kNodeCount; ++id) {
        const bool moving=(job_.active && job_.id==id) ||
            (home_.active && homeId_==id) || experimentId_==id;
        const auto& node=nodes_[id];
        if (moving || node.stopRequested) {
            demand(id,0x36,200,2); demand(id,0x35,200,2);
        }
        if (moving || node.enablePending) demand(id,0x3A,500,1);
    }
    if (home_.active) demand(homeId_,0x3B,300,2);
    if (job_.active && job_.targetProof &&
        (!nodes_[job_.id].targetValid || !isStrictlyNewerThan(nodes_[job_.id].targetMs,job_.startMs)))
        demand(job_.id,0x33,500,2);
    if (targetPollId_ && ageWithin(now,targetPollArmedMs_,kTargetPollWindowMs)) {
        int32_t sample=0;
        if (!freshTarget(targetPollId_,now,sample)) demand(targetPollId_,0x33,500,1);
        else targetPollId_=0;
    }
    if (config_.state==2 && !config_.readIssued) demand(config_.id,0x22,3000,3);
}

bool MotorControl::sendQuery(void* context,uint8_t id,uint8_t field) {
    auto& self=*static_cast<MotorControl*>(context);
    const uint8_t bytes[]={id,field,0x6B};
    self.bus_.clearTransmissionError();
    bool sent=false;
    switch (field) {
        case 0x36: sent=self.bus_.probeReadSysParams(id,X42sSysParam::Cpos); break;
        case 0x35: sent=self.bus_.probeReadSysParams(id,X42sSysParam::Vel); break;
        case 0x33: sent=self.bus_.probeReadSysParams(id,X42sSysParam::Tpos); break;
        case 0x3A: sent=self.bus_.probeReadSysParams(id,X42sSysParam::Flag); break;
        case 0x3B: sent=self.bus_.probeReadSysParams(id,X42sSysParam::Org); break;
        case 0x27: sent=self.bus_.probeReadSysParams(id,X42sSysParam::Cpha); break;
        case 0x22: sent=self.bus_.sendRawLogical(bytes,sizeof(bytes)); break;
        default: break;
    }
    self.bus_.clearTransmissionError();
    if (field==0x22 && self.config_.state==2) {
        self.config_.readIssued=true;self.config_.readAt=millis();
        if (!sent) self.config_.state=8;
        self.queries_.release(id,field,CanQueryScheduler::Controller);
    }
    return sent;
}

void MotorControl::dispatchQueries() {
    queries_.poll(millis());
    if (canReady()) queries_.dispatch(millis(),sendQuery,this);
}

String MotorControl::queryStatusJson() const {
    const auto& c=queries_.config();const auto& s=queries_.statistics();
    String j("{\"queriesPerSecond\":");j+=c.queriesPerSecond;
    j+=",\"gapMs\":";j+=c.gapMs;j+=",\"timeoutMs\":";j+=c.timeoutMs;
    j+=",\"cooldownMs\":";j+=c.cooldownMs;j+=",\"maxInflight\":";j+=c.maxInflight;
    j+=",\"inflight\":";j+=queries_.inflight();
    j+=",\"queries\":";j+=static_cast<unsigned long>(s.queries);
    j+=",\"responses\":";j+=static_cast<unsigned long>(s.responses);
    j+=",\"unanswered\":";j+=static_cast<unsigned long>(s.unanswered);
    j+=",\"sendErrors\":";j+=static_cast<unsigned long>(s.sendErrors);
    j+=",\"latencySumMs\":";j+=static_cast<unsigned long>(s.latencySumMs);
    j+=",\"latencyMaxMs\":";j+=static_cast<unsigned long>(s.latencyMaxMs);
    j+=",\"txFrames\":";j+=static_cast<unsigned long>(txFrameCount_);
    j+=",\"rxFrames\":";j+=static_cast<unsigned long>(rxCount_);
    j+=",\"txFailed\":";j+=static_cast<unsigned long>(txFailedCount_);
    j+=",\"rxMissed\":";j+=static_cast<unsigned long>(rxMissedCount_);
    j+=",\"rxOverrun\":";j+=static_cast<unsigned long>(rxOverrunCount_);
    j+=",\"driverDrops\":null,\"unknownTraffic\":null,\"benchValidated\":false}";
    return j;
}

void MotorControl::failJob(const char* tag, uint32_t now) {
    moveOutcome_ = MoveOutcome::Failed;
    const uint8_t id = job_.id;
    NodeState& node = nodes_[id];
    moveFailure_.id = id;
    moveFailure_.target = job_.targetTenths;
    moveFailure_.positionValid = freshPosition(id, now, moveFailure_.position);
    moveFailure_.velocityValid = freshVelocity(id, now, moveFailure_.velocity);
    moveFailure_.elapsed = now - job_.startMs;
    moveFailure_.deadline = job_.deadlineMs;
    moveFailure_.enabled = node.enableConfirmed;
    job_ = MoveJob{};
    node.stopRequested = true;
    node.stopRequestedMs = now;
    // Best effort single stop; success is never assumed.
    sendStop(id);
    // A halted motor has no confirmed travel left: the driver's target sample
    // describes the state before the stop.
    invalidateTarget(id);
    // A fault invalidates the enable confirmation: explicit re-enable needed.
    node.enableConfirmed = false;
    node.enableDesired = false;
    node.enablePending = false;
    latchFault(id, tag, false);
}

const char* MotorControl::homeOutcomeName(HomeOutcome outcome) {
    switch (outcome) {
        case HomeOutcome::None: return "none";
        case HomeOutcome::Running: return "running";
        case HomeOutcome::Done: return "done";
        case HomeOutcome::NoMotion: return "no_motion";
        case HomeOutcome::Cancelled: return "cancelled";
        case HomeOutcome::Failed: return "failed";
    }
    return "none";
}

void MotorControl::endHome(HomeOutcome outcome) {
    // homeId_/homeMode_ are deliberately kept: a finished outcome stays
    // attributable to the run that produced it.
    home_.active = false;
    homeOutcome_ = outcome;
}

void MotorControl::cancelHome(bool interruptWire, bool stopWire) {
    if (!home_.active) return;
    const uint8_t id = homeId_;
    endHome(HomeOutcome::Cancelled);
    // 9C aborts the homing run itself, FE halts the motor. Both are best effort
    // and a cancel is never reported as a completion.
    if (interruptWire) sendHomeInterrupt(id);
    if (stopWire) sendStop(id);
}

void MotorControl::failHome(const char* tag, uint32_t now) {
    if (!home_.active) return;
    const uint8_t id = homeId_;
    endHome(HomeOutcome::Failed);
    sendHomeInterrupt(id);
    sendStop(id);
    invalidateTarget(id);
    NodeState& node = nodes_[id];
    node.stopRequested = true;
    node.stopRequestedMs = now;
    // A failed homing run invalidates the enable confirmation too: the operator
    // must re-enable explicitly before anything else moves.
    latchFault(id, tag, false);
}

bool MotorControl::sendHomeTrigger(uint8_t id, uint8_t mode) {
    if (!canReady()) return false;
    // [addr][0x9A][mode][sync=0][0x6B] (manual V1.0.5 p61-62). Immediate
    // execution only: the cached form needs the FF trigger, which this board
    // does not supervise.
    const uint8_t frame[5] = {id, kFrameHome, mode, 0, kProtocolChecksum};
    bus_.clearTransmissionError();
    const bool queued = bus_.sendValidatedCommand(frame, sizeof(frame));
    if (!queued || bus_.hasTransmissionError()) {
        bus_.clearTransmissionError();
        return false;
    }
    return true;
}

bool MotorControl::sendHomeInterrupt(uint8_t id) {
    if (!canReady()) return false;
    // [addr][0x9C][0x48][0x6B] (manual V1.0.5 p62): forced abort and exit.
    const uint8_t frame[4] = {id, kFrameHomeInterrupt, 0x48, kProtocolChecksum};
    bus_.clearTransmissionError();
    const bool queued = bus_.sendValidatedCommand(frame, sizeof(frame));
    if (!queued || bus_.hasTransmissionError()) {
        bus_.clearTransmissionError();
        return false;
    }
    return true;
}

bool MotorControl::sendStop(uint8_t id) {
    if (!canReady()) return false;
    bus_.clearTransmissionError();
    bus_.stopNow(id, false);
    if (bus_.hasTransmissionError()) {
        bus_.clearTransmissionError();
        return false;
    }
    return true;
}

Result MotorControl::enable(uint8_t id, bool state) {
    const uint32_t now = millis();
    if (id == 0) return Result{kCodeInvalid, "id_reserved"};
    if (state && configPending()) return Result{kCodeBusy, "config_pending"};
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
        // An explicit disable also ends a homing run: 9C aborts it, and the F3
        // disable below drops the enable on the wire.
        if (home_.active && homeId_ == id) cancelHome(true, false);
        bus_.clearTransmissionError();
        bus_.enableControl(id, false, false);
        if (bus_.hasTransmissionError()) {
            // The disable never reached the wire. Do not silently drop the job:
            // stop best effort, latch and invalidate the enable instead.
            bus_.clearTransmissionError();
            node.stopRequested = true;
            node.stopRequestedMs = now;
            sendStop(id);
            if (wasActive) { moveOutcome_ = MoveOutcome::Cancelled; job_ = MoveJob{}; }
            node.enablePending = false;
            node.enableDesired = false;
            node.enableConfirmed = false;
            latchFault(id, kFaultDisableTxFailed, false);
            return Result{kCodeUnavailable, "can_tx_failed"};
        }
        // Disable accepted: only now is software tracking dropped.
        if (wasActive) { moveOutcome_ = MoveOutcome::Cancelled; job_ = MoveJob{}; }
        node.enableDesired = false;
        node.enableConfirmed = false;
        node.enablePending = true;
        node.enableAck = false; node.enableTimedOut = false;
        node.enablePendingMs = now;
        node.stopRequested = true;
        node.stopRequestedMs = now;
        // A disabled driver (the shaft is released) keeps no meaningful target.
        invalidateTarget(id);
        return Result{kCodeQueued, "queued"};
    }

    // Enable true is rejected while any supervised action is live or any stop
    // is still waiting for confirmation.
    if (experimentId_) return Result{kCodeBusy, "experiment_active"};
    if (job_.active) {
        return Result{kCodeBusy, job_.id == id ? "busy" : "another_motor_active"};
    }
    if (home_.active) {
        return Result{kCodeBusy, homeId_ == id ? "home_active" : "another_motor_active"};
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

    bus_.clearTransmissionError();
    bus_.enableControl(id, true, false);
    if (bus_.hasTransmissionError()) {
        // A failed transmission can never mean enabled.
        bus_.clearTransmissionError();
        node.enableConfirmed = false;
        node.enablePending = false;
        return Result{kCodeUnavailable, "can_tx_failed"};
    }

    watch(id);
    node.enableDesired = true;
    node.enableConfirmed = false;
    node.enablePending = true;
        node.enableAck = false; node.enableTimedOut = false;
    node.enablePendingMs = now;
    return Result{kCodeQueued, "queued"};
}

Result MotorControl::move(const MoveRequest& request) {
    if (configPending()) return Result{kCodeBusy, "config_pending"};
    const uint32_t now = millis();
    if (request.id == 0) return Result{kCodeInvalid, "id_reserved"};

    MovePlan plan;
    const char* error = nullptr;
    if (!buildMovePlan(request, plan, &error, limits_)) {
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
    // A homing run owns the supervised slot just like a move does.
    if (home_.active) {
        return Result{kCodeBusy, homeId_ == id ? "home_active" : "another_motor_active"};
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

    bus_.clearTransmissionError();
    bus_.positionControlWithCurrentLimit(
        id,
        plan.direction,
        plan.speedTenths,
        plan.accelWire,
        plan.decelWire,
        plan.magnitudeTenths,
        plan.motionMode,
        plan.sync,
        plan.currentMa);
    if (bus_.hasTransmissionError()) {
        // A partial transmission may have reached the motor. Stop it best
        // effort and latch instead of returning a bare 503 with a live motor.
        bus_.clearTransmissionError();
        sendStop(id);
        node.stopRequested = true;
        node.stopRequestedMs = now;
        node.enableConfirmed = false;
        node.enableDesired = false;
        node.enablePending = false;
        latchFault(id, kFaultMoveTxFailed, false);
        return Result{kCodeUnavailable, "can_tx_failed"};
    }

    // The command on the wire changes what the driver considers its target.
    invalidateTarget(id);

    job_ = MoveJob{};
    job_.active = true;
    moveOutcome_ = MoveOutcome::Running;
    job_.id = id;
    job_.opcode = kFrameMove;
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
    job_.deadlineMs = limits_.maxMoveDurationMs;
    job_.ackSeen = false;
    job_.doneUpdates = 0;
    job_.lastDonePosMs = now;
    job_.lastDoneVelMs = now;
    return Result{kCodeQueued, "queued"};
}

Result MotorControl::directPosition(const DirectPositionRequest& request) {
    if (configPending()) return Result{kCodeBusy, "config_pending"};
    const uint32_t now = millis();
    if (request.id == 0) return Result{kCodeInvalid, "id_reserved"};

    DirectPositionPlan plan;
    const char* error = nullptr;
    if (!buildDirectPositionPlan(request, plan, &error, limits_)) {
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
    if (home_.active) {
        return Result{kCodeBusy, homeId_ == id ? "home_active" : "another_motor_active"};
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

    // Mode 0 is relative to the target the DRIVER currently holds, so it needs a
    // fresh 0x33 read of that target (manual V1.0.5 p70, the target the last
    // position command asked for). The actual position is a different fact, the
    // last target this board sent is only our own memory of it, and 0x34 (p71)
    // is the real-time setpoint that can be mid-trajectory: none of them may be
    // substituted. Without a fresh sample the request is refused and a bounded
    // refresh is started, so a retry can succeed instead of a target being
    // guessed.
    int32_t priorTarget = 0;
    const bool needPriorTarget =
        plan.motionMode == kMotionModeRelativeToPriorTarget;
    if (needPriorTarget && !freshTarget(id, now, priorTarget)) {
        armTargetPoll(id, now);
        return Result{kCodeUnavailable, "target_not_fresh"};
    }

    DirectPositionResolution resolved;
    if (!resolveDirectTarget(
            plan, position, priorTarget, limits_, resolved, &error)) {
        return Result{kCodeInvalid, error != nullptr ? error : "invalid_request"};
    }

    bus_.clearTransmissionError();
    if (plan.withCurrentLimit) {
        bus_.passthroughPositionControlWithCurrentLimit(
            id,
            plan.direction,
            plan.speedTenths,
            plan.angleTenths,
            plan.motionMode,
            false,
            plan.currentMa);
    } else {
        // FB carries no current field at all: it cannot impose a per-command
        // current limit, and none is invented for it.
        bus_.passthroughPositionControl(
            id,
            plan.direction,
            plan.speedTenths,
            plan.angleTenths,
            plan.motionMode,
            false);
    }
    if (bus_.hasTransmissionError()) {
        // A partial transmission may have reached the motor. Stop it best
        // effort and latch instead of returning a bare 503 with a live motor.
        bus_.clearTransmissionError();
        sendStop(id);
        node.stopRequested = true;
        node.stopRequestedMs = now;
        node.enableConfirmed = false;
        node.enableDesired = false;
        node.enablePending = false;
        latchFault(id, kFaultMoveTxFailed, false);
        return Result{kCodeUnavailable, "can_tx_failed"};
    }

    // The command changed the driver's target: the previous sample is obsolete.
    invalidateTarget(id);

    job_ = MoveJob{};
    job_.active = true;
    moveOutcome_ = MoveOutcome::Running;
    job_.id = id;
    job_.opcode = plan.withCurrentLimit ? kFrameDirectLimit : kFrameDirect;
    job_.targetProof = true;
    job_.startTenths = position;
    job_.targetTenths = resolved.targetTenths;
    // Tolerance is never allowed to reach the full travel: a tiny (0.1 degree)
    // move must not be declared done while the motor still sits at the start. A
    // zero-travel no-op has no travel to halve, so it is judged with a single
    // feedback count instead of an exact match that drift could never satisfy.
    if (resolved.travelTenths == 0) {
        job_.toleranceTenths = kNoOpToleranceTenths;
    } else {
        const uint32_t halfTravel = static_cast<uint32_t>(resolved.travelTenths / 2);
        job_.toleranceTenths = static_cast<int32_t>(
            halfTravel < static_cast<uint32_t>(kTargetToleranceTenths)
                ? halfTravel
                : static_cast<uint32_t>(kTargetToleranceTenths));
    }
    job_.expectedDurationMs = resolved.expectedDurationMs;
    job_.startMs = now;
    // The configured policy duration is the deadline, not an invented ramp: the
    // driver plans this motion itself, so there is no acceleration profile here
    // to derive a tighter one from.
    job_.deadlineMs = limits_.maxMoveDurationMs;
    job_.ackSeen = false;
    job_.doneUpdates = 0;
    job_.lastDonePosMs = now;
    job_.lastDoneVelMs = now;
    return Result{kCodeQueued, "queued"};
}

Result MotorControl::home(uint8_t id, uint8_t mode) {
    if (configPending()) return Result{kCodeBusy, "config_pending"};
    const uint32_t now = millis();
    if (id == 0) return Result{kCodeInvalid, "id_reserved"};
    if (mode > kHomeModeMax) return Result{kCodeInvalid, "home_mode_invalid"};

    refreshBusStatus();
    if (!canReady()) {
        return Result{
            kCodeUnavailable,
            busState_ == CanControllerState::BusOff ? "bus_off" : "can_unavailable"};
    }

    // Feedback is only tracked for the selected node, so select before the
    // freshness gates.
    watch(id);
    NodeState& node = nodes_[id];
    if (faultAppliesTo(id)) return Result{kCodeBusy, faultTag_};
    if (experimentId_) return Result{kCodeBusy, "experiment_active"};
    if (job_.active) {
        return Result{kCodeBusy, job_.id == id ? "busy" : "another_motor_active"};
    }
    // One supervised action at a time: a second homing run is rejected exactly
    // like a second move.
    if (home_.active) {
        return Result{kCodeBusy, homeId_ == id ? "home_active" : "another_motor_active"};
    }
    if (node.stopRequested) return Result{kCodeBusy, "stop_pending"};
    if (node.enablePending) return Result{kCodeBusy, "enable_pending"};
    if (!node.enableConfirmed) return Result{kCodeBusy, "not_enabled"};

    int32_t position = 0;
    int32_t velocity = 0;
    if (!freshPosition(id, now, position) || !freshVelocity(id, now, velocity)) {
        return Result{kCodeUnavailable, "feedback_unavailable"};
    }
    // Homing has no requested target: the position is only used to prove fresh
    // feedback exists, and the speed must be inside the stopped band.
    (void)position;
    const int32_t absSpeed = velocity < 0 ? -velocity : velocity;
    if (absSpeed > kStopSpeedTenths) return Result{kCodeBusy, "not_stopped"};

    if (!sendHomeTrigger(id, mode)) {
        // The trigger may or may not have reached the motor: stop best effort
        // and latch instead of leaving a possibly homing machine unsupervised.
        sendStop(id);
        node.stopRequested = true;
        node.stopRequestedMs = now;
        node.enableConfirmed = false;
        node.enableDesired = false;
        node.enablePending = false;
        latchFault(id, kFaultHomeTxFailed, false);
        return Result{kCodeUnavailable, "can_tx_failed"};
    }

    // A homing run moves the driver and can redefine its origin: the previous
    // target sample describes a state that no longer holds.
    invalidateTarget(id);

    home_ = HomeJob{};
    home_.active = true;
    home_.startMs = now;
    // Configured budget, not a new hardcoded timeout.
    home_.deadlineMs = limits_.maxMoveDurationMs;
    home_.lastDonePosMs = now;
    home_.lastDoneVelMs = now;
    homeId_ = id;
    homeMode_ = mode;
    homeOutcome_ = HomeOutcome::Running;
    return Result{kCodeQueued, "queued_home"};
}

bool MotorControl::rawLogical(const uint8_t* bytes, uint8_t length) {
    if (!bytes || length < 3 || length > 30) return false;
    // A raw frame must never interleave with a supervised operation, and the
    // bus must be healthy: nothing here stops or enables anything to make room.
    if (!canReady() || operationBusy()) return false;
    bus_.clearTransmissionError();
    const bool sent = bus_.sendRawLogical(bytes, length);
    if (!sent || bus_.hasTransmissionError()) {
        bus_.clearTransmissionError();
        return false;
    }
    // Known configuration writes keep their exact bytes, but retain independent
    // acceptance/readback evidence before a following queue step can run.
    if (length == 20 && bytes[0] != 0 && bytes[1] == 0x4C &&
        bytes[2] == 0xAE && bytes[19] == 0x6B) startConfig(bytes);
    return true;
}

void MotorControl::startConfig(const uint8_t* b) {
    const uint32_t next = config_.sequence + 1;
    config_ = ConfigTransaction{};
    config_.sequence = next;
    config_.id = b[0];
    config_.state = 1;
    config_.started = millis();
    memcpy(config_.expected, b + 4, 15);
    watch(b[0]);
}

const char* MotorControl::configMessage() const {
    switch (config_.state) {
        case 1: return "config_wait_ack";
        case 2: return "config_wait_readback";
        case 3: return "config_verified";
        case 4: return "config_rejected";
        case 5: return "config_ack_timeout";
        case 6: return "config_readback_timeout";
        case 7: return "config_mismatch";
        case 8: return "config_read_tx_failed";
        case 9: return "config_cancelled";
        default: return "none";
    }
}

bool MotorControl::configFrame(const CanRawFrame& f, uint32_t now) {
    if (!configPending() || !f.extended || f.remote || f.length < 1 || f.length > 8 ||
        (f.identifier >> 8) != config_.id) return false;
    const uint8_t packet = uint8_t(f.identifier);
    if (f.data[0] == 0x4C) {
        if (packet != 0 || f.length != 3 || f.data[2] != 0x6B || config_.state != 1) return true;
        config_.ack = f.data[1];
        if (config_.ack != 0x02) { config_.state = 4; return true; }
        config_.state = 2;
        config_.readAt = now;
        // pollConfig issues the read after this RX batch has drained, so a
        // buffered response predating our request cannot enter the new assembly.
        return true;
    }
    if (f.data[0] != 0x22 || config_.state != 2) return false;
    if (!config_.readIssued) return true;
    // Manual: logical reply [addr][22][15 parameter bytes][6B]. CAN repeats
    // opcode for each 7-byte slice. Reject reordered/duplicate/truncated parts.
    const uint8_t take = packet < 2 ? 7 : 2;
    if (packet != config_.packet || packet > 2 || f.length != take + 1) return true;
    memcpy(config_.actual + config_.received, f.data + 1, take);
    config_.received += take;
    ++config_.packet;
    if (config_.received == 16) {
        if (config_.actual[15] != 0x6B) { config_.packet = config_.received = 0; return true; }
        queries_.receive(config_.id,0x22,now);
        config_.state = memcmp(config_.expected, config_.actual, 15) == 0 ? 3 : 7;
    }
    return true;
}

void MotorControl::pollConfig(uint32_t now) {
    if (config_.state == 1 && uint32_t(now - config_.started) > 3000) config_.state = 5;
    if (config_.state == 2 && uint32_t(now - config_.readAt) > 3000) config_.state = 6;
}

String MotorControl::configJson() const {
    String j("{\"sequence\":"); j += config_.sequence;
    j += ",\"id\":"; j += config_.id;
    j += ",\"opcode\":76,\"state\":\""; j += configMessage();
    j += "\",\"pending\":"; j += configPending() ? "true" : "false";
    j += ",\"ack\":"; j += config_.ack;
    j += ",\"expected\":[";
    for (uint8_t i=0;i<15;++i) { if(i) j+=','; j+=config_.expected[i]; }
    j += "],\"actual\":[";
    for (uint8_t i=0;i<config_.received && i<15;++i) { if(i) j+=','; j+=config_.actual[i]; }
    j += "]}";
    return j;
}

bool MotorControl::rawCanFrame(uint32_t id, bool extended, const uint8_t* data, uint8_t length) {
    if (length > kMaxCanDataBytes) return false;
    if (!canReady() || operationBusy()) return false;
    bus_.clearTransmissionError();
    const bool sent = bus_.sendRawFrame(id, extended, data, length);
    if (!sent || bus_.hasTransmissionError()) {
        bus_.clearTransmissionError();
        return false;
    }
    return true;
}

bool MotorControl::queueSendLogical(const uint8_t* bytes, uint8_t length) {
    // Same wire path as the manual raw transport, but the queue is the only
    // owner while it runs: the existence of a manual operation or a latched fault
    // must not stop a program the operator asked to send.
    if (!bytes || length < 3 || length > 30) return false;
    if (!canReady()) return false;
    bus_.clearTransmissionError();
    queueTransport_=true;
    const bool sent = bus_.sendRawLogical(bytes, length);
    queueTransport_=false;
    if (!sent || bus_.hasTransmissionError()) {
        bus_.clearTransmissionError();
        return false;
    }
    // Track a successfully submitted immediate F3 without claiming hardware
    // success. The existing RX path requires both its ACK and fresh 3A flags.
    if (length == 6 && bytes[1] == kFrameEnable && bytes[2] == 0xAB &&
        bytes[3] <= 1 && bytes[4] == 0 && bytes[5] == 0x6B) {
        const uint8_t id = bytes[0];
        const bool enabled = bytes[3] != 0;
        for (uint16_t target = 1; target < kNodeCount; ++target) {
            if (id && target != id) continue;
            NodeState& node = nodes_[target];
            node.enableConfirmed = false;
            node.enableDesired = id ? enabled : false;
            node.enablePending = id != 0;
            node.enableAck = false;
            node.enableTimedOut = false;
            node.enablePendingMs = millis();
            if (!enabled) invalidateTarget(static_cast<uint8_t>(target));
        }
    }
    return true;
}

bool MotorControl::queueSendFrame(uint32_t id, bool extended, const uint8_t* data, uint8_t length) {
    if (length > kMaxCanDataBytes) return false;
    if (!canReady()) return false;
    bus_.clearTransmissionError();
    queueTransport_=true;
    const bool sent = bus_.sendRawFrame(id, extended, data, length);
    queueTransport_=false;
    if (!sent || bus_.hasTransmissionError()) {
        bus_.clearTransmissionError();
        return false;
    }
    return true;
}

void MotorControl::noteRawTransmission(uint8_t id) {
    queueDiagnostics_.invalidate(id);
    for (uint16_t nodeId = 1; nodeId < kNodeCount; ++nodeId) {
        if (id != 0 && nodeId != id) continue;
        NodeState& node = nodes_[nodeId];
        node.enablePending = false;
        node.enableConfirmed = false;
        syncEnableDesired(static_cast<uint8_t>(nodeId));
        // The position/velocity/flags confirmations describe the bus state
        // before the raw frame, which the frame may have changed.
        node.positionValid = false;
        node.velocityValid = false;
        node.flagsValid = false;
        node.homeFlagsValid = false;
        node.targetValid = false;
        node.targetMs = 0;
    }
    if (id == 0 || targetPollId_ == id) {
        targetPollId_ = 0;
    }
    if (id == 0) {
        job_ = MoveJob{};
        moveOutcome_ = MoveOutcome::None;
        home_ = HomeJob{};
        homeOutcome_ = HomeOutcome::None;
        experimentId_ = 0;
    } else {
        if (job_.active && job_.id == id) {
            job_ = MoveJob{};
            moveOutcome_ = MoveOutcome::None;
        }
        if (home_.active && homeId_ == id) {
            home_ = HomeJob{};
            homeOutcome_ = HomeOutcome::None;
        }
        if (experimentId_ == id) experimentId_ = 0;
    }
}

bool MotorControl::broadcastAbortAll() {
    // Abort local supervision first (an addressed 9C goes out for a live home),
    // then broadcast 9C for anything this board never supervised (a raw step may
    // have started homing), then the FE broadcast stop. Every part is best
    // effort and none of them is reported as a confirmed stop.
    cancelHome(true, false);
    if (canReady()) {
        const uint8_t frame[4] = {0, kFrameHomeInterrupt, 0x48, kProtocolChecksum};
        bus_.clearTransmissionError();
        bus_.sendRawLogical(frame, sizeof(frame));
        bus_.clearTransmissionError();
    }
    const Result stopped = stopAll();
    return stopped.code < 300;
}

Result MotorControl::stop(uint8_t id) {
    if (configPending() && config_.id == id) config_.state = 9;
    if (experimentId_ == id) experimentId_ = 0;
    const uint32_t now = millis();
    if (id == 0) return Result{kCodeInvalid, "id_reserved"};

    // Software tracking is cleared unconditionally so a stop still takes effect
    // while the bus is down; only the wire stop is gated below.
    if (job_.active && job_.id == id) { moveOutcome_ = MoveOutcome::Cancelled; job_ = MoveJob{}; }
    // A homing run is interrupted too: 9C aborts homing, the FE below halts the
    // motor. A cancel is not a completion.
    if (home_.active && homeId_ == id) cancelHome(true, false);
    // Selecting keeps the node in the query rotation so the stop can be seen.
    watch(id);
    NodeState& node = nodes_[id];
    node.stopRequested = true;
    node.stopRequestedMs = now;
    // A stop cancels any enable IN FLIGHT so a late F3 ack cannot re-enable.
    // An already-confirmed enable is deliberately kept: 0xFE halts the motor,
    // it does not disable the driver, so the operator does not have to re-enable
    // after every stop. Explicit disable and latched faults still clear it.
    node.enableAck = false; node.enableTimedOut = false;
    node.enablePending = false;
    // The desire follows the confirmation again: an enable that was only ever
    // requested (never confirmed) must not leave a phantom desire behind.
    syncEnableDesired(id);

    // The state stays "stop_requested" until a NEW stationary feedback sample
    // confirms it (or a latched fault persists).
    refreshBusStatus();
    if (!canReady()) {
        return Result{
            kCodeUnavailable,
            busState_ == CanControllerState::BusOff ? "bus_off" : "can_unavailable"};
    }
    if (!sendStop(id)) return Result{kCodeUnavailable, "can_tx_failed"};
    // The wire stop may have moved the motor away from its target.
    invalidateTarget(id);
    return Result{kCodeQueued, "queued"};
}

Result MotorControl::stopAll() {
    if (configPending()) config_.state = 9;
    if (job_.active) moveOutcome_ = MoveOutcome::Cancelled;
    experimentId_ = 0;
    const uint32_t now = millis();

    // A homing run is interrupted on the wire (9C) before the broadcast stop.
    cancelHome(true, false);

    // Cancel the job and the in-flight enables first: this holds even when the
    // broadcast cannot be sent.
    job_ = MoveJob{};
    for (uint16_t id = 1; id < kNodeCount; ++id) {
        NodeState& node = nodes_[id];
        // Merely selecting a never-seen address in the UI must not create a
        // permanent global stop wait for a motor that may not exist. The wire
        // broadcast still stops every address. Explicitly commanded/pending
        // nodes and observed selected nodes still require fresh stop evidence.
        const bool touched = (id == selectedId_ && node.seenEver) || node.stopRequested ||
            node.enablePending || node.enableDesired || node.enableConfirmed;
        if (!touched) continue;
        node.enableAck = false; node.enableTimedOut = false;
        node.stopRequested = true;
        node.stopRequestedMs = now;
        // Only an enable that was still in flight is dropped: an already
        // confirmed enable survives the broadcast stop, and an explicit disable
        // is still required to drop it. The desire follows the confirmation, so
        // a never-confirmed request leaves no phantom desire behind.
        node.enablePending = false;
        syncEnableDesired(static_cast<uint8_t>(id));
    }

    refreshBusStatus();
    if (!canReady()) {
        return Result{
            kCodeUnavailable,
            busState_ == CanControllerState::BusOff ? "bus_off" : "can_unavailable"};
    }

    // id 0 is the dedicated broadcast for stopAll.
    bus_.clearTransmissionError();
    bus_.stopNow(0, false);
    if (bus_.hasTransmissionError()) {
        bus_.clearTransmissionError();
        return Result{kCodeUnavailable, "can_tx_failed"};
    }
    // A broadcast stop can halt every node: no target sample survives it.
    invalidateTarget(0);
    return Result{kCodeQueued, "queued"};
}

Result MotorControl::broadcastEnable(bool enabled) {
    const uint8_t frame[] = {0, 0xF3, 0xAB, uint8_t(enabled ? 1 : 0), 0, 0x6B};
    // Broadcast has no per-node acknowledgement; do not invent confirmations.
    const bool sent = queueSendLogical(frame, sizeof(frame));
    if (!enabled) clearControlState();
    return sent ? Result{202, "broadcast_sent"} : Result{503, "can_tx_failed"};
}

void MotorControl::takeQueueControl() {
    job_ = MoveJob{};
    moveOutcome_ = MoveOutcome::None;
    home_ = HomeJob{};
    homeOutcome_ = HomeOutcome::None;
    homeId_ = homeMode_ = 0;
    experimentId_ = 0;
    experimentStart_ = 0;
    faultTag_ = kFaultNone;
    faultId_ = 0;
    faultGlobal_ = false;
    // Keep terminal config evidence for diagnosis, but end a pending transaction.
    if (configPending()) config_.state = 9;
    targetPollId_ = 0;
    targetPollArmedMs_ = 0;
    queries_.release(CanQueryScheduler::Controller);
    queries_.release(CanQueryScheduler::Await);
}

void MotorControl::clearControlState() {
    takeQueueControl();
    for (uint16_t id = 0; id < kNodeCount; ++id) nodes_[id] = NodeState{};
    // limits_, trace_, config evidence, and lastMoveFailure remain available.
    // A real bus fault will be observed again by the next poll.
}

const char* MotorControl::stateString(uint8_t id) const {
    if (experimentId_ == id) return "experiment_running";
    const NodeState& node = nodes_[id];
    if (faultAppliesTo(id)) return "fault";
    if (home_.active && homeId_ == id) return "homing";
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
    json.reserve(420);
    json += "{\"id\":";
    json += static_cast<unsigned int>(id);
    json += ",\"autoQueriesEnabled\":";
    json += autoQueriesEnabled_ ? "true" : "false";
    json += ",\"canReady\":";
    json += canReady() ? "true" : "false";
    json += ",\"busState\":\"";
    json += busStateString();
    json += "\",\"txErrors\":";
    json += static_cast<unsigned long>(txErrorCount());
    json += ",\"activeId\":";
    // The node the board currently supervises: a move, a trial or a homing run.
    json += static_cast<unsigned int>(
        job_.active ? job_.id : (experimentId_ ? experimentId_ : (home_.active ? homeId_ : 0)));
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
    // The driver's target position (0x33, manual V1.0.5 p70). Shown so a refused
    // mode-0 direct command is explainable instead of mysterious; it is null
    // whenever no fresh sample exists, and it is never the actual position and
    // never the 0x34 real-time setpoint.
    int32_t targetTenths = 0;
    const bool targetFresh = freshTarget(id, now, targetTenths);
    json += ",\"targetDeg\":";
    if (targetFresh) {
        json += String(static_cast<float>(targetTenths) / 10.0f, 1);
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
    // Homing supervision for future queue consumers. The outcome is the
    // controller's own verdict (never derived from a bare ack), homeId/homeMode
    // attribute the last run, and the 0x3B fields are this node's latest homing
    // status byte with its documented bit meanings.
    json += ",\"homeOutcome\":\"";
    json += homeOutcomeName(homeOutcome_);
    json += "\",\"homeActive\":";
    json += (home_.active && homeId_ == id) ? "true" : "false";
    json += ",\"homeId\":";
    json += static_cast<unsigned int>(homeId_);
    json += ",\"homeMode\":";
    if (homeId_ != 0) json += static_cast<unsigned int>(homeMode_); else json += "null";
    const bool homeFreshFlags =
        node.homeFlagsValid && ageWithin(now, node.homeFlagsMs, kFeedbackFreshMs);
    json += ",\"homeOrg\":";
    if (homeFreshFlags) json += static_cast<unsigned int>(node.homeFlags); else json += "null";
    json += ",\"homeRunning\":";
    if (homeFreshFlags) json += (node.homeFlags & kHomeStatusRunning) ? "true" : "false";
    else json += "null";
    json += ",\"homeFailed\":";
    if (homeFreshFlags) json += (node.homeFlags & kHomeStatusFailed) ? "true" : "false";
    else json += "null";
    json += ",\"lastAck\":\"";
    json += node.lastAck;
    json += "\",\"lastMoveFailure\":";
    if (moveFailure_.id != id) json += "null";
    else {
        json += "{\"targetDeg\":"; json += String(double(moveFailure_.target)/10.0,1);
        json += ",\"positionDeg\":";
        if (moveFailure_.positionValid) json += String(double(moveFailure_.position)/10.0,1); else json += "null";
        json += ",\"speedRpm\":";
        if (moveFailure_.velocityValid) json += String(double(moveFailure_.velocity)/10.0,1); else json += "null";
        json += ",\"elapsedMs\":"; json += moveFailure_.elapsed;
        json += ",\"deadlineMs\":"; json += moveFailure_.deadline;
        json += ",\"enabled\":"; json += moveFailure_.enabled ? "true" : "false";
        json += '}';
    }
    // Expose board-wide blockers, not only the selected motor's local state.
    // Historical faults are separate from live operation ownership.
    json += ",\"control\":{\"busy\":";
    json += operationBusy() ? "true" : "false";
    json += ",\"stationary\":";
    json += stationaryFeedback(id, now, 0) ? "true" : "false";
    json += ",\"fault\":\""; json += faultTag_;
    json += "\",\"faultId\":"; json += static_cast<unsigned int>(faultId_);
    json += ",\"faultGlobal\":"; json += faultGlobal_ ? "true" : "false";
    json += ",\"blockers\":[";
    uint16_t blockerCount = 0;
    const auto blocker = [&](uint8_t target, const char* reason, uint32_t since) {
        ++blockerCount;
        if (blockerCount > 16) return;
        if (blockerCount > 1) json += ',';
        json += "{\"id\":"; json += static_cast<unsigned int>(target);
        json += ",\"reason\":\""; json += reason;
        json += "\",\"ageMs\":"; json += static_cast<unsigned long>(now - since);
        json += '}';
    };
    if (configPending()) blocker(config_.id, "config_pending", config_.started);
    if (job_.active) blocker(job_.id, "move_active", job_.startMs);
    if (home_.active) blocker(homeId_, "home_active", home_.startMs);
    if (experimentId_) blocker(experimentId_, "experiment_active", experimentStart_);
    for (uint16_t target = 1; target < kNodeCount; ++target) {
        const NodeState& pending = nodes_[target];
        if (pending.enablePending) blocker(static_cast<uint8_t>(target), "enable_pending", pending.enablePendingMs);
        if (pending.stopRequested) blocker(static_cast<uint8_t>(target), "stop_pending", pending.stopRequestedMs);
    }
    json += "],\"blockerCount\":"; json += static_cast<unsigned int>(blockerCount);
    json += "}}";
    return json;
}

void MotorControl::demoProbe(uint8_t id, uint8_t field) {
    if (!id || !canReady()) return;
    // Demo refresh shares the same budget as await and sync. Direct 20 ms
    // probes reset lastTraffic continually and starve the awaited 0x33 target
    // query whenever the configured gap exceeds 20 ms.
    const uint8_t fields[] = {0x36, 0x35, 0x3A, 0x3B};
    queries_.demand(id, fields[field % 4], CanQueryScheduler::Demo,
                    400, 1000, 0, millis());
}

bool MotorControl::demoDriverFault(uint8_t id) const {
    const auto& n = nodes_[id];
    return faultAppliesTo(id) ||
        n.demoRejected ||
        (n.flagsValid && (n.flags & 0x08)) || // stall protection; collision detection alone is not a fault
        (n.homeFlagsValid && (n.homeFlags & 0x38)) ||
        n.queueAckStatus == 0xE2 || n.queueAckStatus == 0xEE;
}

bool MotorControl::demoFlags(uint8_t id, uint8_t& flags, uint32_t& age) const {
    const auto& n = nodes_[id];
    age = n.flagsValid ? millis() - n.flagsMs : UINT32_MAX;
    flags = n.flags;
    return n.flagsValid && age <= kFeedbackFreshMs;
}

MotorControl::Snapshot MotorControl::snapshot(uint8_t id) const {
    Snapshot s;
    if (!id) return s;
    const NodeState& n = nodes_[id]; const uint32_t now = millis();
    s.positionAge=n.positionValid ? now-n.positionMs : UINT32_MAX;
    s.velocityAge=n.velocityValid ? now-n.velocityMs : UINT32_MAX;
    s.currentAge=n.currentValid ? now-n.currentMs : UINT32_MAX;
    s.positionValid=n.positionValid && s.positionAge<=kFeedbackFreshMs;
    s.velocityValid=n.velocityValid && s.velocityAge<=kFeedbackFreshMs;
    s.currentValid=n.currentValid && s.currentAge<=kFeedbackFreshMs;
    s.position=n.positionTenths; s.velocity=n.velocityTenths; s.current=n.currentMa;
    s.enabled=n.enableConfirmed; s.enablePending=n.enablePending;
    s.enableAck=n.enableAck; s.enableTimedOut=n.enableTimedOut;
    s.stopPending=n.stopRequested; s.fault=faultAppliesTo(id); return s;
}
bool MotorControl::operationBusy() const {
    if (configPending()) return true;
    if (job_.active || experimentId_ || home_.active) return true;
    for (uint16_t id=1; id<kNodeCount; ++id)
        if (nodes_[id].enablePending || nodes_[id].stopRequested) return true;
    return false;
}

bool MotorControl::hasActiveMotion() const {
    if (job_.active || experimentId_ || home_.active) return true;
    for (uint16_t id = 1; id < kNodeCount; ++id) {
        const NodeState& n = nodes_[id];
        if (n.enablePending || n.stopRequested || n.enableConfirmed || n.enableDesired) return true;
    }
    return false;
}

bool MotorControl::setDebugLimits(const DebugLimits& limits) {
    if (!validDebugLimits(limits) || operationBusy()) return false;
    limits_ = limits;
    return true;
}

Result MotorControl::command(const uint8_t* b, uint8_t n) {
    const CommandKind kind = validateCommand(b, n, limits_);
    if (kind == CommandKind::Invalid) {
        // A well-formed 0x4C write that only failed a policy bound gets its own
        // reason instead of the generic "unsupported" answer.
        if (const char* directReason = directPositionRefusal(b, n)) {
            return Result{400, directReason};
        }
        // A well-formed 0x45 that only failed the configured current policy says
        // so; the write itself needs no new transport, the existing Configure
        // gate (disabled driver, fresh stationary feedback) applies unchanged.
        if (const char* currentReason = closedLoopCurrentRefusal(b, n, limits_)) {
            return Result{400, currentReason};
        }
        if (const char* homeReason = homeParamRefusal(b, n, limits_)) {
            return Result{400, homeReason};
        }
        DebugLimits ceiling;
        ceiling.maxSpeedTenths = 30000; ceiling.maxAccelRpmS = 65535;
        if (validateCommand(b,n,ceiling) == CommandKind::Experiment) {
            if ((b[1] == 0xF6 || b[1] == 0xC6) && word(b+3) > limits_.maxAccelRpmS)
                return Result{400,"accel_out_of_range"};
            if (((b[1] == 0xF6 || b[1] == 0xC6) && word(b+5) > limits_.maxSpeedTenths) ||
                (b[1] == 0xC5 && word(b+8) > limits_.maxSpeedTenths))
                return Result{400,"speed_out_of_range"};
            return Result{400,"current_out_of_range"};
        }
        return Result{400, "unsupported_or_invalid_command"};
    }
    const uint8_t id = b[0];
    if (configPending() && kind != CommandKind::Read && kind != CommandKind::Stop &&
        kind != CommandKind::Interrupt && !(kind == CommandKind::Enable && b[3] == 0))
        return Result{409, "config_pending"};
    if (kind == CommandKind::Enable) return enable(id, b[3] != 0);
    if (kind == CommandKind::Stop) return stop(id);
    if (kind == CommandKind::Home) return home(id, b[2]);
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
    if (kind == CommandKind::DirectMove) {
        // The frame is taken as it is: FB stays FB, CB stays CB, and the fields
        // keep their documented positions (speed at 3, angle at 5, mode at 9,
        // sync at 10, current at 11 on CB only). Nothing is translated into the
        // trapezoid CD form and no acceleration is invented.
        DirectPositionRequest request;
        request.id = id;
        request.direction = b[2];
        request.speedTenths = word(b+3);
        request.angleTenths = dword(b+5);
        request.motionMode = b[9];
        request.sync = b[10] != 0;
        request.withCurrentLimit = b[1] == kFrameDirectLimit;
        request.currentMa = request.withCurrentLimit ? word(b+11) : 0;
        return directPosition(request);
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
        if (job_.active || experimentId_ || home_.active || anyStopPending() || node.enablePending ||
            faultAppliesTo(id) || !node.enableConfirmed || !stationaryFeedback(id, millis(), 0))
            return Result{409, "enable_and_wait_for_stationary_feedback"};
    }
    bus_.clearTransmissionError();
    const bool queued = bus_.sendValidatedCommand(b, n);
    if (kind == CommandKind::Interrupt) {
        // 0x9C aborts a homing run. The operator's own frame is already on the
        // wire, so the supervised run is cancelled without a second interrupt
        // and the motor is then halted.
        if (home_.active && homeId_ == id) cancelHome(false, false);
        stop(id);
    }
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
        // A velocity/torque run takes the driver off any position target.
        invalidateTarget(id);
    }
    if (kind == CommandKind::Configure) {
        node.positionValid = node.velocityValid = node.flagsValid = false;
        // A parameter write (origin reset, control mode, ...) can redefine what
        // the driver calls its target.
        invalidateTarget(id);
        node.enableConfirmed = false;
        // A parameter write drops the enable confirmation (the driver may have
        // reset), so the desire must follow it down instead of lingering.
        syncEnableDesired(id);
        if (b[1] == 0x4C) startConfig(b);
    }
    return Result{202, kind == CommandKind::Experiment ? "queued_experiment" : "queued"};
}

void MotorControl::traceSink(void* context, const CanRawFrame& frame, bool tx) {
    MotorControl* self = static_cast<MotorControl*>(context);
    if (tx) { ++self->txFrameCount_; self->queries_.noteTraffic(millis()); }
    if(tx && !self->queueTransport_ && frame.length && (frame.identifier&0xFF)==0 &&
       !CanQueryScheduler::supported(frame.data[0])) {
        self->queueDiagnostics_.invalidate(uint8_t(frame.identifier>>8));
    }
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
