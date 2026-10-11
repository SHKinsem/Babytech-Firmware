#pragma once

#include "BoardPairingRecord.h"

namespace babytech { namespace boardlink {

// Ready means identity was read and matched, not that migration or motion is authorized.
enum class PairingLoad { Ready, Missing, Corrupt, IoError, IdentityMismatch, UartError };
// Read-only physical identity check, including before a record exists.
PairingLoad verifyBoardPairing(v4::Role role, const v4::Pairing& pairing);
PairingLoad loadBoardPairing(v4::Role role, v4::Pairing& output);

enum class PairingInstall { Installed, AlreadyInstalled, Invalid, IdentityMismatch,
                            Conflict, StorageFault };

// Storage primitive, NOT a migration/activation endpoint. A controlled owner
// must first settle legacy events, import context barriers and isolate MQTT
// credentials. Never call from handshake, normal boot, MQTT or running motion.
// One owner per MCU; no concurrent record writers or identity replacement.
class PairingInstaller {
public:
    PairingInstall installFirst(v4::Role role, const v4::Pairing& pairing);
    bool faulted() const { return faulted_; }
private:
    // Write/commit/readback uncertainty is sticky for this commissioning run.
    bool faulted_ = false;
};

} }
