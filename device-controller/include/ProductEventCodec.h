#pragma once

#include <ArduinoJson.h>
#include <cstdint>
#include "ProductSession.h"

namespace motion {

void writeProductTerminalEvent(JsonObject target, const ProductTerminal& terminal,
                               const char* deviceId, const char* eventId, uint32_t uptimeMs);

}  // namespace motion
