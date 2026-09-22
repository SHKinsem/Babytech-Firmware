#include "CommandQueue.h"

#include <string.h>

namespace motion {
namespace {

// --- Limits of the executor -------------------------------------------------
// Queue watchdogs for the two steps the controller does not bound itself. An
// enable is additionally bounded by the controller's own ack timeout, which is
// shorter, so its verdict is reported first.
constexpr uint32_t kEnableWaitMs = 5000;
constexpr uint32_t kStopWaitMs = 5000;
// Raw steps keep at least this gap, as the contract requires.
constexpr uint32_t kRawSpacingMs = 2;
// Same "approximately stopped" band the controller uses (0.1 RPM units).
constexpr int32_t kQueueStopTenths = 5;
// Queue bounds for the phases the controller does not bound itself: waiting for
// a supervised step's target evidence and for post-answer evidence (12/22).
// Both are bounded so a silent node can never keep a run alive forever.
constexpr uint32_t kEvidenceWaitMs = 3000;
// A supervised motion step only dispatches on evidence the controller itself
// demands (fresh, approximately stationary feedback of its own target).
bool requiresStationaryBeforeDispatch(QueueAction action) {
    return action == QueueAction::Move || action == QueueAction::Home ||
           action == QueueAction::Torque || action == QueueAction::Velocity;
}
// Default arguments of the readable DSL.
constexpr double kDefaultMoveRpm = 30.0;
constexpr long kDefaultMoveAccel = 60;
constexpr long kDefaultMoveDecel = 60;
constexpr long kDefaultMoveCurrent = 800;
constexpr long kDefaultTorqueMaxRpm = 30;
constexpr long kDefaultTorqueRamp = 1000;
constexpr long kDefaultVelocityAccel = 60;
constexpr long kDefaultVelocityCurrent = 800;

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

bool tokenEquals(const char* text, size_t length, const char* lower) {
    size_t i = 0;
    for (; i < length && lower[i]; ++i) {
        if (lowerAscii(text[i]) != lower[i]) return false;
    }
    return i == length && lower[i] == '\0';
}

struct Tokens {
    // Longest line: "hex" plus 30 byte tokens (31), with headroom. The length is
    // size_t on purpose: a uint8_t would silently truncate a long token and a
    // truncated token could then parse as valid garbage.
    static const uint8_t kMax = 40;
    const char* text[kMax] = {};
    size_t length[kMax] = {};
    uint8_t count = 0;
};

// Splits one line into whitespace separated tokens. The pointers stay inside the
// caller's buffer, which outlives every use here.
bool tokenize(const char* line, size_t length, Tokens& out) {
    size_t i = 0;
    while (i < length) {
        while (i < length && isSpace(line[i])) ++i;
        if (i >= length) break;
        if (out.count >= Tokens::kMax) return false;
        const size_t start = i;
        while (i < length && !isSpace(line[i])) ++i;
        out.text[out.count] = line + start;
        out.length[out.count] = i - start;
        ++out.count;
    }
    return true;
}

// Strict unsigned integer literal: digits only, no sign, no dot, no exponent.
bool parseUnsignedInteger(const char* text, size_t length, long& out) {
    if (length == 0 || length > 9) return false;
    long value = 0;
    for (size_t i = 0; i < length; ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') return false;
        value = value * 10 + long(c - '0');
        if (value > 1000000000L) return false;
    }
    out = value;
    return true;
}

// Strict signed integer literal: an optional '-' (never '+') and digits only, so
// "1.0" and "1e3" are not integers here.
bool parseSignedInteger(const char* text, size_t length, long& out) {
    if (length == 0) return false;
    const bool negative = text[0] == '-';
    const size_t offset = negative ? 1 : 0;
    long magnitude = 0;
    if (!parseUnsignedInteger(text + offset, length - offset, magnitude)) return false;
    out = negative ? -magnitude : magnitude;
    return true;
}

// Strict signed decimal: an optional '-', digits and at most one '.', no sign
// other than '-', no exponent. Bounded so the value is always finite and exactly
// representable.
bool parseSignedDecimal(const char* text, size_t length, double& out) {
    if (length == 0 || length > 24) return false;
    size_t i = 0;
    const bool negative = text[0] == '-';
    if (negative) i = 1;
    if (i >= length) return false;
    bool anyDigit = false, dot = false;
    double value = 0.0, scale = 1.0;
    for (; i < length; ++i) {
        const char c = text[i];
        if (c == '.') {
            if (dot) return false;
            dot = true;
            continue;
        }
        if (c < '0' || c > '9') return false;
        anyDigit = true;
        if (!dot) {
            value = value * 10.0 + double(c - '0');
        } else {
            scale *= 0.1;
            value += double(c - '0') * scale;
        }
        if (value > 1.0e9) return false;
    }
    if (!anyDigit) return false;
    out = negative ? -value : value;
    return true;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "0x1F" style identifier for the can step. At most eight hex digits AFTER the
// optional prefix, so the 32-bit accumulator can never wrap around: an id of
// "100000000" is rejected instead of silently becoming 0.
bool parseHexId(const char* text, size_t length, uint32_t& out) {
    size_t i = 0;
    if (length > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) i = 2;
    const size_t digits = length - i;
    if (digits == 0 || digits > 8) return false;
    uint32_t value = 0;
    for (; i < length; ++i) {
        const int digit = hexDigit(text[i]);
        if (digit < 0) return false;
        value = (value << 4) | uint32_t(digit);
    }
    out = value;
    return true;
}

// One or more two-digit hex bytes.
bool parseHexBytes(const char* text, size_t length, uint8_t* out, uint8_t capacity, uint8_t& count) {
    if (length == 0 || (length % 2) != 0 || length / 2 > capacity) return false;
    uint8_t index = 0;
    for (size_t i = 0; i < length; i += 2) {
        const int high = hexDigit(text[i]);
        const int low = hexDigit(text[i + 1]);
        if (high < 0 || low < 0) return false;
        out[index++] = static_cast<uint8_t>((high << 4) | low);
    }
    count = index;
    return true;
}

QueueAction actionFromToken(const char* text, size_t length) {
    if (tokenEquals(text, length, "enable")) return QueueAction::Enable;
    if (tokenEquals(text, length, "disable")) return QueueAction::Disable;
    if (tokenEquals(text, length, "move")) return QueueAction::Move;
    if (tokenEquals(text, length, "home")) return QueueAction::Home;
    if (tokenEquals(text, length, "torque")) return QueueAction::Torque;
    if (tokenEquals(text, length, "velocity")) return QueueAction::Velocity;
    if (tokenEquals(text, length, "stop")) return QueueAction::Stop;
    if (tokenEquals(text, length, "wait")) return QueueAction::Wait;
    if (tokenEquals(text, length, "hex")) return QueueAction::Hex;
    if (tokenEquals(text, length, "can")) return QueueAction::Can;
    return QueueAction::None;
}

bool failAt(QueueError& error, uint16_t line, const char* message) {
    error.line = line;
    error.message = message;
    return false;
}

// Shared duration policy: a bounded integer within the DSL limit, and when the
// board policy sets an experiment window it is an upper bound (reject, never
// silently stop early).
bool validateDuration(long durationMs, const DebugLimits& limits, QueueError& error, uint16_t line) {
    if (durationMs < 1 || durationMs > long(kQueueMaxDurationMs)) {
        return failAt(error, line, "timed_duration_out_of_range");
    }
    if (limits.experimentDurationMs != 0 &&
        uint32_t(durationMs) > limits.experimentDurationMs) {
        return failAt(error, line, "timed_duration_exceeds_policy");
    }
    return true;
}

}  // namespace

const char* queueActionName(QueueAction action) {
    switch (action) {
        case QueueAction::Enable: return "enable";
        case QueueAction::Disable: return "disable";
        case QueueAction::Move: return "move";
        case QueueAction::Home: return "home";
        case QueueAction::Torque: return "torque";
        case QueueAction::Velocity: return "velocity";
        case QueueAction::Stop: return "stop";
        case QueueAction::Wait: return "wait";
        case QueueAction::Hex: return "hex";
        case QueueAction::Can: return "can";
        case QueueAction::None: break;
    }
    return "none";
}

const char* queueStateName(QueueState state) {
    switch (state) {
        case QueueState::Idle: return "idle";
        case QueueState::Running: return "running";
        case QueueState::Done: return "done";
        case QueueState::Failed: return "failed";
        case QueueState::Cancelled: return "cancelled";
    }
    return "idle";
}

// ---------------------------------------------------------------------------
// Program parsing and validation
// ---------------------------------------------------------------------------

namespace {

bool parseId(const Tokens& tokens, uint8_t index, uint8_t& id, QueueError& error, uint16_t line) {
    long value = 0;
    // A motor address is a plain integer: "1.0" or "+1" is not accepted, exactly
    // like the frontend's own field.
    if (!parseUnsignedInteger(tokens.text[index], tokens.length[index], value)) {
        return failAt(error, line, "invalid_integer");
    }
    if (value < 1 || value > 255) return failAt(error, line, "id_out_of_range");
    id = static_cast<uint8_t>(value);
    return true;
}

bool parseMoveStep(const Tokens& tokens, const DebugLimits& limits,
                   const QueueRotationSource& rotation, QueueStep& step, QueueError& error) {
    // move ID VALUE [deg|rev|mm] [RPM [ACCEL [DECEL [CURRENT]]]] is eight tokens
    // in its longest form.
    if (tokens.count < 3 || tokens.count > 8) return failAt(error, step.line, "argument_count");
    if (!parseId(tokens, 1, step.id, error, step.line)) return false;

    double distance = 0.0;
    if (!parseSignedDecimal(tokens.text[2], tokens.length[2], distance)) {
        return failAt(error, step.line, "invalid_number");
    }
    size_t index = 3;
    uint8_t unit = 0;  // 0 deg, 1 rev, 2 mm
    if (index < tokens.count) {
        if (tokenEquals(tokens.text[index], tokens.length[index], "deg")) { unit = 0; ++index; }
        else if (tokenEquals(tokens.text[index], tokens.length[index], "rev")) { unit = 1; ++index; }
        else if (tokenEquals(tokens.text[index], tokens.length[index], "mm")) { unit = 2; ++index; }
    }

    double degrees = distance;
    if (unit == 1) {
        degrees = distance * 360.0;
    } else if (unit == 2) {
        double rotationMm = 0.0;
        if (!rotation.rotationMm(step.id, rotationMm)) {
            return failAt(error, step.line, "rotation_distance_missing");
        }
        if (!(rotationMm >= kQueueMinRotationMm && rotationMm <= kQueueMaxRotationMm)) {
            return failAt(error, step.line, "rotation_distance_invalid");
        }
        degrees = distance / rotationMm * 360.0;
    }
    const double tenths = degrees * 10.0;
    if (!(tenths >= -double(limits.maxAngleTenths) && tenths <= double(limits.maxAngleTenths))) {
        return failAt(error, step.line, "move_angle_out_of_range");
    }
    const long rounded = static_cast<long>(tenths >= 0 ? tenths + 0.5 : tenths - 0.5);
    if (rounded == 0) return failAt(error, step.line, "move_angle_rounds_to_zero");
    step.distanceTenths = static_cast<int32_t>(rounded);

    double rpm = kDefaultMoveRpm;
    if (index < tokens.count && !parseSignedDecimal(tokens.text[index], tokens.length[index], rpm)) {
        return failAt(error, step.line, "invalid_number");
    }
    if (index < tokens.count) ++index;
    if (!(rpm >= 0.1) || rpm * 10.0 > double(limits.maxSpeedTenths)) {
        return failAt(error, step.line, "move_speed_out_of_range");
    }
    step.speedTenths = static_cast<uint16_t>(rpm * 10.0 + 0.5);
    if (step.speedTenths == 0) return failAt(error, step.line, "move_speed_out_of_range");

    long accel = kDefaultMoveAccel, decel = kDefaultMoveDecel, current = kDefaultMoveCurrent;
    if (index < tokens.count) {
        if (!parseUnsignedInteger(tokens.text[index], tokens.length[index], accel)) {
            return failAt(error, step.line, "invalid_integer");
        }
        ++index;
    }
    if (index < tokens.count) {
        if (!parseUnsignedInteger(tokens.text[index], tokens.length[index], decel)) {
            return failAt(error, step.line, "invalid_integer");
        }
        ++index;
    }
    if (index < tokens.count) {
        if (!parseUnsignedInteger(tokens.text[index], tokens.length[index], current)) {
            return failAt(error, step.line, "invalid_integer");
        }
        ++index;
    }
    if (index != tokens.count) return failAt(error, step.line, "argument_count");

    // Ranges are enforced on the parsed value BEFORE any narrowing cast: a
    // current of 66336 must be rejected, never wrapped into a plausible 800.
    if (accel < 1 || accel > long(limits.maxAccelRpmS)) {
        return failAt(error, step.line, "move_accel_out_of_range");
    }
    if (decel < 1 || decel > long(limits.maxAccelRpmS)) {
        return failAt(error, step.line, "move_decel_out_of_range");
    }
    if (current < 100 || current > long(limits.maxCurrentMa)) {
        return failAt(error, step.line, "move_current_out_of_range");
    }

    // The controller's own planner is the single source of truth for the move
    // limits, so the queue validates with exactly the same policy.
    MoveRequest request;
    request.id = step.id;
    request.angleDeg = float(step.distanceTenths) / 10.0f;
    request.speedRpm = float(step.speedTenths) / 10.0f;
    request.accelRpmS = float(accel);
    request.decelRpmS = float(decel);
    request.currentMa = static_cast<uint16_t>(current);
    MovePlan plan;
    const char* planError = nullptr;
    if (!buildMovePlan(request, plan, &planError, limits)) {
        return failAt(error, step.line, planError ? planError : "move_invalid");
    }
    step.accelRpmS = static_cast<uint16_t>(accel);
    step.decelRpmS = static_cast<uint16_t>(decel);
    step.currentMa = static_cast<uint16_t>(current);
    return true;
}

bool parseTorqueStep(const Tokens& tokens, const DebugLimits& limits, QueueStep& step, QueueError& error) {
    // torque ID SIGNED_MA DURATION_MS [MAX_RPM [RAMP_MA_S]]: the duration is
    // mandatory, so four tokens are the minimum.
    if (tokens.count < 4 || tokens.count > 6) return failAt(error, step.line, "argument_count");
    if (!parseId(tokens, 1, step.id, error, step.line)) return false;
    long currentMa = 0, durationMs = 0, ramp = kDefaultTorqueRamp;
    double maxRpm = kDefaultTorqueMaxRpm;
    if (!parseSignedInteger(tokens.text[2], tokens.length[2], currentMa)) {
        return failAt(error, step.line, "invalid_integer");
    }
    // The C5 current field has no 100 mA floor (unlike C6): 1..maxCurrentMa.
    if (currentMa == 0) return failAt(error, step.line, "timed_value_zero");
    if (currentMa > long(limits.maxCurrentMa) || currentMa < -long(limits.maxCurrentMa)) {
        return failAt(error, step.line, "timed_current_out_of_range");
    }
    if (!parseUnsignedInteger(tokens.text[3], tokens.length[3], durationMs)) {
        return failAt(error, step.line, "invalid_integer");
    }
    if (!validateDuration(durationMs, limits, error, step.line)) return false;
    if (tokens.count > 4) {
        // The C5 limit is a speed in RPM like every other speed field, so it
        // keeps the 0.1 RPM resolution.
        if (!parseSignedDecimal(tokens.text[4], tokens.length[4], maxRpm)) {
            return failAt(error, step.line, "invalid_number");
        }
    }
    if (tokens.count > 5) {
        // The wire field is a uint16: the documented range is 0..65535 mA/s and
        // 0 means "no ramp", so it is accepted rather than clipped.
        if (!parseUnsignedInteger(tokens.text[5], tokens.length[5], ramp)) {
            return failAt(error, step.line, "invalid_integer");
        }
        if (ramp > 65535) return failAt(error, step.line, "timed_ramp_out_of_range");
    }
    // The DEFAULT speed is validated exactly like an explicit one, before any
    // frame is sent: a policy below 30 RPM rejects the step instead of running
    // it faster than the policy allows.
    if (!(maxRpm >= 0.1) || maxRpm * 10.0 > double(limits.maxSpeedTenths)) {
        return failAt(error, step.line, "timed_speed_out_of_range");
    }
    const double maxSpeedTenths = maxRpm * 10.0 + 0.5;
    if (static_cast<long>(maxSpeedTenths) == 0) {
        return failAt(error, step.line, "timed_speed_out_of_range");
    }
    step.torqueMa = static_cast<int32_t>(currentMa);
    step.durationMs = static_cast<uint32_t>(durationMs);
    step.maxSpeedTenths = static_cast<uint16_t>(maxSpeedTenths);
    step.rampMaPerSec = static_cast<uint16_t>(ramp);
    return true;
}

bool parseVelocityStep(const Tokens& tokens, const DebugLimits& limits, QueueStep& step, QueueError& error) {
    // velocity ID SIGNED_RPM DURATION_MS [ACCEL [CURRENT]]: duration mandatory.
    if (tokens.count < 4 || tokens.count > 6) return failAt(error, step.line, "argument_count");
    if (!parseId(tokens, 1, step.id, error, step.line)) return false;
    double rpm = 0.0;
    if (!parseSignedDecimal(tokens.text[2], tokens.length[2], rpm)) {
        return failAt(error, step.line, "invalid_number");
    }
    // Zero is not a timed action (use stop), and a magnitude below 0.1 RPM would
    // round to zero on the wire.
    const double magnitude = rpm < 0.0 ? -rpm : rpm;
    if (magnitude < 0.1) return failAt(error, step.line, "timed_value_zero");
    const double signedTenths = rpm * 10.0;
    const int32_t tenths = static_cast<int32_t>(signedTenths >= 0 ? signedTenths + 0.5 : signedTenths - 0.5);
    if (tenths == 0) return failAt(error, step.line, "timed_value_zero");
    if (tenths > int32_t(limits.maxSpeedTenths) || tenths < -int32_t(limits.maxSpeedTenths)) {
        return failAt(error, step.line, "timed_speed_out_of_range");
    }
    long durationMs = 0;
    if (!parseUnsignedInteger(tokens.text[3], tokens.length[3], durationMs)) {
        return failAt(error, step.line, "invalid_integer");
    }
    if (!validateDuration(durationMs, limits, error, step.line)) return false;
    long accel = kDefaultVelocityAccel, current = kDefaultVelocityCurrent;
    if (tokens.count > 4) {
        if (!parseUnsignedInteger(tokens.text[4], tokens.length[4], accel)) {
            return failAt(error, step.line, "invalid_integer");
        }
    }
    if (tokens.count > 5) {
        if (!parseUnsignedInteger(tokens.text[5], tokens.length[5], current)) {
            return failAt(error, step.line, "invalid_integer");
        }
    }
    // Default acceleration and current limit are validated like explicit values
    // (the C6 frame carries both), before anything is transmitted.
    if (accel < 1 || accel > long(limits.maxAccelRpmS)) {
        return failAt(error, step.line, "timed_accel_out_of_range");
    }
    if (current < 100 || current > long(limits.maxCurrentMa)) {
        return failAt(error, step.line, "timed_current_out_of_range");
    }
    step.velocityTenths = tenths;
    step.durationMs = static_cast<uint32_t>(durationMs);
    step.timedAccel = static_cast<uint16_t>(accel);
    step.timedCurrentMa = static_cast<uint16_t>(current);
    return true;
}

bool parseStep(const Tokens& tokens, const DebugLimits& limits,
               const QueueRotationSource& rotation, QueueStep& step, QueueError& error) {
    const QueueAction action = actionFromToken(tokens.text[0], tokens.length[0]);
    if (action == QueueAction::None) return failAt(error, step.line, "unknown_action");
    step.action = action;

    switch (action) {
        case QueueAction::Enable:
        case QueueAction::Disable:
        case QueueAction::Stop: {
            if (tokens.count != 2) return failAt(error, step.line, "argument_count");
            return parseId(tokens, 1, step.id, error, step.line);
        }
        case QueueAction::Home: {
            if (tokens.count < 2 || tokens.count > 3) return failAt(error, step.line, "argument_count");
            if (!parseId(tokens, 1, step.id, error, step.line)) return false;
            long mode = 0;
            if (tokens.count == 3 && !parseUnsignedInteger(tokens.text[2], tokens.length[2], mode)) {
                return failAt(error, step.line, "invalid_integer");
            }
            if (mode > 5) return failAt(error, step.line, "home_mode_out_of_range");
            step.mode = static_cast<uint8_t>(mode);
            return true;
        }
        case QueueAction::Move:
            return parseMoveStep(tokens, limits, rotation, step, error);
        case QueueAction::Torque:
            return parseTorqueStep(tokens, limits, step, error);
        case QueueAction::Velocity:
            return parseVelocityStep(tokens, limits, step, error);
        case QueueAction::Wait: {
            if (tokens.count != 2) return failAt(error, step.line, "argument_count");
            long waitMs = 0;
            if (!parseUnsignedInteger(tokens.text[1], tokens.length[1], waitMs)) {
                return failAt(error, step.line, "invalid_integer");
            }
            if (waitMs > long(kQueueMaxWaitMs)) {
                return failAt(error, step.line, "wait_out_of_range");
            }
            step.waitMs = static_cast<uint32_t>(waitMs);
            return true;
        }
        case QueueAction::Hex: {
            // One token per byte; the exact byte-count bounds are reported below.
            if (tokens.count < 2 || tokens.count > kQueueMaxRawBytes + 2) {
                return failAt(error, step.line, "argument_count");
            }
            uint8_t bytes[kQueueMaxRawBytes] = {};
            uint8_t count = 0;
            for (uint8_t i = 1; i < tokens.count; ++i) {
                uint8_t one = 0, written = 0;
                if (!parseHexBytes(tokens.text[i], tokens.length[i], &one, 1, written) || written != 1) {
                    return failAt(error, step.line, "hex_digit_invalid");
                }
                bytes[count++] = one;
            }
            if (count < 3 || count > kQueueMaxRawBytes) {
                return failAt(error, step.line, "hex_length_out_of_range");
            }
            memcpy(step.raw, bytes, count);
            step.rawLength = count;
            step.id = bytes[0];  // 0 is the documented broadcast address
            return true;
        }
        case QueueAction::Can: {
            if (tokens.count < 3 || tokens.count > kQueueMaxCanBytes + 4) {
                return failAt(error, step.line, "argument_count");
            }
            const bool extended = tokenEquals(tokens.text[1], tokens.length[1], "ext");
            const bool standard = tokenEquals(tokens.text[1], tokens.length[1], "std");
            if (!extended && !standard) return failAt(error, step.line, "can_format_invalid");
            uint32_t id = 0;
            if (!parseHexId(tokens.text[2], tokens.length[2], id)) {
                return failAt(error, step.line, "can_id_invalid");
            }
            const uint32_t idMax = extended ? 0x1FFFFFFFu : 0x7FFu;
            if (id > idMax) return failAt(error, step.line, "can_id_out_of_range");
            uint8_t data[kQueueMaxCanBytes] = {};
            uint8_t count = 0;
            for (uint8_t i = 3; i < tokens.count; ++i) {
                uint8_t one = 0, written = 0;
                if (!parseHexBytes(tokens.text[i], tokens.length[i], &one, 1, written) || written != 1) {
                    return failAt(error, step.line, "hex_digit_invalid");
                }
                if (count >= kQueueMaxCanBytes) return failAt(error, step.line, "can_length_out_of_range");
                data[count++] = one;
            }
            step.extended = extended;
            step.canId = id;
            step.canLength = count;
            memcpy(step.canData, data, count);
            return true;
        }
        case QueueAction::None:
            break;
    }
    return failAt(error, step.line, "unknown_action");
}

}  // namespace

bool parseQueueProgram(const char* text, size_t length, const DebugLimits& limits,
                       const QueueRotationSource& rotation, QueueProgram& program,
                       QueueError& error) {
    // Cheap reset: assigning a QueueProgram temporary would put the whole ~6 KB
    // plan on the caller's stack, which the ESP32 loop task cannot afford.
    program.count = 0;
    program.hasRaw = false;
    error = QueueError{};
    if (text == nullptr) return failAt(error, 0, "empty_program");
    if (length > kQueueMaxTextBytes) return failAt(error, 0, "program_too_long");

    uint16_t line = 0;
    size_t offset = 0;
    while (offset < length) {
        const size_t start = offset;
        while (offset < length && text[offset] != '\n') ++offset;
        size_t end = offset;
        if (offset < length) ++offset;  // consume the newline
        ++line;

        // '#' starts a comment; the rest of the line is ignored.
        for (size_t i = start; i < end; ++i) {
            if (text[i] == '#') { end = i; break; }
        }
        size_t first = start;
        while (first < end && isSpace(text[first])) ++first;
        size_t last = end;
        while (last > first && isSpace(text[last - 1])) --last;
        if (first == last) continue;  // blank or comment-only line

        Tokens tokens;
        if (!tokenize(text + first, last - first, tokens) || tokens.count == 0) {
            return failAt(error, line, "line_too_long");
        }
        if (program.count >= kQueueMaxSteps) return failAt(error, line, "too_many_actions");

        QueueStep& step = program.steps[program.count];
        step = QueueStep();
        step.line = line;
        if (!parseStep(tokens, limits, rotation, step, error)) {
            program.count = 0;
            program.hasRaw = false;
            return false;
        }
        if (step.action == QueueAction::Hex || step.action == QueueAction::Can) {
            program.hasRaw = true;
        }
        ++program.count;
    }

    if (program.count == 0) return failAt(error, 0, "empty_program");
    return true;
}

// ---------------------------------------------------------------------------
// Executor
// ---------------------------------------------------------------------------

const QueueStep* CommandQueue::currentStep() const {
    if (stepIndex_ >= program_.count) return nullptr;
    return &program_.steps[stepIndex_];
}

bool CommandQueue::stationary(const MotorControl::Snapshot& s) const {
    if (!s.positionValid || !s.velocityValid) return false;
    const int32_t speed = s.velocity < 0 ? -s.velocity : s.velocity;
    return speed <= kQueueStopTenths;
}

void CommandQueue::setMessage(const char* text) {
    if (text == nullptr) text = "";
    size_t i = 0;
    for (; text[i] != '\0' && i + 1 < sizeof(message_); ++i) message_[i] = text[i];
    message_[i] = '\0';
}

Result CommandQueue::start(const char* text, size_t length, long repeat,
                           const QueueRotationSource& rotation, uint32_t now) {
    if (state_ == QueueState::Running) return Result{409, "queue_busy"};
    if (repeat < 1 || repeat > long(kQueueMaxRepeat)) return Result{400, "repeat_out_of_range"};
    if (motor_.operationBusy()) return Result{409, "motion_active"};
    if (!motor_.ready()) return Result{503, "can_unavailable"};

    // The parsed plan is ~6 KB: it lives in one fixed scratch buffer instead of
    // the HTTP task stack, and is copied into program_ only when it is valid.
    // Nothing is allocated and the running plan is untouched by a refusal.
    static QueueProgram scratch;
    QueueError error;
    if (!parseQueueProgram(text, length, motor_.debugLimits(), rotation, scratch, error)) {
        // The previous run's state is left alone: this start was simply refused.
        // The HTTP 400 body carries the reason and the source line.
        errorLine_ = error.line;
        setMessage(error.message);
        return Result{400, error.message};
    }

    // The plan is a fixed-size copy and stays immutable until the run ends.
    program_ = scratch;
    repeat_ = static_cast<uint32_t>(repeat);
    runId_++;
    state_ = QueueState::Running;
    phase_ = kPhaseIdle;
    phaseAt_ = now;
    deadlineAt_ = 0;
    evidenceAt_ = 0;
    stepIndex_ = 0;
    iteration_ = 0;
    lastRawAt_ = 0;
    errorLine_ = 0;
    setMessage("running");
    return Result{202, "queue_started"};
}

Result CommandQueue::cancel(const char* reason) {
    const bool wasRunning = state_ == QueueState::Running;
    if (wasRunning) {
        state_ = QueueState::Cancelled;
        setMessage(reason != nullptr ? reason : "cancelled");
        phase_ = kPhaseIdle;
    }
    // Stop everything unconditionally: an authorised stop must keep working even
    // when the queue software already failed.
    if (!stopEverything()) return Result{503, "can_tx_failed"};
    return wasRunning ? Result{202, "queue_cancelled"} : Result{200, "queue_idle"};
}

bool CommandQueue::stopEverything() {
    // 9C then the FE broadcast: a raw step may have started a homing run this
    // board never supervised, and structured steps may still be moving.
    return motor_.broadcastAbortAll();
}

void CommandQueue::finish(QueueState state, const char* message) {
    state_ = state;
    setMessage(message);
    phase_ = kPhaseIdle;
}

void CommandQueue::fail(uint32_t now, const char* reason, uint16_t line) {
    if (state_ != QueueState::Running) return;  // the first error is kept
    if (errorLine_ == 0) errorLine_ = line;
    finish(QueueState::Failed, reason != nullptr ? reason : "failed");
    (void)now;
    stopEverything();
}

void CommandQueue::advance(uint32_t now) {
    ++stepIndex_;
    phase_ = kPhaseIdle;
    phaseAt_ = now;
    evidenceAt_ = 0;
}

void CommandQueue::dispatchRaw(uint32_t now, const QueueStep& step) {
    bool sent = false;
    if (step.action == QueueAction::Hex) {
        sent = motor_.rawLogical(step.raw, step.rawLength);
        if (sent) motor_.noteRawTransmission(step.id);
    } else {
        sent = motor_.rawCanFrame(step.canId, step.extended, step.canData, step.canLength);
        // An arbitrary CAN identifier has unknown effects, so every software
        // confirmation is dropped rather than attributed to one node.
        if (sent) motor_.noteRawTransmission(0);
    }
    lastRawAt_ = now;
    if (!sent) {
        fail(now, "raw_tx_failed", step.line);
        return;
    }
    // A raw frame invalidated the software feedback state, so the next
    // structured step must acquire a fresh pair even for the same address.
    // (Every supervised motion step acquires one anyway; this keeps the intent
    // explicit for the target selection below.)
    // Reported as sent only: no acknowledgement, no movement, no stop.
    advance(now);
}

void CommandQueue::dispatch(uint32_t now, const QueueStep& step) {
    switch (step.action) {
        case QueueAction::Enable:
        case QueueAction::Disable: {
            const Result result = motor_.enable(step.id, step.action == QueueAction::Enable);
            if (result.code >= 300) { fail(now, result.message, step.line); return; }
            phase_ = kPhaseEnable;
            phaseAt_ = now;
            return;
        }
        case QueueAction::Move: {
            MoveRequest request;
            request.id = step.id;
            request.angleDeg = float(step.distanceTenths) / 10.0f;
            request.speedRpm = float(step.speedTenths) / 10.0f;
            request.accelRpmS = float(step.accelRpmS);
            request.decelRpmS = float(step.decelRpmS);
            request.currentMa = step.currentMa;
            const Result result = motor_.move(request);
            if (result.code >= 300) { fail(now, result.message, step.line); return; }
            phase_ = kPhaseMove;
            phaseAt_ = now;
            return;
        }
        case QueueAction::Home: {
            const Result result = motor_.home(step.id, step.mode);
            if (result.code >= 300) { fail(now, result.message, step.line); return; }
            phase_ = kPhaseHome;
            phaseAt_ = now;
            return;
        }
        case QueueAction::Torque:
        case QueueAction::Velocity: {
            uint8_t frame[11] = {};
            uint8_t length = 0;
            if (step.action == QueueAction::Torque) {
                const int32_t magnitude = step.torqueMa < 0 ? -step.torqueMa : step.torqueMa;
                frame[length++] = step.id;
                frame[length++] = 0xC5;
                frame[length++] = step.torqueMa < 0 ? 1 : 0;
                frame[length++] = static_cast<uint8_t>((step.rampMaPerSec >> 8) & 0xFF);
                frame[length++] = static_cast<uint8_t>(step.rampMaPerSec & 0xFF);
                frame[length++] = static_cast<uint8_t>((magnitude >> 8) & 0xFF);
                frame[length++] = static_cast<uint8_t>(magnitude & 0xFF);
                frame[length++] = 0;  // immediate execution
                frame[length++] = static_cast<uint8_t>((step.maxSpeedTenths >> 8) & 0xFF);
                frame[length++] = static_cast<uint8_t>(step.maxSpeedTenths & 0xFF);
            } else {
                const int32_t magnitude = step.velocityTenths < 0 ? -step.velocityTenths : step.velocityTenths;
                frame[length++] = step.id;
                frame[length++] = 0xC6;
                frame[length++] = step.velocityTenths < 0 ? 1 : 0;
                frame[length++] = static_cast<uint8_t>((step.timedAccel >> 8) & 0xFF);
                frame[length++] = static_cast<uint8_t>(step.timedAccel & 0xFF);
                frame[length++] = static_cast<uint8_t>((magnitude >> 8) & 0xFF);
                frame[length++] = static_cast<uint8_t>(magnitude & 0xFF);
                frame[length++] = 0;  // immediate execution
                frame[length++] = static_cast<uint8_t>((step.timedCurrentMa >> 8) & 0xFF);
                frame[length++] = static_cast<uint8_t>(step.timedCurrentMa & 0xFF);
            }
            frame[length++] = 0x6B;
            const Result result = motor_.command(frame, length);
            if (result.code >= 300) { fail(now, result.message, step.line); return; }
            phase_ = kPhaseTimed;
            phaseAt_ = now;
            deadlineAt_ = now + step.durationMs;
            return;
        }
        case QueueAction::Stop: {
            const Result result = motor_.stop(step.id);
            if (result.code >= 300) { fail(now, result.message, step.line); return; }
            phase_ = kPhaseStop;
            phaseAt_ = now;
            return;
        }
        case QueueAction::Wait: {
            phase_ = kPhaseWait;
            phaseAt_ = now;
            deadlineAt_ = now + step.waitMs;
            return;
        }
        case QueueAction::Hex:
        case QueueAction::Can:
        case QueueAction::None:
            break;
    }
    fail(now, "internal_action", step.line);
}

void CommandQueue::beginStep(uint32_t now) {
    if (stepIndex_ >= program_.count) {
        // One iteration finished. The next one starts on the next poll, so an
        // action never starts twice in one poll.
        ++iteration_;
        if (iteration_ >= repeat_) {
            // Raw steps only ever mean "frames submitted": the run must not
            // report a mechanical completion it cannot know about.
            finish(QueueState::Done, program_.hasRaw ? "raw_frames_submitted" : "done");
            return;
        }
        stepIndex_ = 0;
        return;
    }

    const QueueStep& step = program_.steps[stepIndex_];

    if (step.action == QueueAction::Hex || step.action == QueueAction::Can) {
        if (uint32_t(now - lastRawAt_) < kRawSpacingMs) return;  // keep raw frames apart
        if (motor_.operationBusy()) { fail(now, "busy", step.line); return; }
        dispatchRaw(now, step);
        return;
    }

    if (step.action == QueueAction::Wait) {
        dispatch(now, step);
        return;
    }

    // Every supervised motion step acquires fresh stationary feedback of its own
    // target before it is dispatched, not only when the target changed: a
    // completed home invalidates the position, and a raw frame invalidates the
    // whole software state, so the next step must never reuse the old pair.
    if (requiresStationaryBeforeDispatch(step.action)) {
        motor_.watch(step.id);
        phase_ = kPhaseWatch;
        phaseAt_ = now;
        return;
    }

    // stop/disable/enable are dispatched promptly: a stop that waited for a
    // quiet node would not be a stop. The controller watches the node itself.
    dispatch(now, step);
}

void CommandQueue::watchTarget(uint32_t now) {
    const QueueStep* step = currentStep();
    if (step == nullptr) { fail(now, "internal_step", 0); return; }
    motor_.watch(step->id);
    const MotorControl::Snapshot snapshot = motor_.snapshot(step->id);
    if (snapshot.positionValid && snapshot.velocityValid && stationary(snapshot)) {
        dispatch(now, *step);
        return;
    }
    if (uint32_t(now - phaseAt_) >= kEvidenceWaitMs) {
        fail(now, "target_feedback_timeout", step->line);
    }
}

void CommandQueue::waitEnable(uint32_t now) {
    const QueueStep* step = currentStep();
    if (step == nullptr) { fail(now, "internal_step", 0); return; }
    const bool wanted = step->action == QueueAction::Enable;
    const MotorControl::Snapshot snapshot = motor_.snapshot(step->id);
    const uint32_t elapsed = uint32_t(now - phaseAt_);
    if (snapshot.enableTimedOut) { fail(now, "enable_ack_timeout", step->line); return; }

    // The evidence must be NEWER than the request: a position/velocity pair that
    // predates the F3 frame proves nothing about the state it asked for.
    const bool freshSinceRequest =
        snapshot.positionValid && snapshot.velocityValid &&
        snapshot.positionAge < elapsed && snapshot.velocityAge < elapsed;
    // A disable additionally has to see its own stop confirmation, and no
    // request may still be in flight.
    const bool settled = !snapshot.enablePending && (wanted || !snapshot.stopPending);
    if (snapshot.enableAck && snapshot.enabled == wanted && settled &&
        freshSinceRequest && stationary(snapshot)) {
        advance(now);
        return;
    }
    if (elapsed >= kEnableWaitMs) {
        fail(now, wanted ? "enable_not_stationary" : "disable_not_stationary", step->line);
    }
}

void CommandQueue::waitMove(uint32_t now) {
    const QueueStep* step = currentStep();
    if (step == nullptr) { fail(now, "internal_step", 0); return; }
    switch (motor_.moveOutcome()) {
        case MotorControl::MoveOutcome::Done: advance(now); return;
        case MotorControl::MoveOutcome::Running: break;
        case MotorControl::MoveOutcome::Failed: fail(now, "move_failed", step->line); return;
        case MotorControl::MoveOutcome::Cancelled: fail(now, "move_cancelled", step->line); return;
        case MotorControl::MoveOutcome::None: fail(now, "move_not_started", step->line); return;
    }
}

void CommandQueue::waitHome(uint32_t now) {
    const QueueStep* step = currentStep();
    if (step == nullptr) { fail(now, "internal_step", 0); return; }
    switch (motor_.homeOutcome()) {
        case MotorControl::HomeOutcome::Done: advance(now); return;
        case MotorControl::HomeOutcome::Running: break;
        case MotorControl::HomeOutcome::NoMotion: {
            // Manual 12/22: the trigger was answered with "already at the origin
            // or the limit is already triggered, the motor did not move". That is
            // reported distinctly and the queue only continues once a fresh
            // stationary pair that arrived AFTER that answer confirms the node is
            // not moving. It never claims a homing run happened, and the wait is
            // bounded so a silent node cannot keep the run alive forever.
            if (evidenceAt_ == 0) evidenceAt_ = now;
            const uint32_t elapsed = uint32_t(now - evidenceAt_);
            const MotorControl::Snapshot snapshot = motor_.snapshot(step->id);
            if (stationary(snapshot) &&
                snapshot.positionAge < elapsed && snapshot.velocityAge < elapsed) {
                evidenceAt_ = 0;
                setMessage("home_no_motion");
                advance(now);
                return;
            }
            if (elapsed >= kEvidenceWaitMs) {
                evidenceAt_ = 0;
                fail(now, "home_no_motion_timeout", step->line);
            }
            return;
        }
        case MotorControl::HomeOutcome::Failed: fail(now, "home_failed", step->line); return;
        case MotorControl::HomeOutcome::Cancelled: fail(now, "home_cancelled", step->line); return;
        case MotorControl::HomeOutcome::None: fail(now, "home_not_started", step->line); return;
    }
}

void CommandQueue::waitTimed(uint32_t now) {
    const QueueStep* step = currentStep();
    if (step == nullptr) { fail(now, "internal_step", 0); return; }
    if (int32_t(now - deadlineAt_) < 0) return;  // still inside the timed window
    const Result result = motor_.stop(step->id);
    if (result.code >= 300) { fail(now, result.message, step->line); return; }
    phase_ = kPhaseStop;
    phaseAt_ = now;
}

void CommandQueue::waitStop(uint32_t now) {
    const QueueStep* step = currentStep();
    if (step == nullptr) { fail(now, "internal_step", 0); return; }
    const MotorControl::Snapshot snapshot = motor_.snapshot(step->id);
    if (!snapshot.stopPending && stationary(snapshot)) { advance(now); return; }
    if (uint32_t(now - phaseAt_) >= kStopWaitMs) {
        fail(now, "stop_unconfirmed", step->line);
    }
}

void CommandQueue::waitTimer(uint32_t now) {
    if (int32_t(now - deadlineAt_) < 0) return;
    advance(now);
}

void CommandQueue::poll(uint32_t now) {
    if (state_ != QueueState::Running) return;

    // A lost bus or a latched fault ends the run: nothing continues blind and no
    // step is ever advanced on a timeout. The controller's own fault tag is
    // reported, so the run says WHY it stopped (ack_rejected, move_timeout, ...)
    // instead of a generic label.
    if (!motor_.ready()) { fail(now, "can_unavailable", currentStep() ? currentStep()->line : 0); return; }
    if (motor_.hasFault()) { fail(now, motor_.faultTag(), currentStep() ? currentStep()->line : 0); return; }

    // Keep the controller aimed at this step's node in every phase. A status
    // read or an unrelated read command for another address must not steal the
    // feedback target the running step depends on.
    const QueueStep* step = currentStep();
    if (step != nullptr && step->action != QueueAction::Hex &&
        step->action != QueueAction::Can && step->action != QueueAction::Wait) {
        motor_.watch(step->id);
    }

    switch (phase_) {
        case kPhaseIdle: beginStep(now); return;
        case kPhaseWatch: watchTarget(now); return;
        case kPhaseEnable: waitEnable(now); return;
        case kPhaseMove: waitMove(now); return;
        case kPhaseHome: waitHome(now); return;
        case kPhaseTimed: waitTimed(now); return;
        case kPhaseStop: waitStop(now); return;
        case kPhaseWait: waitTimer(now); return;
    }
    fail(now, "internal_phase", 0);
}

namespace {
void appendEscaped(String& json, const char* text) {
    if (text == nullptr) return;
    for (const char* p = text; *p != '\0'; ++p) {
        const char c = *p;
        if (c == '"' || c == '\\') { json += '\\'; json += c; }
        else if (static_cast<uint8_t>(c) >= 0x20) json += c;
    }
}
}  // namespace

String CommandQueue::statusJson() const {
    // The reported step/action describe where the run is now: the step still in
    // flight, or the one about to start after an advance. Before any run it is 0.
    const bool hasPlan = runId_ != 0 && program_.count != 0;
    const uint16_t stepNumber = !hasPlan ? 0
        : static_cast<uint16_t>(stepIndex_ >= program_.count ? program_.count : stepIndex_ + 1);
    const uint16_t actionIndex = program_.count == 0 ? 0
        : (stepIndex_ >= program_.count ? static_cast<uint16_t>(program_.count - 1)
                                        : stepIndex_);

    String json;
    json.reserve(256);
    json += "{\"state\":\"";
    json += queueStateName(state_);
    json += "\",\"runId\":";
    json += static_cast<unsigned long>(runId_);
    json += ",\"step\":";
    json += static_cast<unsigned int>(stepNumber);
    json += ",\"total\":";
    json += static_cast<unsigned int>(program_.count);
    json += ",\"iteration\":";
    json += static_cast<unsigned int>(
        runId_ == 0 ? 0 : (iteration_ >= repeat_ ? repeat_ : iteration_ + 1));
    json += ",\"repeat\":";
    json += static_cast<unsigned long>(repeat_);
    json += ",\"line\":";
    json += static_cast<unsigned int>(errorLine_ != 0 ? errorLine_
        : (program_.count == 0 ? 0 : program_.steps[actionIndex].line));
    json += ",\"action\":\"";
    json += queueActionName(program_.count == 0 ? QueueAction::None
                                                : program_.steps[actionIndex].action);
    json += "\",\"message\":\"";
    appendEscaped(json, message_);
    json += "\",\"raw\":";
    json += program_.hasRaw ? "true" : "false";
    json += "}";
    return json;
}

}  // namespace motion
