#pragma once
// Bounded motion module for the X42S/X28S CAN motors on an ESP32-S3 (TWAI).
//
// Responsibilities:
//   * own the CAN driver (X42sProtocol) and the polling of motor feedback
//   * keep exactly one active relative move at a time
//   * only confirm "enabled" or "move done" from real post-command feedback
//   * latch visible faults instead of silently retrying
//
// RX work is bounded; CAN transmission can wait up to 50 ms per frame. Sends one
// queued query and services the active job. Everything is millis() based.

#include <Arduino.h>

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

    // Selects the address the module tracks (1..255). Selecting a new id never
    // consumes a slot: queries always cover the selected id, the active job and
    // any node with a pending enable/stop.
    void watch(uint8_t id);

    // Requests the firmware enable state. Result is 202 queued; the enable is
    // only reported as true after the matching F3 acknowledgement arrives.
    Result enable(uint8_t id, bool enabled);

    // Queues one relative position move. Requires a confirmed enable and fresh,
    // approximately stationary feedback.
    Result move(const MoveRequest& request);

    // Cancels software tracking and sends a stop. The state stays
    // "stop_requested" until fresh stationary feedback confirms it.
    Result stop(uint8_t id);

    // Stops every watched node plus the active job using the id 0 broadcast.
    Result stopAll();

    // Complete JSON object for one address. Fields without fresh data are null.
    String statusJson(uint8_t id) const;
    String canDebugJson() const;
    String traceJson() const;
    Result command(const uint8_t* bytes, uint8_t length);

    // True when at least one node has fresh position *and* velocity.
    bool anyMotorOnline() const;
    bool hasActiveMotion() const;

private:
    static void traceSink(void* context, const CanRawFrame& frame, bool tx);
    struct TraceEntry { CanRawFrame frame; uint32_t sequence = 0, atMs = 0; bool tx = false; };
    TraceEntry trace_[48];
    uint32_t traceSequence_ = 0;
    uint8_t traceNext_ = 0, traceCount_ = 0;
    uint8_t experimentId_ = 0;
    uint32_t experimentStart_ = 0;
    enum : uint16_t { kNodeCount = 256, kMaxQueryTargets = 4 };

    struct NodeState {
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

        bool currentValid = false;
        uint16_t currentMa = 0;
        uint32_t currentMs = 0;

        bool enableConfirmed = false;
        bool enableDesired = false;
        bool enablePending = false;
        uint32_t enablePendingMs = 0;

        bool stopRequested = false;
        uint32_t stopRequestedMs = 0;

        const char* lastAck = "none";
        uint32_t lastAckMs = 0;
    };

    struct MoveJob {
        bool active = false;
        uint8_t id = 0;
        // The position feedback is a full int32 and the travel is added to it,
        // so the target and the error are kept in int64 to avoid overflow.
        int64_t startTenths = 0;
        int64_t targetTenths = 0;
        int32_t toleranceTenths = 0;
        uint32_t expectedDurationMs = 0;
        uint32_t startMs = 0;
        uint32_t deadlineMs = 0;
        bool ackSeen = false;
        uint8_t doneUpdates = 0;
        // Timestamp of the last sample pair counted towards completion, so the
        // same feedback sample is never counted twice.
        uint32_t lastDonePosMs = 0;
        uint32_t lastDoneVelMs = 0;
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

    // Feedback helpers.
    bool freshPosition(uint8_t id, uint32_t now, int32_t& out) const;
    bool freshVelocity(uint8_t id, uint32_t now, int32_t& out) const;
    // Fresh position *and* velocity, both strictly after `sinceMs`, with the
    // speed inside the "approximately stopped" band. sinceMs == 0 skips the
    // ordering requirement.
    bool stationaryFeedback(uint8_t id, uint32_t now, uint32_t sinceMs) const;
    bool faultAppliesTo(uint8_t id) const;
    // Any node whose feedback is currently needed: the selected node, the
    // active job and nodes with a pending enable or stop.
    bool nodeOfInterest(uint8_t id) const;
    bool anyStopPending() const;

    // Poll services.
    void serviceEnableTimeouts(uint32_t now);
    void serviceJob(uint32_t now);
    void serviceStopConfirmations(uint32_t now);
    void serviceQueries(uint32_t now);

    // Fault handling. A latched fault always invalidates the enable
    // confirmation so a fresh explicit enable is required.
    void latchFault(uint8_t id, const char* tag, bool global);
    void clearFault(uint8_t id);
    void failJob(const char* tag, uint32_t now);

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

    MoveJob job_;

    const char* faultTag_ = "none";
    uint8_t faultId_ = 0;
    bool faultGlobal_ = false;

    uint32_t lastQueryMs_ = 0;
    uint8_t querySlot_ = 0;
    uint8_t queryFieldIndex_ = 0;
};

}  // namespace motion
