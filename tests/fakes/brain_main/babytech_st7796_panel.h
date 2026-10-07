#pragma once
#include "FakeMainIo.h"

namespace babytech { namespace display {
class BabytechSt7796Panel {
public:
    bool begin() { return fake_main::panelReady; }
};
} }
