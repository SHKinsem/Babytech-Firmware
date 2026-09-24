#pragma once
// Arduino-independent motion core: request validation, trapezoid duration and
// X-firmware CAN response decoding. No Arduino headers here so it can be unit
// tested on the host.
//
// Wire conventions (X firmware, shared by X28S/X42S):
//   position 0x36 : [0x36][sign][mag31..24][mag23..16][mag15..8][mag7..0][0x6B] -> 0.1 degree
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

// Legacy direction field and motion mode for position commands.
constexpr uint8_t kDirectionPositive = 0;
constexpr uint8_t kDirectionNegative = 1;
constexpr uint8_t kMotionModeRelativeToCurrent = 2;

// Function codes of interest (first payload byte).
constexpr uint8_t kFramePosition = 0x36;
constexpr uint8_t kFrameVelocity = 0x35;
constexpr uint8_t kFrameCurrent = 0x27;
constexpr uint8_t kFrameFlags = 0x3A;
constexpr uint8_t kFrameEnable = 0xF3;
constexpr uint8_t kFrameMove = 0xCD;
constexpr uint8_t kFrameStop = 0xFE;
constexpr uint8_t kProtocolChecksum = 0x6B;

enum class FeedbackField : uint8_t {
    None,
    Position,
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
bool buildMovePlan(const MoveRequest& request, MovePlan& plan, const char** errorOut);

// Trapezoid/triangular profile duration in milliseconds (rounded up) for a
// travel given in 0.1 degree and speeds in 0.1 RPM. Returns 0 when the inputs
// cannot describe a move.
uint32_t expectedDurationMs(
    uint32_t distanceTenths,
    uint32_t speedTenths,
    uint32_t accelTenthsPerSec,
    uint32_t decelTenthsPerSec);

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
        case kFramePosition: {
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
            out.field = FeedbackField::Position;
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
// (device-controller/lib/XMotor/src/x_firmware_can_codec.h):
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
    const MoveRequest& request, MovePlan& plan, const char** errorOut) {
    const auto fail = [errorOut](const char* reason) -> bool {
        if (errorOut != nullptr) *errorOut = reason;
        return false;
    };
    if (errorOut != nullptr) *errorOut = nullptr;

    if (request.id == 0) return fail("id_reserved");

    if (!isFiniteNumber(request.angleDeg)) {
        return fail("angle_not_finite");
    }
    // Round first, then judge the value that will actually be sent.
    const int32_t deltaTenths =
        static_cast<int32_t>(lroundf(request.angleDeg * kTenthsPerDeg));
    if (deltaTenths == 0) return fail("angle_rounds_to_zero");
    if (deltaTenths > kMaxAngleTenths || deltaTenths < -kMaxAngleTenths) {
        return fail("angle_out_of_range");
    }

    if (!isFiniteNumber(request.speedRpm)) {
        return fail("speed_not_finite");
    }
    const int32_t speedTenths =
        static_cast<int32_t>(lroundf(request.speedRpm * kTenthsPerRpm));
    if (speedTenths == 0) return fail("speed_rounds_to_zero");
    if (speedTenths < kMinSpeedTenths || speedTenths > kMaxSpeedTenths) {
        return fail("speed_out_of_range");
    }

    if (!isFiniteNumber(request.accelRpmS)) {
        return fail("accel_not_finite");
    }
    if (request.accelRpmS < kMinAccelRpmS || request.accelRpmS > kMaxAccelRpmS) {
        return fail("accel_out_of_range");
    }
    if (floorf(request.accelRpmS) != request.accelRpmS) {
        return fail("accel_not_integer");
    }

    if (!isFiniteNumber(request.decelRpmS)) {
        return fail("decel_not_finite");
    }
    if (request.decelRpmS < kMinAccelRpmS || request.decelRpmS > kMaxAccelRpmS) {
        return fail("decel_out_of_range");
    }
    if (floorf(request.decelRpmS) != request.decelRpmS) {
        return fail("decel_not_integer");
    }

    if (request.currentMa < kMinCurrentMa || request.currentMa > kMaxCurrentMa) {
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
    if (duration == 0 || duration > kMaxExpectedDurationMs) {
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

}  // namespace motion
