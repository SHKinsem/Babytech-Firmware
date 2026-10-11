#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace babytech { namespace boardlink {

// Bounded console, including hex-encoded local network credentials.
// Import payloads are deliberately not accepted yet.
// The caller owns Serial and processes at most kPollBytes per loop.
class MaintenanceLineReader {
public:
    static constexpr size_t kCapacity = 768;
    static constexpr size_t kPollBytes = 64;
    static constexpr uint32_t kTimeoutMs = 2000;
    enum class Result { None, Line, Rejected };

    Result feed(uint8_t byte, uint32_t nowMs) {
        if (length_ && uint32_t(nowMs - lastByteAt_) >= kTimeoutMs) dropping_ = true;
        lastByteAt_ = nowMs;
        if (byte == '\r' || byte == '\n') {
            const bool rejected = dropping_;
            dropping_ = false;
            line_[length_] = '\0';
            const bool complete = length_ != 0;
            length_ = 0;
            if (rejected) { clear(); return Result::Rejected; }
            return complete ? Result::Line : Result::None;
        }
        if (dropping_) return Result::None;
        if (byte < 0x20 || byte > 0x7e || length_ == kCapacity - 1) {
            dropping_ = true;
            return Result::None;
        }
        line_[length_++] = static_cast<char>(byte);
        return Result::None;
    }
    // Valid only until the next feed(). Never echo rejected/unknown input.
    const char* line() const { return line_; }
    void clear() {
        volatile char* p = line_;
        for (size_t i = 0; i < sizeof(line_); ++i) p[i] = 0;
    }

private:
    char line_[kCapacity]{};
    size_t length_ = 0;
    uint32_t lastByteAt_ = 0;
    bool dropping_ = false;
};

class MaintenanceSession {
public:
    // localSafe excludes local in-flight work. It is NOT an import guard or
    // proof of peer state, MQTT handoff, operator approval or storage health.
    const char* handle(const char* line, bool localSafe) {
        if (!line) return nullptr;
        if (!std::strcmp(line, "MAINT STATUS")) return active_ ? "active" : "inactive";
        if (!std::strcmp(line, "MAINT BEGIN")) {
            if (active_) return "active";
            if (!localSafe) return "unsafe";
            active_ = true;
            return "active";
        }
        if (!std::strcmp(line, "MAINT END")) {
            if (!active_) return "inactive";
            if (!localSafe) return "unsafe";
            active_ = false;
            return "inactive";
        }
        return nullptr;
    }
    bool active() const { return active_; }

private:
    bool active_ = false;
};

} }
