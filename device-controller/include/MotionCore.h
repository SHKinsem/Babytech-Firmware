#pragma once
// Arduino-independent motion core: request validation, trapezoid duration and
// X-firmware CAN response decoding. No Arduino headers here so it can be unit
// tested on the host.
//
// Wire conventions (X firmware, shared by X28S/X42S):
//   position 0x36 : [0x36][sign][mag31..24][mag23..16][mag15..8][mag7..0][0x6B] -> 0.1 degree
//   target   0x33 : [0x33][sign][mag31..24][mag23..16][mag15..8][mag7..0][0x6B] -> 0.1 degree
//                   the driver's target position (manual V1.0.5 p70, "读取电机
//                   目标位置"): the target the last position command asked for,
//                   which is what a mode-0 command is relative to.
//                   Deliberately NOT 0x34 (p71, "读取电机实时设定的目标位置"):
//                   that is the real-time setpoint and may be an intermediate
//                   value of a trajectory in progress.
//   velocity 0x35 : [0x35][sign][mag15..8][mag7..0][0x6B]                       -> 0.1 RPM
//   current  0x27 : [0x27][ma15..8][ma7..0][0x6B]                              -> mA
//   flags    0x3A : [0x3A][flags][0x6B]
//   ack      <fn> : [<fn>][status][0x6B]  status 0x02 received, 0x9F completed,
//                                          0xE2 parameter error, 0xEE format error
//   move accel/decel are whole RPM/s (no x10 scaling), see buildMovePlan.
// sign is 0 = positive, 1 = negative (signed magnitude), anything else is
// malformed. Every frame ends with the fixed checksum 0x6B.

#include <math.h>
#include <stdint.h>
#include "DebugLimits.h"

namespace motion {

struct MoveRequest {
    uint8_t id;
    float angleDeg;
    float speedRpm;
    float accelRpmS;
    float decelRpmS;
    uint16_t currentMa;
};

struct Result {
    uint16_t code;
    const char* message;
};

// HTTP status codes used by the module. 202 queued, 400 invalid,
// 409 busy/not enabled, 503 CAN or feedback unavailable.
constexpr uint16_t kCodeQueued = 202;
constexpr uint16_t kCodeInvalid = 400;
constexpr uint16_t kCodeBusy = 409;
constexpr uint16_t kCodeUnavailable = 503;

// Accepted request ranges. Nothing is clipped silently; out of range is 400.
constexpr float kMinAbsAngleDeg = 0.1f;
constexpr float kMaxAbsAngleDeg = 3600.0f;
constexpr float kMinSpeedRpm = 0.1f;
constexpr float kMaxSpeedRpm = 120.0f;
constexpr float kMinAccelRpmS = 1.0f;
constexpr float kMaxAccelRpmS = 240.0f;
constexpr uint16_t kMinCurrentMa = 100;
constexpr uint16_t kMaxCurrentMa = 5000;
constexpr uint32_t kMaxExpectedDurationMs = 60000;

// Wire scaling.
constexpr int32_t kTenthsPerDeg = 10;
constexpr int32_t kTenthsPerRpm = 10;

// Same ranges expressed in the rounded wire units actually transmitted.
constexpr int32_t kMaxAngleTenths = 36000;  // +/-3600.0 degree
constexpr int32_t kMinSpeedTenths = 1;      // 0.1 RPM
constexpr int32_t kMaxSpeedTenths = 1200;   // 120.0 RPM

// The driver reports every position-like value as a sign byte plus a 32-bit
// magnitude, so a resolved target that does not fit INT32 could never be proven
// reached. Everything is computed in int64 and checked against this bound
// instead of letting a sum wrap.
constexpr int64_t kMaxTargetTenths = 0x7FFFFFFFLL;

// Legacy direction field and motion modes for position commands. Manual
// V1.0.5 p54: 00 relative to the previous input target, 01 absolute relative to
// the coordinate zero, 02 relative to the current actual position.
constexpr uint8_t kDirectionPositive = 0;
constexpr uint8_t kDirectionNegative = 1;
constexpr uint8_t kMotionModeRelativeToPriorTarget = 0;
constexpr uint8_t kMotionModeAbsolute = 1;
constexpr uint8_t kMotionModeRelativeToCurrent = 2;
constexpr uint8_t kMaxMotionMode = 2;

// Function codes of interest (first payload byte).
constexpr uint8_t kFramePosition = 0x36;
// 0x33: the driver's target position (manual V1.0.5 p70). Different from the
// real-time setpoint 0x34 (p71), which is not used as a baseline or as proof.
constexpr uint8_t kFrameTarget = 0x33;
constexpr uint8_t kFrameVelocity = 0x35;
constexpr uint8_t kFrameCurrent = 0x27;
constexpr uint8_t kFrameFlags = 0x3A;
constexpr uint8_t kFrameEnable = 0xF3;
constexpr uint8_t kFrameMove = 0xCD;
// Direct (passthrough) position: FB without a current field, CB with one.
constexpr uint8_t kFrameDirect = 0xFB;
constexpr uint8_t kFrameDirectLimit = 0xCB;
constexpr uint8_t kFrameStop = 0xFE;
constexpr uint8_t kProtocolChecksum = 0x6B;

enum class FeedbackField : uint8_t {
    None,
    Position,
    Target,
    Velocity,
    Current,
    Flags,
};

// value units: Position -> 0.1 degree, Velocity -> 0.1 RPM, Current -> mA,
// Flags -> raw flag byte. Signed fields keep their sign.
struct FeedbackSample {
    FeedbackField field = FeedbackField::None;
    int32_t value = 0;
};

// True when the payload has a supported layout: exact length, trailing
// checksum, sign byte <= 1 and a magnitude that fits in int32.
bool decodeFeedback(const uint8_t* data, uint8_t length, FeedbackSample& out);

enum class AckStatus : uint8_t {
    Received,       // command accepted by the motor firmware
    Completed,      // motor reports the operation finished
    ParameterError, // firmware rejected a parameter
    FormatError,    // firmware rejected the frame format
    UnknownError,   // any other non-zero status, treated as a fault
};

// Maps the status byte of a `<function>[status]0x6B` ack frame.
AckStatus classifyAck(uint8_t rawStatus);

// Stable identifiers for logs and the JSON "lastAck" field.
const char* ackStatusToString(AckStatus status);

// Decoded, wire-ready description of one relative position move.
struct MovePlan {
    uint8_t id = 0;
    int32_t deltaTenths = 0;    // signed travel, 0.1 degree
    uint32_t magnitudeTenths = 0;  // |travel|, 0.1 degree
    uint8_t direction = kDirectionPositive;
    uint16_t speedTenths = 0;  // 0.1 RPM
    uint16_t accelWire = 0;    // whole RPM/s (vendor wire unit, no x10)
    uint16_t decelWire = 0;    // whole RPM/s (vendor wire unit, no x10)
    uint16_t currentMa = 0;
    uint8_t motionMode = kMotionModeRelativeToCurrent;
    bool sync = false;
    uint32_t expectedDurationMs = 0;
};

// Validates `request` and fills `plan`. Angle and speed are rounded to 0.1
// before the target and the duration are computed; a value that rounds to
// zero is rejected. Returns false on any invalid input and, when not null,
// sets `errorOut` to a stable identifier (never a heap pointer).
bool buildMovePlan(const MoveRequest& request, MovePlan& plan, const char** errorOut,
                   const DebugLimits& limits = DebugLimits{});

// Trapezoid/triangular profile duration in milliseconds (rounded up) for a
// travel given in 0.1 degree and speeds in 0.1 RPM. Returns 0 when the inputs
// cannot describe a move.
uint32_t expectedDurationMs(
    uint32_t distanceTenths,
    uint32_t speedTenths,
    uint32_t accelTenthsPerSec,
    uint32_t decelTenthsPerSec);

// ---------------------------------------------------------------------------
// Direct (passthrough) position commands: 0xFB (FB) and 0xCB (CB)
//
// Manual V1.0.5 pp54-55, X firmware:
//   [addr][FB][dir][speed u16][angle u32][mode][sync][6B]                 (12)
//   [addr][CB][dir][speed u16][angle u32][mode][sync][current u16][6B]    (14)
// speed is 0.1 RPM, angle is 0.1 degree, mode 00/01/02 as above, sync 00 is
// immediate, the CB current is mA (documented 0000-1388).
//
// The driver plans this motion itself: the command has no acceleration field
// and none is invented here. The FB form carries no current field at all, so it
// cannot impose a per-command current limit; only CB can.
// ---------------------------------------------------------------------------

struct DirectPositionRequest {
    uint8_t id = 0;
    uint8_t direction = kDirectionPositive;
    uint32_t angleTenths = 0;       // magnitude, 0.1 degree
    uint8_t motionMode = kMotionModeRelativeToCurrent;
    uint16_t speedTenths = 0;       // 0.1 RPM; 0 only for a zero-travel no-op
    uint16_t currentMa = 0;         // CB only; 0 is inside the documented range
    bool withCurrentLimit = false;  // CB carries the current field, FB does not
    bool sync = false;              // immediate execution only
};

struct DirectPositionPlan {
    uint8_t id = 0;
    uint8_t direction = kDirectionPositive;
    uint32_t angleTenths = 0;
    uint8_t motionMode = kMotionModeRelativeToCurrent;
    uint16_t speedTenths = 0;
    uint16_t currentMa = 0;
    bool withCurrentLimit = false;
    int64_t signedTenths = 0;  // signed displacement / coordinate, 0.1 degree
};

// The resolved wire target of one immediate direct-position command.
struct DirectPositionResolution {
    int64_t targetTenths = 0;
    int64_t travelTenths = 0;         // |target - current position|
    uint32_t expectedDurationMs = 0;  // constant-speed estimate, 0 for a no-op
};

// Constant-speed duration estimate for a direct move: the driver's own planner
// is not modelled here, so no acceleration ramp is assumed. 0.1 degree counts at
// 0.1 RPM (1 RPM == 6 deg/s). Saturates instead of wrapping; 0 for a no-op.
uint32_t directDurationMs(uint64_t travelTenths, uint32_t speedTenths);

// Validates the parts of a direct-position command that do not depend on the
// current position: direction, motion mode, sync, the angle magnitude, the speed
// policy and (CB only) the current policy. The configured travel policy is
// applied to the RESOLVED target by resolveDirectTarget(), never to the raw
// coordinate, so it is not decided here.
bool buildDirectPositionPlan(
    const DirectPositionRequest& request, DirectPositionPlan& plan,
    const char** errorOut, const DebugLimits& limits = DebugLimits{});

// Resolves the wire target of one immediate direct-position command and applies
// the travel policy:
//   mode 0 -> priorTargetTenths + displacement  (priorTarget must be a FRESH
//             0x33 sample of the driver's target position: never the actual
//             position, never a locally remembered target, and never the 0x34 setpoint)
//   mode 1 -> the signed value is the absolute coordinate itself
//   mode 2 -> currentPositionTenths + displacement
// The travel that is bounded is |target - current position|, not the magnitude
// of the coordinate. The target must still fit the int32 feedback range. Zero
// travel is a legal no-op with any speed, including 0 (it stays supervised by
// actual feedback); a non-zero travel needs a non-zero speed and an estimated
// duration inside the configured move duration.
bool resolveDirectTarget(
    const DirectPositionPlan& plan, int64_t currentPositionTenths,
    int64_t priorTargetTenths, const DebugLimits& limits,
    DirectPositionResolution& out, const char** errorOut);

// ---------------------------------------------------------------------------
// Header-only implementation. Kept inline so the same code is used by the
// firmware and by the host unit tests without an Arduino toolchain.
// ---------------------------------------------------------------------------

// NaN and infinity are rejected everywhere. Written without the isnan/isinf
// macros so it behaves the same on the host and in the Arduino toolchain.
inline bool isFiniteNumber(float value) {
    if (value != value) return false;  // NaN
    return value <= 3.402823466e38f && value >= -3.402823466e38f;
}

inline bool decodeFeedback(
    const uint8_t* data, uint8_t length, FeedbackSample& out) {
    out = FeedbackSample{};
    if (data == nullptr || length == 0) return false;

    switch (data[0]) {
        case kFrameCurrent: {
            if (length != 4 || data[3] != kProtocolChecksum) return false;
            out.field = FeedbackField::Current;
            out.value = (static_cast<int32_t>(data[1]) << 8) | data[2];
            return true;
        }
        case kFrameVelocity: {
            if (length != 5 || data[4] != kProtocolChecksum) return false;
            if (data[1] > 1) return false;
            const int32_t magnitude =
                (static_cast<int32_t>(data[2]) << 8) | data[3];
            out.field = FeedbackField::Velocity;
            out.value = data[1] == 0 ? magnitude : -magnitude;
            return true;
        }
        // 0x36 (actual position) and 0x33 (the driver's target position, manual
        // V1.0.5 p70) share one layout: [fn][sign][mag32][6B]. They are
        // different facts and stay different fields. 0x34 (p71, the real-time
        // setpoint) is deliberately not decoded here.
        case kFramePosition:
        case kFrameTarget: {
            if (length != 7 || data[6] != kProtocolChecksum) return false;
            if (data[1] > 1) return false;
            const uint32_t magnitude =
                (static_cast<uint32_t>(data[2]) << 24) |
                (static_cast<uint32_t>(data[3]) << 16) |
                (static_cast<uint32_t>(data[4]) << 8) |
                static_cast<uint32_t>(data[5]);
            // A magnitude above INT32_MAX cannot be represented as feedback.
            if (magnitude > 0x7FFFFFFFu) return false;
            const int32_t signedValue = static_cast<int32_t>(magnitude);
            out.field = data[0] == kFrameTarget ? FeedbackField::Target
                                                : FeedbackField::Position;
            out.value = data[1] == 0 ? signedValue : -signedValue;
            return true;
        }
        case kFrameFlags: {
            if (length != 3 || data[2] != kProtocolChecksum) return false;
            out.field = FeedbackField::Flags;
            out.value = data[1];
            return true;
        }
        default:
            return false;
    }
}

// Status bytes are taken verbatim from the authoritative X firmware codec
// (motion/lib/XMotor/src/x_firmware_can_codec.h):
//   0x02 received, 0x9F completed, 0xE2 parameter error, 0xEE format error.
inline AckStatus classifyAck(uint8_t rawStatus) {
    switch (rawStatus) {
        case 0x02: return AckStatus::Received;
        case 0x9F: return AckStatus::Completed;
        case 0xE2: return AckStatus::ParameterError;
        case 0xEE: return AckStatus::FormatError;
        default: return AckStatus::UnknownError;
    }
}

inline const char* ackStatusToString(AckStatus status) {
    switch (status) {
        case AckStatus::Received: return "received";
        case AckStatus::Completed: return "completed";
        case AckStatus::ParameterError: return "param_error";
        case AckStatus::FormatError: return "format_error";
        case AckStatus::UnknownError: return "unknown_error";
    }
    return "unknown_error";
}

inline uint32_t expectedDurationMs(
    uint32_t distanceTenths,
    uint32_t speedTenths,
    uint32_t accelRpmPerSec,
    uint32_t decelRpmPerSec) {
    if (distanceTenths == 0 || speedTenths == 0 ||
        accelRpmPerSec == 0 || decelRpmPerSec == 0) {
        return 0;
    }
    // distance is 0.1 degree, speed is 0.1 RPM and acceleration is whole RPM/s
    // exactly as the vendor position command carries it. 1 RPM == 6 deg/s.
    const double distance = static_cast<double>(distanceTenths) / 10.0;
    const double speed = static_cast<double>(speedTenths) / 10.0 * 6.0;  // deg/s
    const double accel = static_cast<double>(accelRpmPerSec) * 6.0;  // deg/s^2
    const double decel = static_cast<double>(decelRpmPerSec) * 6.0;  // deg/s^2

    const double accelDistance = speed * speed / (2.0 * accel);
    const double decelDistance = speed * speed / (2.0 * decel);

    double seconds = 0.0;
    if (accelDistance + decelDistance <= distance) {
        seconds = speed / accel + speed / decel +
            (distance - accelDistance - decelDistance) / speed;
    } else {
        // Triangular profile: the cruise phase never happens.
        const double peak = sqrt(2.0 * distance * accel * decel / (accel + decel));
        seconds = peak / accel + peak / decel;
    }

    if (!(seconds > 0.0) || seconds > 100000.0) return 0;
    return static_cast<uint32_t>(ceil(seconds * 1000.0));
}

inline bool buildMovePlan(
    const MoveRequest& request, MovePlan& plan, const char** errorOut, const DebugLimits& limits) {
    const auto fail = [errorOut](const char* reason) -> bool {
        if (errorOut != nullptr) *errorOut = reason;
        return false;
    };
    if (errorOut != nullptr) *errorOut = nullptr;

    if (request.id == 0) return fail("id_reserved");
    if (!validDebugLimits(limits)) return fail("limits_invalid");

    if (!isFiniteNumber(request.angleDeg)) {
        return fail("angle_not_finite");
    }
    // Bound before conversion so an arbitrary HTTP float cannot overflow int32.
    if (fabsf(request.angleDeg) > limits.maxAngleTenths / 10.0f)
        return fail("angle_out_of_range");
    const int32_t deltaTenths =
        static_cast<int32_t>(lroundf(request.angleDeg * kTenthsPerDeg));
    if (deltaTenths == 0) return fail("angle_rounds_to_zero");
    if (deltaTenths > static_cast<int32_t>(limits.maxAngleTenths) || deltaTenths < -static_cast<int32_t>(limits.maxAngleTenths)) {
        return fail("angle_out_of_range");
    }

    if (!isFiniteNumber(request.speedRpm)) {
        return fail("speed_not_finite");
    }
    if (request.speedRpm > limits.maxSpeedTenths / 10.0f || request.speedRpm < kMinSpeedRpm)
        return fail("speed_out_of_range");
    const int32_t speedTenths =
        static_cast<int32_t>(lroundf(request.speedRpm * kTenthsPerRpm));
    if (speedTenths == 0) return fail("speed_rounds_to_zero");
    if (speedTenths < kMinSpeedTenths || speedTenths > static_cast<int32_t>(limits.maxSpeedTenths)) {
        return fail("speed_out_of_range");
    }

    if (!isFiniteNumber(request.accelRpmS)) {
        return fail("accel_not_finite");
    }
    if (request.accelRpmS < kMinAccelRpmS || request.accelRpmS > limits.maxAccelRpmS) {
        return fail("accel_out_of_range");
    }
    if (floorf(request.accelRpmS) != request.accelRpmS) {
        return fail("accel_not_integer");
    }

    if (!isFiniteNumber(request.decelRpmS)) {
        return fail("decel_not_finite");
    }
    if (request.decelRpmS < kMinAccelRpmS || request.decelRpmS > limits.maxAccelRpmS) {
        return fail("decel_out_of_range");
    }
    if (floorf(request.decelRpmS) != request.decelRpmS) {
        return fail("decel_not_integer");
    }

    if (request.currentMa < kMinCurrentMa || request.currentMa > limits.maxCurrentMa) {
        return fail("current_out_of_range");
    }

    const uint32_t magnitudeTenths = static_cast<uint32_t>(
        deltaTenths < 0 ? -deltaTenths : deltaTenths);
    // The vendor position command (0xCD) carries acceleration/deceleration as
    // whole RPM/s. The request value is already an integer here and is sent as
    // is: no x10 conversion on the wire.
    const uint16_t accelWire = static_cast<uint16_t>(request.accelRpmS);
    const uint16_t decelWire = static_cast<uint16_t>(request.decelRpmS);

    const uint32_t duration = expectedDurationMs(
        magnitudeTenths, static_cast<uint32_t>(speedTenths), accelWire, decelWire);
    if (duration == 0 || duration > limits.maxMoveDurationMs) {
        return fail("duration_too_long");
    }

    plan = MovePlan{};
    plan.id = request.id;
    plan.deltaTenths = deltaTenths;
    plan.magnitudeTenths = magnitudeTenths;
    plan.direction =
        deltaTenths < 0 ? kDirectionNegative : kDirectionPositive;
    plan.speedTenths = static_cast<uint16_t>(speedTenths);
    plan.accelWire = accelWire;
    plan.decelWire = decelWire;
    plan.currentMa = request.currentMa;
    plan.motionMode = kMotionModeRelativeToCurrent;
    plan.sync = false;
    plan.expectedDurationMs = duration;
    return true;
}

inline uint32_t directDurationMs(uint64_t travelTenths, uint32_t speedTenths) {
    if (travelTenths == 0) return 0;  // zero travel: a no-op has no duration
    if (speedTenths == 0) return 0;   // callers reject this pair before asking
    // 0.1 degree counts at 0.1 RPM, 1 RPM == 6 deg/s, no acceleration ramp: the
    // driver plans the profile, this is only a bounded sanity estimate.
    const double seconds = static_cast<double>(travelTenths) /
        (static_cast<double>(speedTenths) * 6.0);
    const double milliseconds = seconds * 1000.0;
    if (!(milliseconds > 0.0)) return 0;
    if (milliseconds >= 4294967295.0) return 0xFFFFFFFFu;  // saturate
    return static_cast<uint32_t>(ceil(milliseconds));
}

inline bool buildDirectPositionPlan(
    const DirectPositionRequest& request, DirectPositionPlan& plan,
    const char** errorOut, const DebugLimits& limits) {
    const auto fail = [errorOut](const char* reason) -> bool {
        if (errorOut != nullptr) *errorOut = reason;
        return false;
    };
    if (errorOut != nullptr) *errorOut = nullptr;

    if (!validDebugLimits(limits)) return fail("limits_invalid");
    if (request.id == 0) return fail("id_reserved");
    // Only the immediate form is supervised: the cached form still needs the FF
    // trigger, which this board does not supervise.
    if (request.sync) return fail("direct_sync_not_supported");
    if (request.direction > kDirectionNegative) return fail("direction_invalid");
    if (request.motionMode > kMaxMotionMode) return fail("motion_mode_invalid");

    // The angle field is a uint32 on the wire, but every resolved target has to
    // stay representable in the int32 the driver reports back, so the magnitude
    // is capped there before any arithmetic happens.
    if (static_cast<int64_t>(request.angleTenths) > kMaxTargetTenths) {
        return fail("angle_out_of_range");
    }
    // Modes 0 and 2 carry a displacement, so the configured travel policy bounds
    // the commanded value itself. Mode 1 carries an absolute coordinate: its real
    // travel is measured against the current position when the target is
    // resolved, never against the coordinate magnitude.
    if (request.motionMode != kMotionModeAbsolute &&
        request.angleTenths > limits.maxAngleTenths) {
        return fail("angle_out_of_range");
    }

    if (request.speedTenths > limits.maxSpeedTenths) return fail("speed_out_of_range");
    // CB only: the documented current range is 0000-1388 (0-5000 mA) subject to
    // the configured policy. No floor is invented - 0 is a documented value -
    // and FB cannot carry a current limit at all.
    if (request.withCurrentLimit && request.currentMa > limits.maxCurrentMa) {
        return fail("current_out_of_range");
    }

    plan = DirectPositionPlan{};
    plan.id = request.id;
    plan.direction = request.direction;
    plan.angleTenths = request.angleTenths;
    plan.motionMode = request.motionMode;
    plan.speedTenths = request.speedTenths;
    plan.currentMa = request.withCurrentLimit ? request.currentMa : 0;
    plan.withCurrentLimit = request.withCurrentLimit;
    plan.signedTenths = request.direction == kDirectionNegative
        ? -static_cast<int64_t>(request.angleTenths)
        : static_cast<int64_t>(request.angleTenths);
    return true;
}

inline bool resolveDirectTarget(
    const DirectPositionPlan& plan, int64_t currentPositionTenths,
    int64_t priorTargetTenths, const DebugLimits& limits,
    DirectPositionResolution& out, const char** errorOut) {
    const auto fail = [errorOut](const char* reason) -> bool {
        if (errorOut != nullptr) *errorOut = reason;
        return false;
    };
    if (errorOut != nullptr) *errorOut = nullptr;
    out = DirectPositionResolution{};

    if (!validDebugLimits(limits)) return fail("limits_invalid");
    if (plan.motionMode > kMaxMotionMode) return fail("motion_mode_invalid");

    // Signed magnitude resolution in int64: an int32 position plus a delta up to
    // INT32_MAX cannot overflow here, and the result is range checked below.
    int64_t target = plan.signedTenths;
    if (plan.motionMode == kMotionModeRelativeToPriorTarget) {
        target += priorTargetTenths;
    } else if (plan.motionMode == kMotionModeRelativeToCurrent) {
        target += currentPositionTenths;
    }
    if (target > kMaxTargetTenths || target < -kMaxTargetTenths) {
        return fail("target_out_of_range");
    }

    // The travel policy is about the distance actually commanded from where the
    // motor is now, not about the coordinate the target happens to name.
    int64_t travel = target - currentPositionTenths;
    if (travel < 0) travel = -travel;
    if (travel > static_cast<int64_t>(limits.maxAngleTenths)) {
        return fail("travel_out_of_range");
    }

    // Zero travel is a supervised no-op: it needs no motion and may carry any
    // speed, including 0. Anything else needs a real speed to be timed.
    if (travel != 0 && plan.speedTenths == 0) return fail("speed_required");

    const uint32_t duration =
        directDurationMs(static_cast<uint64_t>(travel), plan.speedTenths);
    if (travel != 0 && duration > limits.maxMoveDurationMs) {
        return fail("duration_too_long");
    }

    out.targetTenths = target;
    out.travelTenths = travel;
    out.expectedDurationMs = travel == 0 ? 0 : duration;
    return true;
}

}  // namespace motion
