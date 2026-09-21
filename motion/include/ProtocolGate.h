#pragma once
#include <stdint.h>

namespace motion {
enum class CommandKind { Invalid, Read, Configure, Enable, Stop, Move, Experiment, Interrupt };
inline uint16_t word(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }
inline uint32_t dword(const uint8_t* p) { return (uint32_t(word(p)) << 16) | word(p + 2); }

// Strict whitelist, shared by controller and host tests. Unknown layouts cannot
// bypass the controller through the HEX editor. Synchronous queues and modes
// without a documented position interpretation remain preview-only.
inline CommandKind validateCommand(const uint8_t* b, uint8_t n) {
    using K = CommandKind;
    if (!b || n < 3 || !b[0] || b[n-1] != 0x6B) return K::Invalid;
    switch (b[1]) {
    case 0x1F: case 0x20: case 0x21: case 0x24: case 0x27: case 0x31:
    case 0x33: case 0x35: case 0x36: case 0x37: case 0x3A: case 0x3B:
        return n == 3 ? K::Read : K::Invalid;
    case 0x42: return n == 4 && b[2] == 0x6C ? K::Read : K::Invalid;
    case 0x43: return n == 4 && b[2] == 0x7A ? K::Read : K::Invalid;
    case 0xF3: return n == 6 && b[2] == 0xAB && b[3] <= 1 && b[4] == 0 ? K::Enable : K::Invalid;
    case 0xFE: return n == 5 && b[2] == 0x98 && b[3] == 0 ? K::Stop : K::Invalid;
    case 0x9C: return n == 4 && b[2] == 0x48 ? K::Interrupt : K::Invalid;
    case 0x0A: return n == 4 && b[2] == 0x6D ? K::Configure : K::Invalid;
    case 0x0E: return n == 4 && b[2] == 0x52 ? K::Configure : K::Invalid;
    case 0x46: return n == 6 && b[2] == 0x69 && b[3] <= 1 ? K::Configure : K::Invalid;
    case 0x93: return n == 5 && b[2] == 0x88 && b[3] <= 1 ? K::Configure : K::Invalid;
    case 0x11: return n == 7 && b[2] == 0x18 && b[3] == 0x36 &&
        (word(b+4) == 0 || word(b+4) >= 30) ? K::Configure : K::Invalid;
    case 0x4C: return n == 20 && b[2] == 0xAE && b[3] <= 1 && b[5] <= 1 &&
        word(b+6) <= 1200 && dword(b+8) <= 60000 && word(b+14) <= 5000 &&
        b[18] == 0 ? K::Configure : K::Invalid; // never arm power-on homing
    case 0xCD: return n == 18 && b[2] <= 1 && b[13] == 2 && b[14] == 0 ? K::Move : K::Invalid;
    case 0xF5: case 0xC5:
        return n == (b[1] == 0xC5 ? 11 : 9) && b[2] <= 1 && b[7] == 0 &&
            word(b+5) <= 5000 && (b[1] != 0xC5 || word(b+8) <= 1200) ? K::Experiment : K::Invalid;
    case 0xF6: case 0xC6:
        return n == (b[1] == 0xC6 ? 11 : 9) && b[2] <= 1 && b[7] == 0 &&
            word(b+3) >= 1 && word(b+3) <= 240 && word(b+5) <= 1200 &&
            (b[1] != 0xC6 || (word(b+8) >= 100 && word(b+8) <= 5000)) ? K::Experiment : K::Invalid;
    default: return K::Invalid;
    }
}
} // namespace motion
