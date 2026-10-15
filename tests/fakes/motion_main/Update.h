#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

constexpr int U_FLASH = 0;

namespace fake_motion_ota {
// SDK/HTTP wiring only: no flash, image validation or successful OTA staging.
struct UpdateState {
    bool allowBegin = false;
    bool running = false;
    size_t expected = 0;
    size_t received = 0;
    size_t writeLimit = std::numeric_limits<size_t>::max();
    unsigned beginCalls = 0;
    unsigned writeCalls = 0;
    unsigned endCalls = 0;
    unsigned abortCalls = 0;
};
inline UpdateState update;
} // namespace fake_motion_ota

class UpdateClass {
public:
    bool isRunning() const { return fake_motion_ota::update.running; }
    bool begin(size_t size, int command = U_FLASH) {
        auto& state = fake_motion_ota::update;
        ++state.beginCalls;
        if (!state.allowBegin || state.running || !size || command != U_FLASH) return false;
        state.expected = size;
        state.received = 0;
        state.running = true;
        return true;
    }
    size_t write(uint8_t* bytes, size_t size) {
        auto& state = fake_motion_ota::update;
        ++state.writeCalls;
        if (!state.running || (!bytes && size) || state.received > state.expected ||
            size > state.expected - state.received) return 0;
        const size_t count = size < state.writeLimit ? size : state.writeLimit;
        state.received += count;
        return count;
    }
    bool end(bool = false) {
        auto& state = fake_motion_ota::update;
        ++state.endCalls;
        state.running = false;
        return false; // Deliberately never claim that a flash image was validated.
    }
    void abort() {
        ++fake_motion_ota::update.abortCalls;
        fake_motion_ota::update.running = false;
    }
};

inline UpdateClass Update;

