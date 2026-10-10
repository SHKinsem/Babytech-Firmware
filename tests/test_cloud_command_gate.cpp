#include "CloudCommandGate.h"

#include <cassert>

int main() {
    assert(motion::cloudCommandAllowedDuringOta("stop"));
    assert(!motion::cloudCommandAllowedDuringOta("prepare"));
    assert(!motion::cloudCommandAllowedDuringOta("clean"));
    assert(!motion::cloudCommandAllowedDuringOta("set_target_temp"));
    assert(!motion::cloudCommandAllowedDuringOta("reset_error"));
    assert(!motion::cloudCommandAllowedDuringOta("check_firmware_update"));
    assert(!motion::cloudCommandAllowedDuringOta(""));
    assert(!motion::cloudCommandAllowedDuringOta(nullptr));
    assert(motion::cloudInboundEligible(true, true, 7, 7));
    assert(!motion::cloudInboundEligible(false, true, 7, 7));
    assert(!motion::cloudInboundEligible(true, false, 7, 7));
    assert(!motion::cloudInboundEligible(true, true, 6, 7));
}
