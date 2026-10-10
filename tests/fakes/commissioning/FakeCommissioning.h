#pragma once
#include "FakeBrainNvs.h"
#include <array>

namespace fake_commissioning {
extern std::array<uint8_t, 6> mac;
extern unsigned macCalls;
extern unsigned failMacCall;
extern unsigned stringQueries;
extern unsigned stringReads;
void reset();
}
