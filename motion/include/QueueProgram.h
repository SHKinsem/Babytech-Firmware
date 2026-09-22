#pragma once
// Board queue DSL: the resolved plan plus strict parsing/validation.
//
// One action per line, '#' starts a comment, commands and units are
// case-insensitive. Numbers are parsed strictly (signed decimal, finite, integer
// exactly where the contract says integer, no trailing garbage, no expressions,
// no eval). Nothing here allocates and nothing here talks to hardware: the plan
// is a fixed 64-step array that stays immutable until the run is cancelled or
// finished.
//
// The whole program is validated BEFORE any CAN frame is sent, so an invalid
// line 2 can never let a valid line 1 run. The implementation lives in
// CommandQueue.cpp so the host suite needs one extra source file, not two.
//
// Arduino-independent on purpose: the host tests compile this header directly.

#include <stddef.h>
#include <stdint.h>

#include "DebugLimits.h"

namespace motion {

constexpr uint8_t kQueueMaxSteps = 64;         // non-blank actions per program
constexpr size_t kQueueMaxTextBytes = 8192;    // whole program text
constexpr uint32_t kQueueMaxRepeat = 1000;
constexpr uint32_t kQueueMaxWaitMs = 3600000;
constexpr uint32_t kQueueMaxDurationMs = 3600000;
constexpr uint8_t kQueueMaxRawBytes = 30;      // logical X command
constexpr uint8_t kQueueMaxCanBytes = 8;
constexpr double kQueueMinRotationMm = 0.000001;
constexpr double kQueueMaxRotationMm = 1000000.0;

enum class QueueAction : uint8_t {
    None = 0,
    Enable,
    Disable,
    Move,
    Home,
    Torque,
    Velocity,
    Stop,
    Wait,
    Hex,
    Can,
};

// Stable identifiers for the status JSON and error text.
const char* queueActionName(QueueAction action);

// Per-ID rotation distance source. The queue resolves mm steps at validation
// time from the values stored on the board and never falls back to a default.
class QueueRotationSource {
public:
    virtual ~QueueRotationSource() {}
    // True when id has a stored rotation distance; out is mm per revolution.
    virtual bool rotationMm(uint8_t id, double& out) const = 0;
};

struct QueueError {
    uint16_t line = 0;               // 1-based source line, 0 when not line bound
    const char* message = "invalid"; // stable identifier
};

struct QueueStep {
    QueueAction action = QueueAction::None;
    uint16_t line = 0;
    uint8_t id = 0;

    // move: resolved at validation, 0.1 degree, the sign carries the direction.
    int32_t distanceTenths = 0;
    uint16_t speedTenths = 0;        // 0.1 RPM
    uint16_t accelRpmS = 0;
    uint16_t decelRpmS = 0;
    uint16_t currentMa = 0;

    // home
    uint8_t mode = 0;

    // timed torque (C5) / velocity (C6)
    int32_t torqueMa = 0;            // signed, |value| is transmitted
    int32_t velocityTenths = 0;      // signed 0.1 RPM
    uint16_t maxSpeedTenths = 0;     // C5 maximum speed
    uint16_t rampMaPerSec = 0;       // C5 slope
    uint16_t timedAccel = 0;         // C6 acceleration (whole RPM/s)
    uint16_t timedCurrentMa = 0;     // C6 current limit
    uint32_t durationMs = 0;

    // wait
    uint32_t waitMs = 0;

    // hex: raw logical X command bytes, exactly as written (3..30).
    uint8_t raw[kQueueMaxRawBytes] = {};
    uint8_t rawLength = 0;

    // can: one actual CAN data frame.
    uint32_t canId = 0;
    bool extended = false;
    uint8_t canData[kQueueMaxCanBytes] = {};
    uint8_t canLength = 0;
};

struct QueueProgram {
    QueueStep steps[kQueueMaxSteps];
    uint8_t count = 0;
    bool hasRaw = false;             // any hex/can step in the program
};

/**
 * Parses and validates a whole program.
 *
 * `limits` is the confirmed DebugLimits the run will execute under. `rotation`
 * resolves mm steps; a move in mm for an id without a stored rotation distance
 * is rejected instead of guessed. On failure `program` is cleared and `error`
 * carries the stable reason plus the offending 1-based source line.
 */
bool parseQueueProgram(const char* text, size_t length, const DebugLimits& limits,
                       const QueueRotationSource& rotation, QueueProgram& program,
                       QueueError& error);

}  // namespace motion
