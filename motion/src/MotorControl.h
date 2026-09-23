#pragma once
// Bounded motion module for the X42S/X28S CAN motors on an ESP32-S3 (TWAI).
//
// Responsibilities:
//   * own the CAN driver (X42sProtocol) and the polling of motor feedback
//   * keep exactly one supervised action at a time (move, trial or homing)
//   * only confirm "enabled", "move done" or "home done" from real post-command
//     feedback, never from a bare 0x02 ack or a single idle status byte
//   * latch visible faults instead of silently retrying
//
// RX work is bounded; CAN transmission can wait up to 50 ms per frame. Sends one
// queued query and services the active job. Everything is millis() based.

#include <Arduino.h>
#include <cstring>

#include "MotionCore.h"
#include "X42sProtocol.h"

namespace motion {

class MotorControl {
public:
    MotorControl();

    // Initializes CAN only. Never enables, moves or stops any motor.
    bool begin(int tx, int rx, long bitrate);

    // Bounded housekeeping (CAN TX may briefly block). Call often from loop().
    void poll();
    void setAutoQueriesEnabled(bool enabled) { autoQueriesEnabled_ = enabled; }
    bool autoQueriesEnabled() const { return autoQueriesEnabled_; }

    // Selects the address the module tracks (1..255). Selecting a new id never
    // consumes a slot: queries always cover the selected id, the active job and
    // any node with a pending enable/stop.
    void watch(uint8_t id);

    // Requests the firmware enable state. Result is 202 queued; the enable is
    // only reported as true after matching F3 ACK and fresh enabled 3A flags.
    Result enable(uint8_t id, bool enabled);
    Result broadcastEnable(bool enabled);

    // Queues one relative position move. Requires a confirmed enable and fresh,
    // approximately stationary feedback.
    Result move(const MoveRequest& request);

    // Queues one direct (passthrough) position command: 0xFB or 0xCB, immediate
    // execution only. The bytes and the opcode stay exactly as documented - the
    // command is never translated into the trapezoid CD form - and no
    // acceleration is invented: the driver plans this motion itself.
    //
    // Same gates as move() (confirmed enable, fresh stationary feedback, single
    // supervised owner), plus:
    //   * mode 0 is resolved against a FRESH 0x33 sample of the driver's target
    //     position (manual V1.0.5 p70). Without one the request is refused with
    //     "target_not_fresh" and a bounded refresh is started; the actual
    //     position, the last locally sent target and the 0x34 real-time setpoint
    //     (p71, possibly mid-trajectory) are never substituted for it.
    //   * the travel policy applies to |resolved target - current position|.
    //   * completion needs a matched FB/CB acknowledgement, a fresh 0x33 target
    //     sample and two distinct fresh stationary position/velocity pairs.
    Result directPosition(const DirectPositionRequest& request);

    // Triggers one homing run (0x9A, immediate). Requires a confirmed enable and
    // fresh, approximately stationary feedback, exactly like a move. Homing is
    // supervised: the completion is proven from post-start homing status
    // (0x3B) or an explicit documented 9A/9F completion plus fresh stationary
    // feedback, never from a 0x02 ack or from a single "not homing" byte.
    Result home(uint8_t id, uint8_t mode);

    // Cancels software tracking and sends a stop. The state stays
    // "stop_requested" until fresh stationary feedback confirms it. An
    // already-confirmed enable survives a stop (0xFE halts the motor, it does
    // not disable the driver); an enable still in flight is cancelled so a late
    // F3 acknowledgement cannot re-enable the node.
    Result stop(uint8_t id);

    // Stops every watched node plus the active job using the id 0 broadcast.
    // Confirmed enables survive, in-flight enables do not.
    Result stopAll();

    // Operator-requested software reset, after the caller has cancelled writers
    // and attempted broadcast stop. No CAN re-init, NVS write, enable or motion.
    // Clears stale ownership; it is NOT evidence of physical stop.
    void clearControlState();

    // Complete JSON object for one address. Fields without fresh data are null.
    String statusJson(uint8_t id) const;
    String canDebugJson() const;
    String traceJson() const;
    Result command(const uint8_t* bytes, uint8_t length);
    const DebugLimits& debugLimits() const { return limits_; }
    bool setDebugLimits(const DebugLimits& limits);

    // --- Raw transport for the board queue (see queue-contract.md) ----------
    // Both require a ready bus and no active supervised operation, and neither
    // stops nor enables anything implicitly. They report transmission only:
    // no motion completion or parameter policy. Known 4C logical frames are
    // tracked separately through ACK and 22 readback before queue advancement.
    bool rawLogical(const uint8_t* bytes, uint8_t length);
    bool rawCanFrame(uint32_t id, bool extended, const uint8_t* data, uint8_t length);

    // --- Queue-only direct transport ----------------------------------------
    // The board queue is a command sender (see CommandQueue.h): these put one
    // frame on the bus and report transmission only. They check the buffer / ID /
    // DLC bounds and CAN readiness and nothing else - no operationBusy gate, no
    // supervision, no enable/job/home state, no configuration transaction - so a
    // running program can send the next line immediately. The manual and lab
    // paths above keep their existing semantics unchanged.
    bool queueSendLogical(const uint8_t* bytes, uint8_t length);
    bool queueSendFrame(uint32_t id, bool extended, const uint8_t* data, uint8_t length);

    // A raw frame was put on the bus: every software confirmation that depended
    // on the previous bus state is dropped (enable, position/velocity freshness,
    // move and home ownership). Nothing is inferred about the motor, so a later
    // structured step needs a fresh explicit enable. id 0 = every node.
    void noteRawTransmission(uint8_t id);

    // Broadcast 9C (abort homing, manual p62) followed by the FE broadcast stop.
    // Used when a run is cancelled or fails: a raw step may have started a
    // homing run this board never supervised. Best effort; the return value
    // reports whether the stop broadcast was accepted.
    bool broadcastAbortAll();

    // True when at least one node has fresh position *and* velocity.
    bool anyMotorOnline() const;
    bool hasActiveMotion() const;

    enum class MoveOutcome : uint8_t { None, Running, Done, Cancelled, Failed };
    // Homing outcome. NoMotion is the manual's 12/22 answer ("already at the
    // origin or the limit is already triggered, the motor does not move"): it is
    // a finished attempt, but it is NOT a completion claim and not a fault.
    //
    // Frozen names for the frontend/queue (do not rename):
    //   homeOutcome strings: "none" | "running" | "done" | "no_motion" |
    //                        "cancelled" | "failed"
    //   statusJson.homeOrg  is the raw 0x3B homing status byte of that node
    //                       (the manual's Org), with homeRunning = bit2 and
    //                       homeFailed = bit3 decoded from the same byte.
    enum class HomeOutcome : uint8_t { None, Running, Done, NoMotion, Cancelled, Failed };
    static const char* homeOutcomeName(HomeOutcome outcome);
    struct Snapshot {
        bool positionValid=false, velocityValid=false, currentValid=false;
        bool enabled=false, enablePending=false, stopPending=false, fault=false;
        bool enableAck=false, enableTimedOut=false;
        int32_t position=0, velocity=0; uint16_t current=0;
        uint32_t positionAge=UINT32_MAX, velocityAge=UINT32_MAX, currentAge=UINT32_MAX;
    };
    Snapshot snapshot(uint8_t id) const;
    // Read-only demo supervisor access; does not acquire manual job ownership.
    void demoProbe(uint8_t id, uint8_t field);
    void demoWatch(uint8_t id, bool enabled) { demoWatched_[id] = enabled; }
    bool demoDriverFault(uint8_t id) const;
    bool demoFlags(uint8_t id, uint8_t& flags, uint32_t& age) const;
    bool operationBusy() const;
    bool stopping() const { return anyStopPending(); }
    bool hasFault() const { return faultTag_ && strcmp(faultTag_, "none") != 0; }
    bool canRecover(uint8_t id) const { return !faultGlobal_ && faultId_ == id; }
    bool configPending() const { return config_.state == 1 || config_.state == 2; }
    bool configFailed() const { return config_.state >= 4; }
    const char* configMessage() const;
    String configJson() const;
    // Stable identifier of the latched fault ("none" when there is none), so a
    // supervisor (the board queue) can report WHY a run ended instead of a
    // generic "fault_active".
    const char* faultTag() const { return faultTag_; }
    bool ready() const { return canReady(); }
    MoveOutcome moveOutcome() const { return moveOutcome_; }
    // Homing accessors. homeId()/homeMode() keep the last requested run so a
    // finished outcome can still be attributed; homeActive() tells whether one
    // is running right now.
    HomeOutcome homeOutcome() const { return homeOutcome_; }
    uint8_t homeId() const { return homeId_; }
    uint8_t homeMode() const { return homeMode_; }
    bool homeActive() const { return home_.active; }

private:
    friend class CommandQueue;
    bool demoWatched_[256] = {};
    uint8_t queueObserveId_ = 0;
    struct MoveFailure {
        uint8_t id = 0;
        int64_t target = 0;
        int32_t position = 0, velocity = 0;
        uint32_t elapsed = 0, deadline = 0;
        bool positionValid = false, velocityValid = false, enabled = false;
    } moveFailure_;
    // One serialized 4C write and its 22 readback. Terminal evidence is retained
    // independently of the rolling CAN trace and ordinary feedback polling.
    struct ConfigTransaction {
        bool readIssued = false;
        uint32_t sequence = 0, started = 0, readAt = 0;
        uint8_t id = 0, state = 0, ack = 0, packet = 0, received = 0;
        uint8_t expected[15] = {}, actual[16] = {};
    } config_;
    void startConfig(const uint8_t* bytes);
    bool configFrame(const CanRawFrame& frame, uint32_t now);
    void pollConfig(uint32_t now);
    DebugLimits limits_;
    static void traceSink(void* context, const CanRawFrame& frame, bool tx);
    struct TraceEntry { CanRawFrame frame; uint32_t sequence = 0, atMs = 0; bool tx = false; };
    TraceEntry trace_[48];
    uint32_t traceSequence_ = 0;
    uint8_t traceNext_ = 0, traceCount_ = 0;
    uint8_t experimentId_ = 0;
    uint32_t experimentStart_ = 0;
    enum : uint16_t { kNodeCount = 256, kMaxQueryTargets = 4 };

    struct NodeState {
        bool demoRejected = false;
        bool seenEver = false;
        uint32_t lastSeenMs = 0;

        bool positionValid = false;
        int32_t positionTenths = 0;
        uint32_t positionMs = 0;

        bool velocityValid = false;
        int32_t velocityTenths = 0;
        uint32_t velocityMs = 0;

        bool flagsValid = false;
        uint8_t flags = 0;
        uint32_t flagsMs = 0;

        // 0x3B homing status byte (bit0 encoder ready, bit1 calibration ready,
        // bit2 homing running, bit3 homing failed, bit4 over-temp, bit5
        // over-current). Kept separate from the 0x3A motor flags: they are
        // different bit tables and must never be mixed.
        bool homeFlagsValid = false;
        uint8_t homeFlags = 0;
        uint32_t homeFlagsMs = 0;

        bool currentValid = false;
        uint16_t currentMa = 0;
        uint32_t currentMs = 0;

        // 0x33: the driver's target position (manual V1.0.5 p70), i.e. the
        // target the last position command asked for. This is the value mode-0
        // direct commands are resolved against and the proof of completion. It
        // is deliberately kept apart from the actual position (0x36), from
        // anything this board last sent and from the 0x34 real-time setpoint
        // (p71), which may be an intermediate trajectory value.
        bool targetValid = false;
        int32_t targetTenths = 0;
        uint32_t targetMs = 0;

        bool enableConfirmed = false;
        bool enableDesired = false;
        bool enablePending = false;
        bool enableAck = false, enableTimedOut = false;
        uint32_t enablePendingMs = 0;

        bool stopRequested = false;
        uint32_t stopRequestedMs = 0;

        const char* lastAck = "none";
        uint8_t queueAckFunction = 0, queueAckStatus = 0;
        uint8_t queueExpectedFunction = 0;
        bool queueAckPending = false;
        uint32_t queueAckMs = 0;
        bool queueHomeRunning = false, queueHomeComplete = false, queueHomeFailed = false;
        uint32_t queueHomeProofMs = 0;
        uint32_t lastAckMs = 0;
    };

    struct MoveJob {
        bool active = false;
        uint8_t id = 0;
        // Opcode this job was started with (0xCD for a trapezoid move, 0xFB/0xCB
        // for a direct one). An acknowledgement may only ever confirm the job
        // whose opcode it matches, so an ack of another command cannot complete
        // this one.
        uint8_t opcode = kFrameMove;
        // The position feedback is a full int32 and the travel is added to it,
        // so the target and the error are kept in int64 to avoid overflow.
        int64_t startTenths = 0;
        int64_t targetTenths = 0;
        int32_t toleranceTenths = 0;
        uint32_t expectedDurationMs = 0;
        uint32_t startMs = 0;
        uint32_t deadlineMs = 0;
        bool ackSeen = false;
        // Direct (FB/CB) jobs additionally prove completion with the driver's own
        // target sample: a fresh 0x33 that reports the resolved target.
        bool targetProof = false;
        uint8_t doneUpdates = 0;
        // Timestamp of the last sample pair counted towards completion, so the
        // same feedback sample is never counted twice.
        uint32_t lastDonePosMs = 0;
        uint32_t lastDoneVelMs = 0;
        uint32_t lastDoneTargetMs = 0;
    };

    // CAN plumbing.
    bool canReady() const;
    void refreshBusStatus();
    const char* busStateString() const;
    uint32_t txErrorCount() const;
    void drainRx(uint32_t now);
    void handleFrame(const CanRawFrame& frame, uint32_t now);
    void handleAck(uint8_t id, uint8_t function, uint8_t status, uint32_t now);
    bool sendStop(uint8_t id);
    bool sendHomeTrigger(uint8_t id, uint8_t mode);
    bool sendHomeInterrupt(uint8_t id);

    // Feedback helpers.
    bool freshPosition(uint8_t id, uint32_t now, int32_t& out) const;
    bool freshVelocity(uint8_t id, uint32_t now, int32_t& out) const;
    // Fresh 0x33 target sample of the driver, or false when none arrived in the
    // window. A stale sample is never used to resolve a mode-0 command.
    bool freshTarget(uint8_t id, uint32_t now, int32_t& out) const;
    // Drops the driver's target sample for one node (0 = every node). Every frame
    // that can change what the driver considers its target invalidates it.
    void invalidateTarget(uint8_t id);
    // Bounded 0x33 polling for one node: one immediate probe plus at most
    // kMaxTargetProbes further probes inside kTargetPollWindowMs, sent in
    // addition to the normal feedback rotation.
    void armTargetPoll(uint8_t id, uint32_t now);
    // Fresh 0x3B homing status byte, or false when none arrived in the window.
    bool freshHomeFlags(uint8_t id, uint32_t now, uint8_t& out) const;
    // Fresh position *and* velocity, both strictly after `sinceMs`, with the
    // speed inside the "approximately stopped" band. sinceMs == 0 skips the
    // ordering requirement.
    bool stationaryFeedback(uint8_t id, uint32_t now, uint32_t sinceMs) const;
    bool faultAppliesTo(uint8_t id) const;
    // Any node whose feedback is currently needed: the selected node, the
    // active job, the homing node and nodes with a pending enable or stop.
    bool nodeOfInterest(uint8_t id) const;
    bool anyStopPending() const;

    // Poll services.
    void serviceEnableTimeouts(uint32_t now);
    void serviceJob(uint32_t now);
    void serviceHome(uint32_t now);
    void serviceStopConfirmations(uint32_t now);
    void serviceQueries(uint32_t now);

    // Homing supervision. endHome() only records the outcome; cancelHome() and
    // failHome() also abort the run on the wire (9C interrupts homing, FE halts
    // the motor) and a failure latches a fault, which invalidates the enable.
    void endHome(HomeOutcome outcome);
    void cancelHome(bool interruptWire, bool stopWire);
    void failHome(const char* tag, uint32_t now);

    // Fault handling. A latched fault always invalidates the enable
    // confirmation (and cancels a pending enable) so a fresh explicit enable is
    // required.
    void latchFault(uint8_t id, const char* tag, bool global);
    void clearFault(uint8_t id);
    void failJob(const char* tag, uint32_t now);
    // `enableDesired` may only be true while an enable is confirmed or still in
    // flight. Without this, dropping a confirmation while a request was pending
    // leaves a phantom desire behind: hasActiveMotion() would stay true forever
    // and every parameter write would be refused with
    // "disable_and_wait_for_stationary_feedback" although nothing is enabled and
    // nothing is pending.
    void syncEnableDesired(uint8_t id);

    const char* stateString(uint8_t id) const;

    struct RxDiagnostic { CanRawFrame frame; uint32_t atMs = 0; };
    RxDiagnostic rxDiagnostics_[8];
    uint8_t diagnosticNext_ = 0, diagnosticCount_ = 0;
    uint32_t rxCount_ = 0;

    X42sProtocol can_;
    bool canReady_ = false;
    CanControllerState busState_ = CanControllerState::Unavailable;
    uint32_t txErrorCounter_ = 0;

    NodeState nodes_[kNodeCount];
    uint8_t selectedId_ = 0;

    // Homing supervision state. The trigger is a single 0x9A frame; completion
    // is never taken from the ack alone.
    //
    // Two independent proofs exist and both are timestamped, because a stationary
    // sample may only count when it is NEWER than the proof:
    //   * stoppedMs — inferred proof: the first post-start 0x3B sample that
    //     reported "not running, no failure" AFTER bit2 had been observed set.
    //     It is only usable while the 0x3B status is still fresh, and it is
    //     withdrawn (0) as soon as a newer status reports running again, so a
    //     stale "not homing" byte can never complete a later run.
    //   * completedMs — explicit proof: the documented 9A/9F completion reply
    //     arrived at this time. This path needs no 0x3B at all.
    struct HomeJob {
        bool active = false;
        uint32_t startMs = 0;
        // Duration budget taken from the configured limits (no extra hardcoded
        // timeout); it is a duration, compared wrap-safely against startMs.
        uint32_t deadlineMs = 0;
        bool ackSeen = false;
        bool runningSeen = false;
        uint32_t stoppedMs = 0;
        uint32_t completedMs = 0;
        uint8_t doneUpdates = 0;
        uint32_t lastDonePosMs = 0;
        uint32_t lastDoneVelMs = 0;
    };

    MoveJob job_;
    MoveOutcome moveOutcome_ = MoveOutcome::None;

    HomeJob home_;
    HomeOutcome homeOutcome_ = HomeOutcome::None;
    uint8_t homeId_ = 0, homeMode_ = 0;

    const char* faultTag_ = "none";
    uint8_t faultId_ = 0;
    bool faultGlobal_ = false;

    uint32_t lastQueryMs_ = 0;
    bool autoQueriesEnabled_ = true;
    uint8_t querySlot_ = 0;
    uint8_t queryFieldIndex_ = 0;

    // Bounded on-demand 0x33 refresh. `targetPollId_` is 0 when nothing is being
    // refreshed; the window and the probe cap keep a quiet node from turning into
    // an endless poll.
    uint8_t targetPollId_ = 0;
    uint32_t targetPollArmedMs_ = 0;
    uint8_t targetPollProbes_ = 0;
};

}  // namespace motion
