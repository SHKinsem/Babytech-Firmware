#include "CloudInboundOrder.h"

#include <cassert>

int main() {
    using motion::CloudInboundLane;
    using motion::selectCloudInbound;
    assert(selectCloudInbound(true, true, true) == CloudInboundLane::Stop);
    assert(selectCloudInbound(true, false, false) == CloudInboundLane::Stop);
    assert(selectCloudInbound(false, true, true) == CloudInboundLane::Config);
    assert(selectCloudInbound(false, true, false) == CloudInboundLane::Config);
    assert(selectCloudInbound(false, false, true) == CloudInboundLane::Command);
    assert(selectCloudInbound(false, false, false) == CloudInboundLane::None);
}
