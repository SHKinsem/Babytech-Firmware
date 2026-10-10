#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace babytech { namespace brain {

class BrainInstallConsole {
public:
    template<class Installer>
    static bool handle(const char* line, Installer& installer, uint32_t nowMs,
                       char* output, size_t capacity) {
        if (!line || std::strncmp(line, "PAIR INSTALL", 12)) return false;
        if (!std::strcmp(line, "PAIR INSTALL STATUS") ||
            !std::strcmp(line, "PAIR INSTALL CANCEL")) {
            if (!std::strcmp(line, "PAIR INSTALL CANCEL")) installer.cancel(nowMs);
            std::snprintf(output, capacity, "[install] %s reason=%s writes_may_have_persisted=%u\n",
                          installer.status(), installer.reason(), unsigned(installer.mayHaveWritten()));
            return true;
        }
        char device[65]{};
        const char* at = line + 12;
        size_t length = 0;
        if (*at == ' ') {
            ++at;
            while (*at && *at != ' ' && length < sizeof(device) - 1) device[length++] = *at++;
        }
        // Operator assertion after the one-time broker/session handoff. Neither
        // this token nor a v4 build proves the old credential was revoked.
        const bool confirmed = length && !std::strcmp(at, " HANDOFF_CONFIRMED");
        const bool started = confirmed && installer.start(device, true, nowMs);
        std::snprintf(output, capacity, "[install] %s\n", started ? "started" : "not_started");
        return true;
    }
};

} }
