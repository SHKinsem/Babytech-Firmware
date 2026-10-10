#pragma once

#include "BoardDiscovery.h"
#include <cstdio>
#include <cstring>

namespace babytech { namespace brain {

class BrainPairingConsole {
public:
    template<class Link>
    static bool handle(const char* line, bool maintenance, Link& link, uint32_t nowMs,
                       char* output, size_t capacity) {
        if (!line || std::strncmp(line, "PAIR ", 5)) return false;
        const auto reply = [&](const char* text) {
            std::snprintf(output, capacity, "[pair] %s\n", text);
            return true;
        };
        if (!std::strcmp(line, "PAIR STATUS")) {
            const auto& result = link.discoveryResult();
            const char* state = "idle";
            switch (result.state) {
                case boardlink::DiscoveryState::Idle: break;
                case boardlink::DiscoveryState::Pending: state = "pending"; break;
                case boardlink::DiscoveryState::Found: state = "found"; break;
                case boardlink::DiscoveryState::Conflict: state = "identity_conflict"; break;
                case boardlink::DiscoveryState::Unavailable: state = "pairing_record_unavailable"; break;
                case boardlink::DiscoveryState::TimedOut: state = "timed_out"; break;
            }
            std::snprintf(output, capacity, "[pair] %s motion=%s pairing=%u\n", state,
                          result.physicalId[0] ? result.physicalId : "unknown",
                          unsigned(result.pairingState));
            return true;
        }
        if (!std::strcmp(line, "PAIR RECORDS")) {
            const char* state = "records_idle";
            switch (link.recordsState()) {
                case boardlink::ExportTransferState::Idle: break;
                case boardlink::ExportTransferState::Pending: state = "records_pending"; break;
                case boardlink::ExportTransferState::Complete: state = "records_complete"; break;
                case boardlink::ExportTransferState::Unavailable: state = "records_unavailable"; break;
                case boardlink::ExportTransferState::Invalid: state = "records_invalid"; break;
                case boardlink::ExportTransferState::TimedOut: state = "records_timed_out"; break;
            }
            return reply(state);
        }
        if (!std::strcmp(line, "PAIR HOLD")) {
            const char* state = "hold_idle";
            switch (link.maintenanceState()) {
                case boardlink::BoardMaintenanceState::Idle: break;
                case boardlink::BoardMaintenanceState::Pending: state = "hold_pending"; break;
                case boardlink::BoardMaintenanceState::Active: state = "hold_active"; break;
                case boardlink::BoardMaintenanceState::Releasing: state = "hold_releasing"; break;
                case boardlink::BoardMaintenanceState::Released: state = "hold_released"; break;
                case boardlink::BoardMaintenanceState::Unsafe: state = "hold_unsafe"; break;
                case boardlink::BoardMaintenanceState::Busy: state = "hold_busy"; break;
                case boardlink::BoardMaintenanceState::Unavailable: state = "hold_unavailable"; break;
                case boardlink::BoardMaintenanceState::TimedOut: state = "hold_timed_out"; break;
            }
            return reply(state);
        }
        if (!std::strcmp(line, "PAIR RELEASE"))
            return reply(link.releaseMaintenance(nowMs) ? "hold_releasing" : "hold_unavailable");
        if (!maintenance) return reply("maintenance_required");
        if (!std::strncmp(line, "PAIR DISCOVER ", 14))
            return reply(link.requestDiscovery(line + 14, nowMs) ? "discovery_pending" : "discovery_unavailable");
        if (!std::strncmp(line, "PAIR READ ", 10))
            return reply(link.requestRecords(line + 10, nowMs) ? "records_pending" : "records_unavailable");
        if (!std::strncmp(line, "PAIR HOLD ", 10))
            return reply(link.requestMaintenance(line + 10, nowMs) ? "hold_pending" : "hold_unavailable");
        return reply("unknown_command");
    }
};

} }
