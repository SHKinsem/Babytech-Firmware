#pragma once

#include "MotionStateRecord.h"

namespace babytech { namespace boardlink {

enum class MotionLoad { Ready, Missing, Corrupt, IoError, IdentityMismatch };
enum class MotionWrite { Stored, Unchanged, Invalid, Conflict, Busy, QueueFull, Expired,
                         ContextRequired, StorageFault };

// Single owner only, not a concurrent transaction or a remote entry point.
// Pairing must already be verified against actual hardware. Storage readiness
// is not mechanical authorization. Only Stored for a newly accepted intent in
// the current recordDecision call can precede action, subject to external gates.
// Unchanged never authorizes replay. Recovered Intent requires supervised Stop
// and recovery, never resume. This class performs no hardware or runtime gates.
class MotionStateStore {
public:
    MotionStateStore() = default;
    MotionStateStore(const MotionStateStore&) = delete;
    MotionStateStore& operator=(const MotionStateStore&) = delete;

    // Read-only: no boot creation. Once ready, changed/missing storage or a
    // different pairing latches failure without replacing the last good state.
    // All faults persist for this object's lifetime; there is no reset API.
    MotionLoad load(const v4::Pairing& verifiedPairing);

    // Commissioning only, after legacy journal/context, stationary and MQTT
    // ownership checks by the caller. A missing new record is NOT authorization.
    // nullptr explicitly requests a blank barrier; imported retains tombstones.
    MotionWrite installInitial(const v4::Pairing& verifiedPairing,
                               const ProductContext* imported = nullptr);
    MotionWrite saveContext(const ProductContext& context);

    // Validate the complete request before sequence/deduplication handling.
    // New accepted initialize/prepare/clean requires a nonzero 32-lowercase-hex
    // executionId and an empty slot. All other decisions require empty/null ID.
    // Duplicate identity/decision returns Unchanged without reserving an ID.
    // New rejections consume sequence even while an older execution occupies
    // the slot. Busy/ContextRequired means no decision was recorded: the caller
    // must make and persist its final rejection, never retry acceptance later.
    MotionWrite recordDecision(const ProductRequest& request, bool accepted,
                               const char* reason, const char* executionId = nullptr);

    // Replace only the matching prepare Intent with its terminal snapshot.
    // Success has empty reason/error; failure requires a reason code. Codes
    // are ASCII letters/digits/underscore, at most 64 bytes, never truncated.
    // Exact terminal repeats are Unchanged; no method here acts or resumes.
    MotionWrite finishFeeding(const char* executionId, bool completed,
                              const char* reason, const char* errorCode,
                              uint32_t uptimeMs);
    // After fresh stationary confirmation, move the frozen terminal to the
    // result queue and free the execution slot in one durable replacement.
    // No Cloud receipt is required. Never archives an unfinished Intent.
    MotionWrite archiveFeeding(bool stationary);
    // Caller must supply fresh stationary evidence, including on boot recovery.
    // Only initialize/clean Intent may be cleared; recent-result outcome is
    // changed only while that result still refers to the same request.
    MotionWrite finishOperation(const char* executionId, MotionOutcome outcome,
                                bool stationary);
    // Caller has verified Cloud's stored receipt, not just MQTT delivery.
    // Queued results already passed stationary confirmation; acknowledging one
    // does not require stopping a later task. A terminal still in the execution
    // slot requires stationary=true, preserving pre-queue BMS1 recovery.
    MotionWrite acknowledge(const char* eventId, bool completed, bool stationary);

    // Call ONLY AFTER the caller has handled immediate hardware Stop, and has
    // fresh stationary evidence. This result is NEVER permission to deny Stop,
    // including Expired/Conflict/StorageFault. Preserves the execution journal.
    // Empty/null executionId records observed idle, NEVER a wildcard. Otherwise
    // it must be valid nonzero 32-lowercase-hex. Caller already matched owner.
    // Local Stop has no NVS sequence and deliberately has no API here.
    MotionWrite recordCloudStop(uint64_t sequence, const char* commandId,
                                const char* executionId, bool accepted,
                                const char* reason, bool stationary);

    bool ready() const { return ready_ && !faulted_; }
    bool faulted() const { return faulted_; }
    // Last verified state, unchanged on any failed write; always consult ready().
    const MotionState& state() const { return state_; }

private:
    MotionLoad readStored();
    MotionLoad latchLoadFault(MotionLoad reason);
    MotionWrite latchWriteFault();
    bool checkCurrent();
    MotionWrite writeState(bool initial);

    MotionState state_;
    // Independent mutation and readback scratch: neither aliases the last good
    // state, and large records/blobs do not occupy the small MCU task stack.
    MotionState working_;
    MotionState observed_;
    uint8_t buffer_[kMotionStateMaxSize] = {};
    bool ready_ = false;
    bool faulted_ = false;
    MotionLoad faultReason_ = MotionLoad::IoError;
};

} }
