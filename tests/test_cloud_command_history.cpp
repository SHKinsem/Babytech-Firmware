#include "CloudCommandHistory.h"

#include <cassert>
#include <cstdio>

int main() {
    motion::CloudCommandHistory history;
    assert(history.find("prepare-1") == nullptr);
    history.remember("prepare-1", "accepted");
    history.remember("stop-1", "stopped");
    assert(*history.find("prepare-1") == "accepted");
    assert(*history.find("stop-1") == "stopped");

    for (size_t i = 0; i < motion::CloudCommandHistory::kCapacity; ++i)
        history.remember("command-" + std::to_string(i), "rejected");
    assert(history.find("prepare-1") == nullptr);
    assert(history.find("stop-1") == nullptr);
    assert(*history.find("command-0") == "rejected");
    std::puts("PASS cloud command ACK history");
}
