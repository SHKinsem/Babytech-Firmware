#pragma once

#include "BoardPairingStore.h"
#include "BrainStateStore.h"
#include "MotionStateStore.h"

namespace babytech { namespace boardlink {

struct CommissioningImport {
    v4::Pairing pairing;
    bool hasContext = false;
    ProductContext context;
};

enum class CommissioningResult {
    Installed, AlreadyInstalled, Invalid, IdentityMismatch, Conflict,
    Unsafe, LegacyPending, StorageFault, StateMissing
};

// Runtime owner must hold exclusive maintenance ownership, block commands and
// debug mutations, verify fresh stationary feedback and explicit local operator
// authorization, and confirm exclusive product MQTT credential handoff. Brain
// additionally needs the verified Motion export matching this exact request,
// including the context's full digest (not just the device or profile version).
// Resuming a partial migration also requires peer evidence: a locally unused
// record cannot prove the peer has not consumed sequences since that snapshot.
// Called again before each durable phase; not a cached boolean from startup.
class CommissioningGuard {
public:
    virtual ~CommissioningGuard() = default;
    virtual bool allowImport(const CommissioningImport& request) = 0;
};

// Controlled first migration only, never ordinary boot, handshake or MQTT.
// Writes state before productpair; partial writes are resumable only with the
// identical pairing/context and unused watermarks. Never erases old keys.
// A full restored/erased NVS cannot prove its own history: replacement/recovery
// still requires external evidence and is NOT implemented by these methods.
// Single owner, persistent object for the entire commissioning session. Guard
// owns the maintenance lock until return; there is no cross-key transaction.
// Unsafe can follow a successful state write; it does NOT imply zero mutation.
class BoardCommissioning {
public:
    CommissioningResult importBrain(const CommissioningImport& request,
                                    BrainStateStore& store, CommissioningGuard& guard);
    CommissioningResult importMotion(const CommissioningImport& request,
                                     MotionStateStore& store, CommissioningGuard& guard);
    bool faulted() const { return faulted_; }
private:
    CommissioningResult inspectPair(const CommissioningImport& request, v4::Role role,
                                   bool& alreadyPaired);
    CommissioningResult inspectLegacyMotion(const CommissioningImport& request);
    CommissioningResult finishPair(const CommissioningImport& request, bool stateWritten,
                                  CommissioningGuard& guard);
    CommissioningResult fault();
    PairingInstaller installer_;
    bool faulted_ = false;
};

} }
