#pragma once

#include "BrainStateRecord.h"

namespace babytech { namespace boardlink {

enum class BrainLoad { Ready, Missing, Corrupt, IoError, IdentityMismatch };
enum class BrainWrite { Stored, Unchanged, Invalid, Conflict, Busy, Exhausted,
                        ContextRequired, StorageFault };

// Single owner only; not a concurrent transaction or a remote entry point.
// The caller must supply pairing already verified against actual hardware.
// Storage readiness does not authorize mechanical actions or commissioning.
// ready()/Stored describe durable state, never permission to send an old-boot
// intent. A pending request recovered by load() may only be queried, not sent
// again. There is no automatic replay API.
class BrainStateStore {
public:
    BrainStateStore() = default;
    BrainStateStore(const BrainStateStore&) = delete;
    BrainStateStore& operator=(const BrainStateStore&) = delete;

    // Read-only, including on Missing. Once ready, only identical state may be
    // reloaded. Missing/changed records latch a fault and preserve the last RAM
    // state; a different expected pairing also faults. No reload-based rollback
    // or reinitialization. Recovery after an actual restart requires a new
    // commissioning object and external gates, never an API reset of this one.
    // Faults remain latched for this lifetime.
    BrainLoad load(const v4::Pairing& verifiedPairing);

    // Read-only commissioning evidence, not verified identity or readiness.
    // The coordinator must check actual hardware identities and full pairing.
    // Ready copies the complete decoded record; Missing/failure preserve output.
    // Never changes state_/ready_ or clears faults. Before ready, Missing is not
    // a fault; after ready, external changes latch faults just as load() does.
    BrainLoad inspectForCommissioning(BrainState& output);

    // Commissioning ONLY: the caller must first validate legacy journal/context,
    // stationary and MQTT ownership gates. Missing alone is not authorization.
    // nullptr explicitly requests a new blank cache; imported retains tombstones.
    BrainWrite installInitial(const v4::Pairing& verifiedPairing,
                              const ProductContext* imported = nullptr);
    BrainWrite saveContext(const ProductContext& context);

    // Caller supplies the complete request: next sequence and makeLocalCommandId.
    // Wrong identity/source/sequence/ID is Invalid. Prepare needs the matching
    // active cache (ContextRequired otherwise); valid water/temp may override it.
    // Stored permits the caller to send this newly reserved request once in the
    // current boot, subject to all external freshness and mechanical gates.
    BrainWrite reserveLocal(const ProductRequest& request);

    // Caller must have a definitive Motion COMMAND_RESULT or request-result
    // query response. UART write success or a timeout is not resolution. This
    // does not wait for a feeding terminal event or its Cloud stored receipt.
    // No pending request or a different request/digest returns Conflict.
    BrainWrite clearPending(const ProductRequest& request);

    bool ready() const { return ready_ && !faulted_; }
    bool faulted() const { return faulted_; }
    // Last successfully verified state, unchanged on failure; consult ready().
    const BrainState& state() const { return state_; }

private:
    BrainLoad readStored();
    BrainLoad latchLoadFault(BrainLoad reason);
    BrainWrite latchWriteFault();
    bool checkCurrent();
    BrainWrite writeState(const BrainState& next, bool initial);

    BrainState state_;
    // Decode scratch and the bounded blob live off the small MCU task stack.
    BrainState observed_;
    uint8_t buffer_[kBrainStateMaxSize] = {};
    bool ready_ = false;
    bool faulted_ = false;
    BrainLoad faultReason_ = BrainLoad::IoError;
};

} }
