#include "CommandQueue.h"

#include <string.h>

namespace motion {
namespace {

// --- Limits of the sender ---------------------------------------------------
// Raw frames retain the documented minimum gap; sync has its own state machine.
constexpr uint32_t kRawSpacingMs = 2;
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
    if (tokenEquals(text, length, "sync")) return QueueAction::SyncBegin;
    return QueueAction::None;
}

bool failAt(QueueError& error, uint16_t line, const char* message) {
    error.line = line;
    error.message = message;
    return false;
}

// The only duration bound is the DSL resource bound: the timer is the user's own
// instruction and no board policy shortens or rejects it.
bool validateDuration(long durationMs, QueueError& error, uint16_t line) {
    if (durationMs < 1 || durationMs > long(kQueueMaxDurationMs)) {
        return failAt(error, line, "timed_duration_out_of_range");
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
        case QueueAction::SyncBegin: return "sync";
        case QueueAction::SyncEnd: return "sync_end";
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

bool parseMoveStep(const Tokens& tokens,
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
    // The angle field is a signed 32-bit count on the wire: that implementation
    // bound is kept explicit (reject, never wrap), and it is the ONLY distance
    // bound - the board's configured travel policy is not consulted here.
    if (!(tenths >= -double(kQueueMaxMoveTenths) && tenths <= double(kQueueMaxMoveTenths))) {
        return failAt(error, step.line, "move_angle_out_of_range");
    }
    const double roundedValue = tenths >= 0 ? tenths + 0.5 : tenths - 0.5;
    if (!(roundedValue >= -double(kQueueMaxMoveTenths) && roundedValue <= double(kQueueMaxMoveTenths))) {
        return failAt(error, step.line, "move_angle_out_of_range");
    }
    const int64_t rounded = static_cast<int64_t>(roundedValue);
    if (rounded == 0) return failAt(error, step.line, "move_angle_rounds_to_zero");
    step.distanceTenths = static_cast<int32_t>(rounded);

    double rpm = kDefaultMoveRpm;
    if (index < tokens.count && !parseSignedDecimal(tokens.text[index], tokens.length[index], rpm)) {
        return failAt(error, step.line, "invalid_number");
    }
    if (index < tokens.count) ++index;
    // Documented X firmware speed field: 0000-7E30, i.e. 0-3000.0 RPM.
    if (!(rpm >= 0.1) || rpm * 10.0 > double(kQueueMaxSpeedTenths)) {
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
    // The bounds are the documented wire ranges: acceleration 0..65535 RPM/s
    // (0 is legal on the wire) and current 0..5000 mA (no arbitrary floor).
    if (accel < 0 || accel > long(kQueueMaxAccelRpmS)) {
        return failAt(error, step.line, "move_accel_out_of_range");
    }
    if (decel < 0 || decel > long(kQueueMaxAccelRpmS)) {
        return failAt(error, step.line, "move_decel_out_of_range");
    }
    if (current < 0 || current > long(kQueueMaxCurrentMa)) {
        return failAt(error, step.line, "move_current_out_of_range");
    }

    step.accelRpmS = static_cast<uint16_t>(accel);
    step.decelRpmS = static_cast<uint16_t>(decel);
    step.currentMa = static_cast<uint16_t>(current);
    return true;
}

bool parseTorqueStep(const Tokens& tokens, QueueStep& step, QueueError& error) {
    // torque ID SIGNED_MA [DURATION_MS [MAX_RPM [RAMP_MA_S]]]
    //   * without a duration: send the frame and continue (no timer, no stop)
    //   * with a duration: legacy timed form - send, wait, then send FE and
    //     continue immediately (no feedback wait)
    if (tokens.count < 3 || tokens.count > 6) return failAt(error, step.line, "argument_count");
    if (!parseId(tokens, 1, step.id, error, step.line)) return false;
    long currentMa = 0, durationMs = 0, ramp = kDefaultTorqueRamp;
    double maxRpm = kDefaultTorqueMaxRpm;
    if (!parseSignedInteger(tokens.text[2], tokens.length[2], currentMa)) {
        return failAt(error, step.line, "invalid_integer");
    }
    // Documented C5 current field: 0000-1388, i.e. 0-5000 mA, and 0 is a legal
    // value on the wire (it is not a "zero torque" special case here).
    if (currentMa > long(kQueueMaxCurrentMa) || currentMa < -long(kQueueMaxCurrentMa)) {
        return failAt(error, step.line, "timed_current_out_of_range");
    }
    bool timed = false;
    if (tokens.count > 3) {
        timed = true;
        if (!parseUnsignedInteger(tokens.text[3], tokens.length[3], durationMs)) {
            return failAt(error, step.line, "invalid_integer");
        }
        if (!validateDuration(durationMs, error, step.line)) return false;
    }
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
    // The C5 speed field is 0000-7E30, i.e. 0-3000.0 RPM; 0 means "no limit" and
    // is accepted as written.
    if (maxRpm < 0.0 || maxRpm * 10.0 > double(kQueueMaxSpeedTenths)) {
        return failAt(error, step.line, "timed_speed_out_of_range");
    }
    const double maxSpeedTenths = maxRpm * 10.0 + 0.5;
    step.torqueMa = static_cast<int32_t>(currentMa);
    step.durationMs = timed ? static_cast<uint32_t>(durationMs) : 0;
    step.maxSpeedTenths = static_cast<uint16_t>(maxSpeedTenths);
    step.rampMaPerSec = static_cast<uint16_t>(ramp);
    return true;
}

bool parseVelocityStep(const Tokens& tokens, QueueStep& step, QueueError& error) {
    // velocity ID SIGNED_RPM [DURATION_MS [ACCEL [CURRENT]]]
    //   * without a duration: send the frame and continue (no timer, no stop)
    //   * with a duration: legacy timed form - send, wait, then send FE
    if (tokens.count < 3 || tokens.count > 6) return failAt(error, step.line, "argument_count");
    if (!parseId(tokens, 1, step.id, error, step.line)) return false;
    double rpm = 0.0;
    if (!parseSignedDecimal(tokens.text[2], tokens.length[2], rpm)) {
        return failAt(error, step.line, "invalid_number");
    }
    const double magnitude = rpm < 0.0 ? -rpm : rpm;
    if (magnitude < 0.1) return failAt(error, step.line, "timed_value_zero");
    const double signedTenths = rpm * 10.0;
    const int32_t tenths = static_cast<int32_t>(signedTenths >= 0 ? signedTenths + 0.5 : signedTenths - 0.5);
    if (tenths == 0) return failAt(error, step.line, "timed_value_zero");
    if (tenths > int32_t(kQueueMaxSpeedTenths) || tenths < -int32_t(kQueueMaxSpeedTenths)) {
        return failAt(error, step.line, "timed_speed_out_of_range");
    }
    long durationMs = 0;
    bool timed = false;
    if (tokens.count > 3) {
        timed = true;
        if (!parseUnsignedInteger(tokens.text[3], tokens.length[3], durationMs)) {
            return failAt(error, step.line, "invalid_integer");
        }
        if (!validateDuration(durationMs, error, step.line)) return false;
    }
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
    // Documented C6 fields: acceleration 0000-FFFF RPM/s and current
    // 0000-1388 mA. Both defaults are checked like explicit values because the
    // frame carries them either way.
    if (accel < 0 || accel > long(kQueueMaxAccelRpmS)) {
        return failAt(error, step.line, "timed_accel_out_of_range");
    }
    if (current < 0 || current > long(kQueueMaxCurrentMa)) {
        return failAt(error, step.line, "timed_current_out_of_range");
    }
    step.velocityTenths = tenths;
    step.durationMs = timed ? static_cast<uint32_t>(durationMs) : 0;
    step.timedAccel = static_cast<uint16_t>(accel);
    step.timedCurrentMa = static_cast<uint16_t>(current);
    return true;
}

bool parseStep(const Tokens& tokens,
               const QueueRotationSource& rotation, QueueStep& step, QueueError& error) {
    const QueueAction action = actionFromToken(tokens.text[0], tokens.length[0]);
    if (action == QueueAction::None) return failAt(error, step.line, "unknown_action");
    step.action = action;

    switch (action) {
        case QueueAction::SyncBegin:
        case QueueAction::SyncEnd:
            if(tokens.count==3 && tokenEquals(tokens.text[1],tokens.length[1],"begin") &&
               tokenEquals(tokens.text[2],tokens.length[2],"trigger")) {
                step.action=QueueAction::SyncBegin;step.syncTriggerOnly=true;return true;
            }
            if(tokens.count!=2) return failAt(error,step.line,"argument_count");
            if(tokenEquals(tokens.text[1],tokens.length[1],"begin")) {step.action=QueueAction::SyncBegin;return true;}
            if(tokenEquals(tokens.text[1],tokens.length[1],"end")) {step.action=QueueAction::SyncEnd;return true;}
            return failAt(error,step.line,"sync_invalid_delimiter");
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
            return parseMoveStep(tokens, rotation, step, error);
        case QueueAction::Torque:
            return parseTorqueStep(tokens, step, error);
        case QueueAction::Velocity:
            return parseVelocityStep(tokens, step, error);
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

bool parseQueueProgram(const char* text, size_t length,
                       const QueueRotationSource& rotation, QueueProgram& program,
                       QueueError& error) {
    // Cheap reset: assigning a QueueProgram temporary would put the whole plan
    // plan on the caller's stack, which the ESP32 loop task cannot afford.
    program.count = 0;
    program.hasRaw = false;
    error = QueueError{};
    if (text == nullptr) return failAt(error, 0, "empty_program");
    if (length > kQueueMaxTextBytes) return failAt(error, 0, "program_too_long");

    int groupStart=-1;
    const auto invalidGroup=[&](uint16_t sourceLine,const char* reason) {
        program.count=0;program.hasRaw=false;return failAt(error,sourceLine,reason);
    };
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

        if(tokenEquals(tokens.text[0],tokens.length[0],"helix")) {
            if(groupStart>=0) return invalidGroup(line,"sync_nested");
            if(tokens.count!=13) return invalidGroup(line,"helix_argument_count");
            if(program.count+4>kQueueMaxSteps) return invalidGroup(line,"too_many_actions");
            uint8_t rotaryId=0,linearId=0;
            if(!parseId(tokens,1,rotaryId,error,line) || !parseId(tokens,2,linearId,error,line)) {
                program.count=0;return false;
            }
            double values[10]={};
            for(uint8_t i=0;i<10;++i)
                if(!parseSignedDecimal(tokens.text[i+3],tokens.length[i+3],values[i])) return invalidGroup(line,"invalid_number");
            // turns, lead, ratio, rotary sign, linear sign, RPM, accel, decel, current, axial tolerance
            if((values[3]!=-1 && values[3]!=1) || (values[4]!=-1 && values[4]!=1) ||
               values[5]<0.1 || values[5]>3000 || values[6]<1 || values[6]>65535 || floor(values[6])!=values[6] ||
               values[7]<1 || values[7]>65535 || floor(values[7])!=values[7] ||
               values[8]<0 || values[8]>5000 || floor(values[8])!=values[8] || values[9]<=0)
                return invalidGroup(line,"helix_parameters_invalid");
            HelixGeometry geometry;
            geometry.leadMmPerCapRev=values[1];geometry.motorRevPerCapRev=values[2];
            geometry.rotaryDirection=int8_t(values[3]);geometry.linearDirection=int8_t(values[4]);
            if(!rotation.rotationMm(linearId,geometry.linearMmPerMotorRev)) return invalidGroup(line,"rotation_distance_missing");
            QueueStep& begin=program.steps[program.count];begin=QueueStep{};begin.action=QueueAction::SyncBegin;
            begin.line=line;begin.groupSize=2;begin.helixTravelMm=fabs(values[0]*values[1]);
            if(!isfinite(begin.helixTravelMm) || begin.helixTravelMm<=0) return invalidGroup(line,"helix_parameters_invalid");
            begin.syncToleranceProgress=values[9]/begin.helixTravelMm;
            for(uint8_t i=1;i<=2;++i) {
                auto& axis=program.steps[program.count+i];axis=QueueStep{};axis.line=line;
                axis.id=i==1?rotaryId:linearId;axis.speedTenths=uint16_t(values[5]*10+0.5);
                axis.accelRpmS=uint16_t(values[6]);axis.decelRpmS=uint16_t(values[7]);axis.currentMa=uint16_t(values[8]);
            }
            const char* reason=resolveHelix(values[0],geometry,program.steps[program.count+1],program.steps[program.count+2]);
            if(reason) return invalidGroup(line,reason);
            const double rotaryTravel=program.steps[program.count+1].distanceTenths/3600.0/values[2]*values[3]*values[1];
            const double linearTravel=program.steps[program.count+2].distanceTenths/3600.0*geometry.linearMmPerMotorRev*values[4];
            // Reserve twice the displacement-rounding error for progress up to
            // 2 (the runtime rejects travel beyond the much tighter envelope).
            begin.helixGeometryErrorMm=2*(fabs(rotaryTravel-values[0]*values[1])+fabs(linearTravel-values[0]*values[1]));
            if(begin.helixGeometryErrorMm>=values[9])
                return invalidGroup(line,"helix_geometry_quantization_tolerance");
            begin.syncToleranceProgress=(values[9]-begin.helixGeometryErrorMm)/begin.helixTravelMm;
            auto& endStep=program.steps[program.count+3];endStep=QueueStep{};endStep.action=QueueAction::SyncEnd;endStep.line=line;
            program.count+=4;continue;
        }

        QueueStep& step = program.steps[program.count];
        step = QueueStep();
        step.line = line;
        for (uint8_t i = 1; i < tokens.count; ++i) {
            if (!tokenEquals(tokens.text[i], tokens.length[i], "await")) continue;
            const QueueAction action = actionFromToken(tokens.text[0], tokens.length[0]);
            if (i != tokens.count - 1 ||
                (action != QueueAction::Move && action != QueueAction::Home)) {
                program.count = 0;
                program.hasRaw = false;
                return failAt(error, line, "invalid_await");
            }
            step.awaitCompletion = true;
            --tokens.count;
        }
        if (!parseStep(tokens, rotation, step, error)) {
            program.count = 0;
            program.hasRaw = false;
            return false;
        }
        if(step.action==QueueAction::SyncBegin) {
            if(groupStart>=0) return invalidGroup(line,"sync_nested");
            groupStart=program.count;
        } else if(step.action==QueueAction::SyncEnd) {
            if(groupStart<0) return invalidGroup(line,"sync_unmatched_end");
            const uint8_t members=program.count-groupStart-1;
            if(members<2 || members>8) return invalidGroup(line,"sync_member_count");
            program.steps[groupStart].groupSize=members;groupStart=-1;
        } else if(groupStart>=0) {
            if(step.action!=QueueAction::Move || step.awaitCompletion) return invalidGroup(line,"sync_only_relative_move");
            if(program.count-groupStart>8) return invalidGroup(line,"sync_member_count");
            for(uint8_t i=groupStart+1;i<program.count;++i)
                if(program.steps[i].id==step.id) return invalidGroup(line,"sync_duplicate_id");
        }
        if (step.action == QueueAction::Hex || step.action == QueueAction::Can) {
            program.hasRaw = true;
        }
        ++program.count;
    }

    if(groupStart>=0) return invalidGroup(program.steps[groupStart].line,"sync_missing_end");
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

void CommandQueue::setMessage(const char* text) {
    if (text == nullptr) text = "";
    size_t i = 0;
    for (; text[i] != '\0' && i + 1 < sizeof(message_); ++i) message_[i] = text[i];
    message_[i] = '\0';
}

Result CommandQueue::start(const char* text, size_t length, long repeat,
                           const QueueRotationSource& rotation, uint32_t now) {
    if (active()) return Result{409, "queue_busy"};
    if (repeat < 1 || repeat > long(kQueueMaxRepeat)) return Result{400, "repeat_out_of_range"};
    // Nothing else may refuse a start: a fault, a pending stop, a busy manual
    // operation or unloaded board limits are not this sender's business. Only a
    // bus that cannot transmit at all is reported, because then nothing could be
    // sent and pretending otherwise would be a lie.
    if (!motor_.ready()) return Result{503, "can_unavailable"};

    // The parsed plan lives in one fixed scratch buffer instead of
    // the HTTP task stack, and is copied into program_ only when it is valid.
    // Nothing is allocated and the running plan is untouched by a refusal.
    static QueueProgram scratch;
    QueueError error;
    if (!parseQueueProgram(text, length, rotation, scratch, error)) {
        // The previous run's state is left alone: this start was simply refused.
        // Nothing was sent and nothing was cleared.
        errorLine_ = error.line;
        setMessage(error.message);
        return Result{400, error.message};
    }
    for(uint8_t i=0;i<scratch.count;++i) {
        const auto& step=scratch.steps[i];
        if(step.action!=QueueAction::SyncBegin) continue;
        SyncSettings settings=syncSettings_;
        if(step.syncToleranceProgress>0) settings.tolerance.progress=fmin(settings.tolerance.progress,step.syncToleranceProgress);
        const char* reason=sync_.validate(scratch.steps+i+1,step.groupSize,settings,nullptr,step.syncTriggerOnly);
        if(reason) {errorLine_=step.line;setMessage(reason);return Result{400,reason};}
    }

    // The plan is a fixed-size copy and stays immutable until the run ends. Only
    // now - after a fully successful parse - take the board over from a stale
    // software supervisor: this clears MotorControl's volatile tracking and
    // sends nothing on CAN.
    program_ = scratch;
    strictHome_ = false;
    programHash_=2166136261u;
    for(size_t i=0;i<length;++i) programHash_=(programHash_^uint8_t(text[i]))*16777619u;
    sync_.reset();
    motor_.clearControlState();
    repeat_ = static_cast<uint32_t>(repeat);
    runId_++;
    motor_.queueDiagnostics_.beginRun(runId_);
    state_ = QueueState::Running;
    phase_ = kPhaseIdle;
    phaseAt_ = now;
    deadlineAt_ = 0;
    stepIndex_ = 0;
    iteration_ = 0;
    lastRawAt_ = 0;
    errorLine_ = 0;
    setMessage("running");
    motionComplete_=false;
    unconfirmedMotion_=false;
    return Result{202, "queue_started"};
}

Result CommandQueue::startDemo(const QueueProgram& program, uint32_t now) {
    if (active() || motor_.operationBusy()) return Result{409, "queue_busy"};
    if (!motor_.ready() || motor_.hasFault()) return Result{503, "can_unavailable"};
    if (!program.count || program.hasRaw) return Result{400, "invalid_demo_program"};
    program_ = program;
    strictHome_ = true;
    sync_.reset();
    programHash_ = 0;  // Demo programs have no DSL source text.
    motionComplete_ = false;
    unconfirmedMotion_ = false;
    helixTravelMm_ = helixGeometryErrorMm_ = 0;
    motor_.queueDiagnostics_.beginRun(runId_ + 1);
    repeat_ = 1; ++runId_; state_ = QueueState::Running;
    phase_ = kPhaseIdle; phaseAt_ = now; deadlineAt_ = 0;
    stepIndex_ = 0; iteration_ = 0; lastRawAt_ = 0; errorLine_ = 0;
    setMessage("running");
    return Result{202, "queue_started"};
}

Result CommandQueue::cancel(const char* reason) {
    motor_.queueObserveId_ = 0;
    motor_.queries_.release(CanQueryScheduler::Await);
    const bool wasRunning = state_ == QueueState::Running;
    if (wasRunning) {
        state_ = QueueState::Cancelled;
        setMessage(reason != nullptr ? reason : "cancelled");
        phase_ = kPhaseIdle;
    }
    if(sync_.active()) {
        sync_.abort(reason ? reason : "cancelled",millis());
        // A prior direct move or raw command may still be running outside the
        // sync group. The user-facing all-stop must cover those nodes too.
        if (!stopEverything()) return Result{503,"can_tx_failed"};
        return Result{202,"sync_stop_requested"};
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

Result CommandQueue::clearControlState() {
    const Result stopped = cancel("control_state_cleared");
    state_ = QueueState::Idle;
    phase_ = kPhaseIdle;
    program_.count = 0;
    program_.hasRaw = false;
    stepIndex_ = iteration_ = 0;
    repeat_ = 1;
    errorLine_ = 0;
    phaseAt_ = deadlineAt_ = lastRawAt_ = 0;
    setMessage("control_state_cleared");
    motor_.clearControlState();
    return stopped;
}

void CommandQueue::finish(QueueState state, const char* message) {
    motor_.queueObserveId_ = 0;
    motor_.queries_.release(CanQueryScheduler::Await);
    state_ = state;
    setMessage(message);
    phase_ = kPhaseIdle;
}

void CommandQueue::fail(uint32_t now, const char* reason, uint16_t line) {
    if (state_ != QueueState::Running) return;  // the first error is kept
    if (errorLine_ == 0) errorLine_ = line;
    // A send failure ends the run where it happened and reports the reason and
    // the source line. Nothing else is transmitted: no automatic stop broadcast
    // and no retry, because the operator asked for a plain sender.
    finish(QueueState::Failed, reason != nullptr ? reason : "failed");
    (void)now;
}

void CommandQueue::advance(uint32_t now) {
    motor_.queueObserveId_ = 0;
    motor_.queries_.release(CanQueryScheduler::Await);
    ++stepIndex_;
    phase_ = kPhaseIdle;
    phaseAt_ = now;
}

// One FE frame for one address: explicit/timed stops, or an opt-in sync fault.
// Ordinary direct actions never add fault-generated stops.
bool CommandQueue::sendStopFrame(uint8_t id) {
    const uint8_t frame[5] = {id, 0xFE, 0x98, 0, 0x6B};
    return motor_.queueSendLogical(frame, sizeof(frame));
}

// Encodes and sends one step. Everything goes through the queue-only transport,
// which checks the buffer/ID/DLC bounds and CAN readiness and nothing else: no
// supervised controller call is ever made from here, so no hidden enable, job,
// fault or config transaction can come back to life.
void CommandQueue::dispatchStep(uint32_t now, const QueueStep& step) {
    if(step.action!=QueueAction::Wait) motionComplete_=false;
    if((!step.awaitCompletion && (step.action==QueueAction::Move || step.action==QueueAction::Home ||
       step.action==QueueAction::Torque || step.action==QueueAction::Velocity)) ||
       step.action==QueueAction::Hex || step.action==QueueAction::Can) unconfirmedMotion_=true;
    if(step.action==QueueAction::SyncBegin) {
        SyncSettings settings=syncSettings_;
        if(step.syncToleranceProgress>0) settings.tolerance.progress=fmin(settings.tolerance.progress,step.syncToleranceProgress);
        helixTravelMm_=step.helixTravelMm;
        helixGeometryErrorMm_=step.helixGeometryErrorMm;
        const char* reason=sync_.start(program_.steps+stepIndex_+1,step.groupSize,settings,now,step.syncTriggerOnly);
        if(reason) {fail(now,reason,step.line);return;}
        phase_=kPhaseSync;setMessage("sync_checking");return;
    }
    if (step.awaitCompletion) {
        motor_.queueObserveId_ = step.id;
        auto& node = motor_.nodes_[step.id];
        int32_t startPosition = 0;
        expectedMoveTargetValid_ = step.action == QueueAction::Move &&
            (step.absolute || motor_.freshPosition(step.id, now, startPosition));
        if (expectedMoveTargetValid_) {
            expectedMoveTargetTenths_ = step.absolute ? step.distanceTenths :
                int64_t(startPosition) + step.distanceTenths;
        }
        node.queueAckFunction = node.queueAckStatus = 0;
        node.queueExpectedFunction = step.action == QueueAction::Home ? 0x9A : 0xCD;
        node.queueAckPending = false;
        node.queueHomeRunning = node.queueHomeComplete = node.queueHomeFailed = false;
        node.queueHomeProofMs = 0;
        node.targetValid = node.homeFlagsValid = false;
        accepted_ = homeSeenRunning_ = homeComplete_ = false;
        doneSamples_ = 0;
        observedAckAt_ = donePosAt_ = doneVelAt_ = now;
        homeProofAt_ = 0;
        phaseAt_ = now;
    }
    const bool sent = encodeAndSend(step);
    uint8_t function = 0;
    switch (step.action) {
        case QueueAction::Enable: case QueueAction::Disable: function=0xF3; break;
        case QueueAction::Move: function=0xCD; break;
        case QueueAction::Home: function=0x9A; break;
        case QueueAction::Torque: function=0xC5; break;
        case QueueAction::Velocity: function=0xC6; break;
        case QueueAction::Stop: function=0xFE; break;
        default: break;
    }
    const bool raw=step.action==QueueAction::Hex || step.action==QueueAction::Can;
    const uint8_t diagnosticId=step.action==QueueAction::Hex?step.raw[0]:
        step.action==QueueAction::Can?uint8_t(step.canId>>8):step.id;
    if(raw) function=step.action==QueueAction::Hex?step.raw[1]:step.canLength?step.canData[0]:0;
    motor_.queueDiagnostics_.submit(runId_, iteration_+1, step.line, diagnosticId,
                                    function, now, sent,raw);
    if (step.action == QueueAction::Hex || step.action == QueueAction::Can)
        motor_.queueDiagnostics_.invalidate(0);
    if (!sent) {
        // A real transmission failure ends the run here, with the source line. No
        // extra CAN frame is broadcast and the step is not retried.
        fail(now, "tx_failed", step.line);
        return;
    }
    if (step.action == QueueAction::Wait) {
        phase_ = kPhaseWait;
        deadlineAt_ = now + step.waitMs;
        return;
    }
    if (step.awaitCompletion) {
        phase_ = kPhaseMotion;
        setMessage(step.action == QueueAction::Move ? "waiting_position" : "waiting_home");
        return;
    }
    if (step.durationMs != 0 &&
        (step.action == QueueAction::Torque || step.action == QueueAction::Velocity)) {
        // Legacy timed form: the user's own duration, then one FE, then the next
        // step. No feedback is awaited: this is timing, not supervision.
        phase_ = kPhaseTimed;
        deadlineAt_ = now + step.durationMs;
        return;
    }
    if (step.action == QueueAction::Hex || step.action == QueueAction::Can) lastRawAt_ = now;
    advance(now);
}

bool CommandQueue::encodeAndSend(const QueueStep& step,bool synchronized) {
    uint8_t frame[20] = {};
    uint8_t length = 0;
    switch (step.action) {
        case QueueAction::Enable:
        case QueueAction::Disable: {
            // [addr][F3][AB][state][sync=0][6B]
            frame[length++] = step.id;
            frame[length++] = 0xF3;
            frame[length++] = 0xAB;
            frame[length++] = step.action == QueueAction::Enable ? 1 : 0;
            frame[length++] = 0;
            frame[length++] = 0x6B;
            return motor_.queueSendLogical(frame, length);
        }
        case QueueAction::Move: {
            // [addr][CD][dir][accel u16][decel u16][speed u16][angle u32]
            // [mode=2 relative to current][sync=0][current u16][6B]
            const int32_t distance = step.distanceTenths;
            const uint32_t magnitude = distance < 0
                ? static_cast<uint32_t>(-static_cast<int64_t>(distance))
                : static_cast<uint32_t>(distance);
            frame[length++] = step.id;
            frame[length++] = 0xCD;
            frame[length++] = distance < 0 ? 1 : 0;
            frame[length++] = static_cast<uint8_t>((step.accelRpmS >> 8) & 0xFF);
            frame[length++] = static_cast<uint8_t>(step.accelRpmS & 0xFF);
            frame[length++] = static_cast<uint8_t>((step.decelRpmS >> 8) & 0xFF);
            frame[length++] = static_cast<uint8_t>(step.decelRpmS & 0xFF);
            frame[length++] = static_cast<uint8_t>((step.speedTenths >> 8) & 0xFF);
            frame[length++] = static_cast<uint8_t>(step.speedTenths & 0xFF);
            frame[length++] = static_cast<uint8_t>((magnitude >> 24) & 0xFF);
            frame[length++] = static_cast<uint8_t>((magnitude >> 16) & 0xFF);
            frame[length++] = static_cast<uint8_t>((magnitude >> 8) & 0xFF);
            frame[length++] = static_cast<uint8_t>(magnitude & 0xFF);
            frame[length++] = step.absolute ? 1 : 2;
            frame[length++] = synchronized ? 1 : 0;
            frame[length++] = static_cast<uint8_t>((step.currentMa >> 8) & 0xFF);
            frame[length++] = static_cast<uint8_t>(step.currentMa & 0xFF);
            frame[length++] = 0x6B;
            return motor_.queueSendLogical(frame, length);
        }
        case QueueAction::Home: {
            // [addr][9A][mode][sync=0][6B]
            frame[length++] = step.id;
            frame[length++] = 0x9A;
            frame[length++] = step.mode;
            frame[length++] = 0;
            frame[length++] = 0x6B;
            return motor_.queueSendLogical(frame, length);
        }
        case QueueAction::Torque: {
            // [addr][C5][dir][slope u16][current u16][sync=0][maxSpeed u16][6B]
            const int32_t current = step.torqueMa;
            const uint32_t magnitude = current < 0
                ? static_cast<uint32_t>(-static_cast<int64_t>(current))
                : static_cast<uint32_t>(current);
            frame[length++] = step.id;
            frame[length++] = 0xC5;
            frame[length++] = current < 0 ? 1 : 0;
            frame[length++] = static_cast<uint8_t>((step.rampMaPerSec >> 8) & 0xFF);
            frame[length++] = static_cast<uint8_t>(step.rampMaPerSec & 0xFF);
            frame[length++] = static_cast<uint8_t>((magnitude >> 8) & 0xFF);
            frame[length++] = static_cast<uint8_t>(magnitude & 0xFF);
            frame[length++] = 0;
            frame[length++] = static_cast<uint8_t>((step.maxSpeedTenths >> 8) & 0xFF);
            frame[length++] = static_cast<uint8_t>(step.maxSpeedTenths & 0xFF);
            frame[length++] = 0x6B;
            return motor_.queueSendLogical(frame, length);
        }
        case QueueAction::Velocity: {
            // [addr][C6][dir][accel u16][speed u16][sync=0][current u16][6B]
            const int32_t velocity = step.velocityTenths;
            const uint32_t magnitude = velocity < 0
                ? static_cast<uint32_t>(-static_cast<int64_t>(velocity))
                : static_cast<uint32_t>(velocity);
            frame[length++] = step.id;
            frame[length++] = 0xC6;
            frame[length++] = velocity < 0 ? 1 : 0;
            frame[length++] = static_cast<uint8_t>((step.timedAccel >> 8) & 0xFF);
            frame[length++] = static_cast<uint8_t>(step.timedAccel & 0xFF);
            frame[length++] = static_cast<uint8_t>((magnitude >> 8) & 0xFF);
            frame[length++] = static_cast<uint8_t>(magnitude & 0xFF);
            frame[length++] = 0;
            frame[length++] = static_cast<uint8_t>((step.timedCurrentMa >> 8) & 0xFF);
            frame[length++] = static_cast<uint8_t>(step.timedCurrentMa & 0xFF);
            frame[length++] = 0x6B;
            return motor_.queueSendLogical(frame, length);
        }
        case QueueAction::Stop:
            return sendStopFrame(step.id);
        case QueueAction::Hex:
            return motor_.queueSendLogical(step.raw, step.rawLength);
        case QueueAction::Can:
            return motor_.queueSendFrame(step.canId, step.extended, step.canData, step.canLength);
        case QueueAction::Wait:
            return true;   // a timer needs no frame
        case QueueAction::SyncBegin:
        case QueueAction::SyncEnd:
            return false;
        case QueueAction::None:
            break;
    }
    return false;
}

// Starts the next step. The plan is sent in order, one step per poll, with the
// documented minimum gap between raw frames (the motor needs a moment between
// packets). Only explicitly awaited move/home steps inspect driver evidence.
void CommandQueue::beginStep(uint32_t now) {
    if (stepIndex_ >= program_.count) {
        // One iteration finished. The next one starts on the next poll, so an
        // action never starts twice in one poll.
        ++iteration_;
        if (iteration_ >= repeat_) {
            // Raw steps only ever mean "frames submitted": the run must not
            // report a mechanical completion it cannot know about.
            finish(QueueState::Done,motionComplete_ ? "sync_motion_complete" : program_.hasRaw ? "raw_frames_submitted" : "done");
            return;
        }
        stepIndex_ = 0;
        return;
    }

    const QueueStep& step = program_.steps[stepIndex_];
    if ((step.action == QueueAction::Hex || step.action == QueueAction::Can) &&
        uint32_t(now - lastRawAt_) < kRawSpacingMs) {
        return;  // keep raw frames apart
    }
    if (step.action == QueueAction::Move && step.awaitCompletion) {
        // An awaited relative move needs a fresh starting position. Otherwise
        // an unchanged old target and old position could masquerade as arrival.
        motor_.queueObserveId_ = step.id;
        int32_t position = 0, velocity = 0;
        const bool positionFresh = motor_.freshPosition(step.id, now, position);
        const bool velocityFresh = motor_.freshVelocity(step.id, now, velocity);
        if (!positionFresh || !velocityFresh || velocity < -5 || velocity > 5) {
            motor_.queries_.demand(step.id, 0x36, CanQueryScheduler::Await, 150, 1000, 2, now);
            motor_.queries_.demand(step.id, 0x35, CanQueryScheduler::Await, 150, 1000, 2, now);
            setMessage(positionFresh && velocityFresh ? "waiting_move_stationary" :
                       "waiting_move_start_feedback");
            return;
        }
    }
    dispatchStep(now, step);
}

void CommandQueue::poll(uint32_t now) {
    if(sync_.active() || phase_==kPhaseSync) {pollSync(now);return;}
    if (state_ != QueueState::Running) return;

    // No board state is consulted here: no fault, no pending stop, no feedback
    // freshness and no controller verdict. The sender's only hard requirement is
    // that the bus can transmit at all - otherwise nothing could be sent and the
    // run must say so instead of pretending.
    switch (phase_) {
        case kPhaseSync:
            pollSync(now);return;
        case kPhaseMotion:
            observeMotion(now);
            return;
        case kPhaseIdle:
            beginStep(now);
            return;
        case kPhaseWait:
            if (int32_t(now - deadlineAt_) < 0) return;
            advance(now);
            return;
        case kPhaseTimed: {
            if (int32_t(now - deadlineAt_) < 0) return;
            // Legacy timed form: the user asked for this duration, so one FE
            // frame goes out now and the next step follows immediately. It is not
            // a stop confirmation and nothing waits for one.
            const QueueStep* step = currentStep();
            const uint8_t id = step != nullptr ? step->id : 0;
            const bool sent=sendStopFrame(id);
            motor_.queueDiagnostics_.submit(runId_,iteration_+1,step ? step->line : 0,id,0xFE,now,sent);
            if (!sent) { fail(now, "tx_failed", step != nullptr ? step->line : 0); return; }
            advance(now);
            return;
        }
    }
    fail(now, "internal_phase", 0);
}

SyncFeedback CommandQueue::syncFeedback(uint8_t id) const {
    const auto& n=motor_.nodes_[id];SyncFeedback f;
    f.position=n.positionTenths;f.velocity=n.velocityTenths;f.target=n.targetTenths;
    f.positionAt=n.positionMs;f.velocityAt=n.velocityMs;f.targetAt=n.targetMs;
    f.flagsAt=n.flagsMs;f.homeAt=n.homeFlagsMs;f.flags=n.flags;f.homeFlags=n.homeFlags;
    f.positionValid=n.positionValid;f.velocityValid=n.velocityValid;f.targetValid=n.targetValid;
    f.flagsValid=n.flagsValid;f.homeValid=n.homeFlagsValid;
    const auto match=[&](uint8_t field,uint32_t at,uint32_t& requested) {
        const auto evidence=motor_.queries_.evidence(id,field);requested=evidence.sampleRequestAt;
        return evidence.receivedAt==at && at-requested<=syncSettings_.responseBudgetMs;
    };
    f.positionValid=f.positionValid && match(0x36,f.positionAt,f.positionRequestedAt);
    f.velocityValid=f.velocityValid && match(0x35,f.velocityAt,f.velocityRequestedAt);
    f.flagsValid=f.flagsValid && match(0x3A,f.flagsAt,f.flagsRequestedAt);
    f.homeValid=f.homeValid && match(0x3B,f.homeAt,f.homeRequestedAt);
    f.targetValid=f.targetValid && match(0x33,f.targetAt,f.targetRequestedAt);
    f.ackSequence=n.syncAckSequence;f.ackAt=n.syncAckMs;f.ack=n.syncAck;return f;
}
bool CommandQueue::syncSendMove(const QueueStep& step) {
    auto& n=motor_.nodes_[step.id];n.syncAck=0;n.targetValid=false;
    const uint32_t now=millis();
    const bool sent=encodeAndSend(step,true);
    motor_.queueDiagnostics_.submit(runId_,iteration_+1,step.line,step.id,0xCD,now,sent);
    return sent;
}
bool CommandQueue::syncTrigger() {
    const uint8_t bytes[]={0,0xFF,0x66,0x6B};
    return motor_.queueSendLogical(bytes,sizeof(bytes));
}
bool CommandQueue::syncStop(uint8_t id) {
    const bool sent=sendStopFrame(id);
    uint16_t line=currentStep()?currentStep()->line:0;
    for(uint8_t i=0;i<sync_.plan().count;++i) if(sync_.plan().axes[i].id==id) line=sync_.plan().axes[i].line;
    motor_.queueDiagnostics_.submit(runId_,iteration_+1,line,id,0xFE,millis(),sent);
    return sent;
}
void CommandQueue::syncObserve(uint8_t id,bool value) {motor_.syncObserve_[id]=value;}
void CommandQueue::pollSync(uint32_t now) {
    if(sync_.active() && !motor_.ready()) sync_.abort("can_unavailable",now);
    sync_.poll(now);
    if(sync_.phase()==SyncRuntime::Phase::Failed) {
        if(state_==QueueState::Running) fail(now,sync_.error(),currentStep()?currentStep()->line:0);
        else phase_=kPhaseIdle;
    } else if(sync_.phase()==SyncRuntime::Phase::Complete) {
        if(state_!=QueueState::Running) {phase_=kPhaseIdle;return;}
        const QueueStep* step=currentStep();
        if(!step || step->action!=QueueAction::SyncBegin) {fail(now,"sync_internal_step",0);return;}
        stepIndex_+=step->groupSize+1; // advance() then skips the closing delimiter
        advance(now);motionComplete_=!unconfirmedMotion_;setMessage("sync_motion_complete");
    } else {
        setMessage(sync_.error()?sync_.error():SyncRuntime::phaseName(sync_.phase()));
    }
}

String CommandQueue::syncSettingsJson() const {
    const auto& s=syncSettings_;
    String j("{\"configured\":");j+=s.valid()?"true":"false";
    j+=",\"progressTolerance\":";j+=String(s.tolerance.progress,8);
    j+=",\"timeToleranceMs\":";j+=static_cast<unsigned long>(s.tolerance.timeMs);
    j+=",\"feedbackTimeoutMs\":";j+=static_cast<unsigned long>(s.feedbackTimeoutMs);
    j+=",\"prepareTimeoutMs\":";j+=static_cast<unsigned long>(s.prepareTimeoutMs);
    j+=",\"stopTimeoutMs\":";j+=static_cast<unsigned long>(s.stopTimeoutMs);
    j+=",\"responseBudgetMs\":";j+=static_cast<unsigned long>(s.responseBudgetMs);
    j+=",\"completionTenths\":";j+=s.completionTenths;
    j+="}";return j;
}

// Observe completion without invoking the manual controller's fault/stop policy.
// Missing feedback keeps this line waiting and can recover; rejection releases
// the queue with a reason. Neither path sends stop/disable or latches a fault.
void CommandQueue::observeMotion(uint32_t now) {
    const QueueStep* step = currentStep();
    if (!step) return;
    auto& n = motor_.nodes_[step->id];
    const bool home = step->action == QueueAction::Home;
    const uint8_t opcode = home ? 0x9A : 0xCD;
    const auto newer = [](uint32_t a, uint32_t b) { return int32_t(a-b) > 0; };
    if (n.queueAckFunction == opcode && n.queueAckPending) {
        n.queueAckPending = false;
        observedAckAt_ = n.queueAckMs;
        const uint8_t code = n.queueAckStatus;
        if (code == 0xE2 || code == 0xEE) {
            fail(now, code == 0xE2 ? "driver_rejected" : "driver_command_error", step->line);
            return;
        }
        if (strictHome_ && home && (code == 0x12 || code == 0x22)) {
            fail(now, "home_no_motion", step->line); return;
        }
        if (code == 2 || code == 0x9F || (home && (code == 0x12 || code == 0x22))) {
            if (!accepted_) { accepted_ = true; phaseAt_ = n.queueAckMs; }
            if (home && code != 2 && !homeComplete_) {
                homeComplete_ = true;
                homeProofAt_ = n.queueAckMs;
            }
        }
    }
    if (home) {
        // RX accumulates transitions in arrival order, before another frame can
        // overwrite the public latest-value cache (including same-tick frames).
        if (n.queueHomeFailed) { fail(now, "home_failed", step->line); return; }
        // Fast homing may finish before any running sample is captured.
        // After a 1 s startup grace, accept a fresh idle/no-failure status.
        // Latch its first timestamp so later idle polls do not reset settling.
        if (!strictHome_ && accepted_ && !n.queueHomeComplete && n.homeFlagsValid &&
            newer(n.homeFlagsMs, phaseAt_) && uint32_t(n.homeFlagsMs - phaseAt_) >= 1000 &&
            uint32_t(now - n.homeFlagsMs) < 1000 && !(n.homeFlags & 0x3C)) {
            n.queueHomeComplete = true;
            n.queueHomeProofMs = n.homeFlagsMs;
        }
        homeSeenRunning_ = n.queueHomeRunning;
        homeComplete_ = n.queueHomeComplete;
        homeProofAt_ = n.queueHomeProofMs;
    }
    // Anchor to the completion transition, NOT each subsequent idle response.
    // Otherwise the 3B/36/35 query cycle resets the stationary pair count forever.
    const uint32_t proofAt = home && homeComplete_ ? homeProofAt_ : phaseAt_;
    const bool fresh = n.positionValid && n.velocityValid &&
        newer(n.positionMs, proofAt) && newer(n.velocityMs, proofAt) &&
        now-n.positionMs < 1000 && now-n.velocityMs < 1000;
    // An accepted CD can still leave the driver's old target unchanged. If the
    // pre-send position was fresh, require the target readback to represent this
    // move before allowing two stationary samples to complete the await.
    const bool expectedTargetSeen = !expectedMoveTargetValid_ ||
        (int64_t(n.targetTenths) - expectedMoveTargetTenths_ <= 5 &&
         int64_t(n.targetTenths) - expectedMoveTargetTenths_ >= -5);
    bool reached = home ? accepted_ && homeComplete_ :
        accepted_ && n.targetValid && newer(n.targetMs, phaseAt_) &&
        expectedTargetSeen &&
        int64_t(n.positionTenths)-n.targetTenths <= 1 &&
        int64_t(n.positionTenths)-n.targetTenths >= -1;
    if (!fresh || !reached) doneSamples_ = 0;
    if (fresh && newer(n.positionMs, donePosAt_) && newer(n.velocityMs, doneVelAt_)) {
        donePosAt_ = n.positionMs; doneVelAt_ = n.velocityMs;
        if (reached && n.velocityTenths >= -5 && n.velocityTenths <= 5) ++doneSamples_;
        else doneSamples_ = 0;
        if (doneSamples_ >= 2) { advance(now); setMessage("running"); return; }
    }
    setMessage(now-phaseAt_ > 2000 && !fresh ? "waiting_feedback" :
        (home ? "waiting_home" : "waiting_position"));
    if (home) {
        setMessage(!accepted_ ? "home_wait_ack" :
            !homeComplete_ ? (homeSeenRunning_ ? "home_wait_end" : "home_wait_start_or_done") :
            !fresh ? "home_wait_fresh_feedback" :
            (n.velocityTenths < -5 || n.velocityTenths > 5) ? "home_wait_stationary" :
            "home_confirming_stationary");
    } else if (accepted_ && n.targetValid && newer(n.targetMs, phaseAt_) &&
               !expectedTargetSeen) {
        setMessage("waiting_move_target");
    }
    const auto demand=[this,step,now](uint8_t field,uint32_t period) {
        motor_.queries_.demand(step->id,field,CanQueryScheduler::Await,period,1000,2,now);
    };
    demand(0x36,200); demand(0x35,200);
    if (home && !homeComplete_) demand(0x3B,300);
    else motor_.queries_.release(step->id,0x3B,CanQueryScheduler::Await);
    if (!home && (!n.targetValid || !newer(n.targetMs,phaseAt_))) demand(0x33,500);
    else motor_.queries_.release(step->id,0x33,CanQueryScheduler::Await);
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
    json += ",\"active\":";json += active()?"true":"false";
    json += ",\"programHash\":";json += static_cast<unsigned long>(programHash_);
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
    json += ",\"motionComplete\":";json += motionComplete_?"true":"false";
    json += ",\"sync\":{\"phase\":\"";json += SyncRuntime::phaseName(sync_.phase());
    json += "\",\"mode\":\"";json += sync_.triggerOnly()?"trigger_only":"progress_guarded";
    json += "\",\"error\":\"";appendEscaped(json,sync_.error());
    json += "\",\"sampleBudgetMs\":";json += static_cast<unsigned long>(sync_.sampleBudgetMs());
    json += ",\"errorLower\":";json += String(sync_.errorLower(),6);
    json += ",\"errorUpper\":";json += String(sync_.errorUpper(),6);
    json += ",\"maxObservableError\":";json += String(sync_.maxObservableError(),6);
    json += ",\"maxHelixObservableErrorMm\":";
    if(helixTravelMm_>0) json += String(fmax(0.0,sync_.maxObservableError()*helixTravelMm_-helixGeometryErrorMm_),6);else json += "null";
    json += ",\"helixErrorLowerMm\":";
    if(helixTravelMm_>0) json += String(fmax(0.0,sync_.errorLower()*helixTravelMm_-helixGeometryErrorMm_),6);else json += "null";
    json += ",\"helixErrorUpperMm\":";
    if(helixTravelMm_>0) json += String(sync_.errorUpper()*helixTravelMm_+helixGeometryErrorMm_,6);else json += "null";
    json += ",\"members\":[";
    for(uint8_t i=0;i<sync_.plan().count;++i) {
        if(i) json+=',';
        const auto& m=sync_.member(i);
        json += "{\"id\":";json += sync_.plan().axes[i].id;
        json += ",\"line\":";json += sync_.plan().axes[i].line;
        json += ",\"sentAt\":";json += static_cast<unsigned long>(m.sentAt);
        json += ",\"accepted\":";json += m.accepted?"true":"false";
        json += ",\"ackAssociation\":\"";
        json += m.targetConfirmed?(m.targetDeferred?"opcode_only_with_triggered_target":
            "opcode_only_with_changed_target"):"opcode_only";json += '"';
        json += ",\"targetObserved\":";json += m.targetObserved?"true":"false";
        json += ",\"targetDeferred\":";json += m.targetDeferred?"true":"false";
        json += ",\"targetConfirmed\":";json += m.targetConfirmed?"true":"false";
        json += ",\"targetExpectedTenths\":";json += String(static_cast<long>(m.target));
        json += ",\"previousTargetTenths\":";json += m.previousTarget;
        json += ",\"targetReadbackTenths\":";
        if(m.targetReadbackValid) json += m.lastTargetReadback;else json += "null";
        json += ",\"done\":";json += m.done?"true":"false";
        json += ",\"stopSent\":";json += m.stopSent?"true":"false";
        json += ",\"stopped\":";json += m.stopped?"true":"false";
        json += '}';
    }
    json += "]}";
    json += ",\"alert\":";
    const auto* alert=motor_.queueDiagnostics_.alert();
    if (!alert) json += "null";
    else {
        json += "{\"runId\":";json += static_cast<unsigned long>(alert->run);
        json += ",\"iteration\":";json += static_cast<unsigned long>(alert->iteration);
        json += ",\"line\":";json += alert->line;
        json += ",\"motor\":";json += alert->id;
        json += ",\"function\":";json += alert->function;
        json += ",\"sentAt\":";json += static_cast<unsigned long>(alert->sentAt);
        json += ",\"code\":";json += alert->code;
        json += ",\"confirmation\":\"";
        json += QueueDiagnostics::name(alert->confirmation);json += "\"}";
    }
    json += ",\"diagnosticsDropped\":";
    json += static_cast<unsigned long>(motor_.queueDiagnostics_.dropped());
    json += ",\"diagnostics\":[";
    for (uint8_t i=0; i<motor_.queueDiagnostics_.count(); ++i) {
        const auto& event=motor_.queueDiagnostics_.at(i);
        if (i) json += ',';
        json += "{\"sequence\":"; json += static_cast<unsigned long>(event.sequence);
        json += ",\"runId\":"; json += static_cast<unsigned long>(event.run);
        json += ",\"iteration\":"; json += static_cast<unsigned long>(event.iteration);
        json += ",\"line\":"; json += event.line;
        json += ",\"motor\":"; json += event.id;
        json += ",\"function\":"; json += event.function;
        json += ",\"sentAt\":"; json += static_cast<unsigned long>(event.sentAt);
        json += ",\"responseAt\":";
        if (event.responseSeen) json += static_cast<unsigned long>(event.responseAt);
        else json += "null";
        json += ",\"code\":"; json += event.code;
        json += ",\"confirmation\":\"";
        json += QueueDiagnostics::name(event.confirmation);
        json += "\",\"associationUncertain\":";json += event.ambiguous?"true":"false";
        json += "}";
    }
    json += ']';
    json += "}";
    return json;
}

}  // namespace motion
