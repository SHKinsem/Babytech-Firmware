#pragma once

#include <ArduinoJson.h>
#include <cstring>

namespace motion {
enum class CloudConfigKind { Invalid, Context, Receipt, SessionProbe };

inline CloudConfigKind cloudConfigKind(const uint8_t* payload, size_t length,
                                       const char* deviceId, bool allowSessionProbe = false) {
    DynamicJsonDocument document(3072);
    if (deserializeJson(document, payload, length) ||
        std::strcmp(document["device_id"] | "", deviceId) != 0)
        return CloudConfigKind::Invalid;
    if (document["type"] == "feeding_context") return CloudConfigKind::Context;
    if (document["type"] == "feeding_event_receipt") return CloudConfigKind::Receipt;
    if (allowSessionProbe && document["type"] == "command_session_probe")
        return CloudConfigKind::SessionProbe;
    return CloudConfigKind::Invalid;
}
}  // namespace motion
