#pragma once

#include "BoardPairingRecord.h"
#include "MotionStateRecord.h"

namespace babytech { namespace boardlink {

enum class ExportRead : uint8_t {
    Ready, Missing, Corrupt, IoError, IdentityMismatch, Conflict, NotApplicable, Present
};

// A read-only capture, not installation authority. Keep this off the task stack.
struct MotionExportSnapshot {
    char deviceId[65]{};
    char physicalId[13]{};
    char challenge[33]{};
    uint64_t boot = 0;
    uint32_t capturedAtMs = 0;
    ExportRead pairStatus = ExportRead::Missing;
    ExportRead stateStatus = ExportRead::Missing;
    ExportRead legacyStatus = ExportRead::Missing;
    ExportRead legacyEvent = ExportRead::Missing;
    v4::Pairing pairing;
    MotionState state;
    ProductContext legacy;
};

constexpr size_t kMotionExportWireMaxSize = 16384;

// Parses the existing MaintenanceExport schema. Input is mutable for bounded
// ArduinoJson zero-copy parsing; output remains unchanged on failure.
bool decodeMotionExport(char* bytes, size_t length, const char* expectedDevice,
                        const char* expectedPhysical, uint64_t expectedBoot,
                        const char* expectedChallenge, MotionExportSnapshot& output);

} }
