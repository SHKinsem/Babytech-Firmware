#include "CloudCommandPriority.h"

#include <cassert>
#include <cstdio>
#include <string>

namespace {
constexpr char kDeviceId[] = "bt-184DCE6E27AC";

bool priority(const std::string& payload) {
    return motion::isPriorityStopCommand(
        reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), kDeviceId);
}
}

int main() {
    assert(priority(R"({"command":"stop","command_id":"cmd-1","device_id":"bt-184DCE6E27AC"})"));
    const std::string longPayload =
        R"({"command":"stop","command_id":"cmd-2","device_id":"bt-184DCE6E27AC","extra":")" +
        std::string(900, 'x') + R"("})";
    assert(longPayload.size() > 256);
    assert(longPayload.size() < 1536);
    assert(priority(longPayload));
    assert(!priority(R"({"command":"prepare","command_id":"cmd-1","device_id":"bt-184DCE6E27AC"})"));
    assert(!priority(R"({"command":"stop","command_id":"cmd-1","device_id":"bt-other"})"));
    assert(!priority(R"({"command":"stop","device_id":"bt-184DCE6E27AC"})"));
    assert(!priority(R"({"command":"stop","command_id":"cmd-1","device_id":"bt-184DCE6E27AC")"));
    std::puts("PASS cloud Stop command priority");
}
