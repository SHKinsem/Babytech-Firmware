#pragma once

#include <ArduinoJson.h>
#include "ProductBoardMessages.h"
#include "CloudSession.h"
#include "ProductContext.h"

namespace babytech { namespace brain {

struct SimulationStatus {
    bool commandsEnabled = false;
    bool canStart = false;
    bool running = false;
    bool complete = false;
    const boardlink::ProductRequest* request = nullptr;
    const boardlink::ProductContext* context = nullptr;
};

// Compact ArduinoJson 6 does not escape every ASCII control byte. Validate
// capacity after escaping, before writing, so identities are never truncated.
bool encodeStatusJson(const JsonDocument& document, char* output, size_t capacity);

// No mechanical inference or mutation: full identifiers come from telemetry,
// while names are explicitly the short display labels until context sync lands.
// motionConnected means fresh evidence. Capability/start flags are explicit
// runtime decisions, not inferred from display startEnabled or MQTT online.
void writeStatus(JsonObject output, const char* deviceId, const char* firmwareVersion,
                 const boardlink::Status* lastMotion, bool motionConnected,
                 const cloud::SessionSnapshot& session, const char* challenge = nullptr,
                 bool commandsEnabled = false, bool canStart = false);

void writeSimulationStatus(JsonObject output, const char* deviceId, const char* firmwareVersion,
                           const cloud::SessionSnapshot& session, const SimulationStatus& simulation,
                           const char* challenge = nullptr);

} }
