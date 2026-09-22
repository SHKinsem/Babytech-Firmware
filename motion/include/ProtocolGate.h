#pragma once
#include <stdint.h>
#include "DebugLimits.h"
#include "MotionCore.h"

namespace motion {
enum class CommandKind { Invalid, Read, Configure, Enable, Stop, Move, DirectMove, Experiment, Home, Interrupt };
inline uint16_t word(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }
inline uint32_t dword(const uint8_t* p) { return (uint32_t(word(p)) << 16) | word(p + 2); }

// Manual V1.0.5 p64: the homing velocity field is 0000-0BB8, i.e. 0-3000 RPM in
// whole RPM (no 0.1 RPM scaling). This is the documented protocol ceiling; the
// configured speed policy can be stricter (see below).
constexpr uint16_t kHomeVelocityMaxRpm = 3000;
// Manual V1.0.5 p61: homing mode 00-05; the trigger is immediate only.
constexpr uint8_t kHomeModeMax = 5;
// Manual V1.0.5 p82: 0x45 writes the closed-loop maximum phase current; the
// documented field range is 0000-1388, i.e. 0-5000 mA (same as the current
// fields of the motion commands).
constexpr uint16_t kClosedLoopCurrentMaxMa = 5000;

// Strict whitelist, shared by controller and host tests. Unknown layouts cannot
// bypass the controller through the HEX editor. Synchronous queues and modes
// without a documented position interpretation remain preview-only.
inline CommandKind validateCommand(const uint8_t* b, uint8_t n, const DebugLimits& limits = DebugLimits{}) {
    using K = CommandKind;
    if (!b || n < 3 || !b[0] || b[n-1] != 0x6B) return K::Invalid;
    switch (b[1]) {
    // V1.0.5 pp67-77: additional read-only diagnostics. Replies are exposed
    // through the raw trace; this does not enable any new motion operation.
    case 0x1A: case 0x1F: case 0x20: case 0x21: case 0x24: case 0x26: case 0x27: case 0x31:
    case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37:
    case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D:
        return n == 3 ? K::Read : K::Invalid;
    case 0x42: return n == 4 && b[2] == 0x6C ? K::Read : K::Invalid;
    case 0x43: return n == 4 && b[2] == 0x7A ? K::Read : K::Invalid;
    case 0xF3: return n == 6 && b[2] == 0xAB && b[3] <= 1 && b[4] == 0 ? K::Enable : K::Invalid;
    case 0xFE: return n == 5 && b[2] == 0x98 && b[3] == 0 ? K::Stop : K::Invalid;
    case 0x9C: return n == 4 && b[2] == 0x48 ? K::Interrupt : K::Invalid;
    // V1.0.5 p61-62: trigger homing. Immediate execution only (sync 00); the
    // cached form still needs the FF trigger, which the board does not supervise.
    case 0x9A: return n == 5 && b[2] <= kHomeModeMax && b[3] == 0 ? K::Home : K::Invalid;
    case 0x0A: return n == 4 && b[2] == 0x6D ? K::Configure : K::Invalid;
    case 0x0E: return n == 4 && b[2] == 0x52 ? K::Configure : K::Invalid;
    case 0x46: return n == 6 && b[2] == 0x69 && b[3] <= 1 && b[4] <= 1 ? K::Configure : K::Invalid;
    // Manual V1.0.5 p82 (5.6.13), X firmware: the global closed-loop maximum
    // phase current is [addr][45][66][save 00/01][current u16][6B], 7 bytes, and
    // the documented field range is 0000-1388 (0-5000 mA). It is a global
    // ceiling on the running current in every state - not the 4C collision
    // detection threshold and not homing-only - and it is a parameter write, so
    // the configured current policy applies on top of the field range. The
    // policy reason is reported separately (see closedLoopCurrentRefusal).
    case 0x45: return n == 7 && b[2] == 0x66 && b[3] <= 1 &&
        word(b + 4) <= kClosedLoopCurrentMaxMa &&
        word(b + 4) <= limits.maxCurrentMa ? K::Configure : K::Invalid;
    case 0x93: return n == 5 && b[2] == 0x88 && b[3] <= 1 ? K::Configure : K::Invalid;
    case 0x11: return n == 7 && b[2] == 0x18 && b[3] == 0x36 &&
        (word(b+4) == 0 || word(b+4) >= 30) ? K::Configure : K::Invalid;
    // V1.0.5 pp64-65. Documented ranges instead of the previous arbitrary
    // caps: homing velocity 0000-0BB8 RPM and a uint32 timeout are legal on the
    // wire; the collision-detection current additionally follows the current
    // policy, and the speed field follows the speed policy in whole RPM.
    // Power-on homing stays unarmed for now (see homeParamRefusal).
    case 0x4C: return n == 20 && b[2] == 0xAE && b[3] <= 1 && b[4] <= kHomeModeMax && b[5] <= 1 &&
        word(b+6) <= kHomeVelocityMaxRpm &&
        static_cast<uint32_t>(word(b+6)) * 10u <= limits.maxSpeedTenths &&
        word(b+14) <= limits.maxCurrentMa &&
        b[18] == 0 ? K::Configure : K::Invalid;
    case 0xCD: return n == 18 && b[2] <= 1 && b[13] == 2 && b[14] == 0 ? K::Move : K::Invalid;
    // Manual V1.0.5 pp54-55: direct (passthrough) position, X firmware. FB is
    // 12 bytes, CB adds the max-current field for 14 ([addr][CB][dir][speed u16]
    // [angle u32][mode][sync][current u16][6B]). The opcode is preserved exactly
    // as documented - nothing is translated to the trapezoid CD form - and all
    // three documented motion modes are allowed. Only the immediate form
    // (sync 00) is supervised; the cached form still needs the FF trigger, so it
    // is refused with its own reason (see directPositionRefusal). Numeric policy
    // (speed, current, travel) needs the resolved target and stays in
    // buildDirectPositionPlan()/resolveDirectTarget().
    case 0xFB: return n == 12 && b[2] <= 1 && b[9] <= kMaxMotionMode && b[10] == 0
        ? K::DirectMove : K::Invalid;
    case 0xCB: return n == 14 && b[2] <= 1 && b[9] <= kMaxMotionMode && b[10] == 0
        ? K::DirectMove : K::Invalid;
    case 0xF5: case 0xC5:
        return n == (b[1] == 0xC5 ? 11 : 9) && b[2] <= 1 && b[7] == 0 &&
            word(b+5) <= limits.maxCurrentMa && (b[1] != 0xC5 || word(b+8) <= limits.maxSpeedTenths) ? K::Experiment : K::Invalid;
    case 0xF6: case 0xC6:
        return n == (b[1] == 0xC6 ? 11 : 9) && b[2] <= 1 && b[7] == 0 &&
            word(b+3) >= 1 && word(b+3) <= limits.maxAccelRpmS && word(b+5) <= limits.maxSpeedTenths &&
            (b[1] != 0xC6 || (word(b+8) >= 100 && word(b+8) <= limits.maxCurrentMa)) ? K::Experiment : K::Invalid;
    default: return K::Invalid;
    }
}

// Why a well-formed 0x4C homing-parameter write was refused, or nullptr when it
// is acceptable. Keeps the refusal reasons honest instead of collapsing every
// rejection into "unsupported_or_invalid_command": the power-on trigger and the
// policy bounds are reported separately from layout errors.
inline const char* homeParamRefusal(const uint8_t* b, uint8_t n, const DebugLimits& limits = DebugLimits{}) {
    if (!b || n != 20 || b[1] != 0x4C) return nullptr;
    // Layout errors keep the generic message: they are malformed frames, not
    // policy decisions.
    if (b[2] != 0xAE || b[3] > 1 || b[4] > kHomeModeMax || b[5] > 1) return nullptr;
    if (word(b + 14) > limits.maxCurrentMa) return "home_current_out_of_range";
    if (word(b + 6) > kHomeVelocityMaxRpm) return "home_velocity_out_of_range";
    if (static_cast<uint32_t>(word(b + 6)) * 10u > limits.maxSpeedTenths) return "home_velocity_out_of_policy";
    if (b[18] != 0) return "power_on_homing_not_supported";
    return nullptr;
}

// Why a well-formed 0x45 closed-loop current write was refused, or nullptr when
// it is acceptable. Layout errors (length, auxiliary byte 0x66, save 00/01) and
// the documented 0-5000 mA field bound keep the generic answer: they are
// malformed frames or protocol range errors, not a policy decision. Only the
// configured current policy gets this dedicated reason.
inline const char* closedLoopCurrentRefusal(
    const uint8_t* b, uint8_t n, const DebugLimits& limits = DebugLimits{}) {
    if (!b || n != 7 || b[1] != 0x45) return nullptr;
    if (b[2] != 0x66 || b[3] > 1) return nullptr;
    const uint16_t current = word(b + 4);
    if (current > kClosedLoopCurrentMaxMa) return nullptr;
    if (current > limits.maxCurrentMa) return "current_out_of_range";
    return nullptr;
}

// Why a well-formed immediate FB/CB frame was refused, or nullptr when the
// controller should evaluate it. Only the cached (sync 01) form is answered
// here: it is a documented command this board does not supervise, so the reason
// says so instead of collapsing into "unsupported_or_invalid_command". Value
// policy is not decided here - it needs the resolved target and the configured
// limits, and buildDirectPositionPlan()/resolveDirectTarget() already report
// those reasons precisely.
inline const char* directPositionRefusal(const uint8_t* b, uint8_t n) {
    // Never read a byte the caller did not provide: the function code itself
    // needs two bytes, and validateCommand()'s structural rules (a real address
    // and the fixed checksum) are checked here too. A malformed frame keeps the
    // generic answer instead of being reported as a sync-flag policy decision.
    if (!b || n < 3 || b[0] == 0 || b[n - 1] != 0x6B) return nullptr;
    if (b[1] != 0xFB && b[1] != 0xCB) return nullptr;
    // Only the exact documented lengths reach the fields below.
    if (n != (b[1] == 0xCB ? 14 : 12)) return nullptr;
    if (b[2] > 1 || b[9] > kMaxMotionMode) return nullptr;
    if (b[10] != 0) return "direct_sync_not_supported";
    return nullptr;
}
} // namespace motion
