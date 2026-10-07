#pragma once

#include "brain_simulation.h"
#include <cstdio>

namespace babytech { namespace brain {

// Pure USB command parser; main must explicitly route it to the simulation
// owner. No maintenance session, real-operation cancellation or UART command.
class BrainSimulationConsole {
public:
    static bool handle(const char* line, BrainSimulation& simulation, bool realRequestUnresolved,
                       char* output, size_t capacity) {
        if (!line || std::strncmp(line, "SIM ", 4)) return false;
        if (!output || !capacity) return true;
        if (!std::strcmp(line, "SIM STATUS")) {
            std::snprintf(output, capacity, "[simulation] %s running=%s pending=%u duration_ms=%lu ram_only\n",
                simulation.enabled() ? "on" : "off", simulation.running() ? "yes" : "no",
                unsigned(simulation.resultCount()), static_cast<unsigned long>(simulation.durationMs()));
            return true;
        }
        if (std::strcmp(line, "SIM ON") && std::strcmp(line, "SIM OFF")) {
            std::snprintf(output, capacity, "[simulation] unknown_command\n");
            return true;
        }
        const bool enabled = !std::strcmp(line, "SIM ON");
        const auto result = simulation.setEnabled(enabled, realRequestUnresolved);
        std::snprintf(output, capacity, "[simulation] %s\n",
            result == SimulationModeResult::Busy ? "busy" : (enabled ? "on" : "off"));
        return true;
    }
};

} }
