#pragma once

#include <ArduinoJson.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace motion {

// Ignore unrelated command fields so a valid Stop stays ahead of queued work
// even when its JSON payload is larger than the priority parser's capacity.
inline bool isPriorityStopCommand(const uint8_t* payload, size_t length,
                                  const char* deviceId) {
    if (!payload || !deviceId) return false;
    StaticJsonDocument<128> filter;
    filter["command"] = true;
    filter["command_id"] = true;
    filter["device_id"] = true;
    StaticJsonDocument<512> command;
    if (deserializeJson(command, payload, length, DeserializationOption::Filter(filter)))
        return false;
    const char* action = command["command"] | "";
    const char* id = command["command_id"] | "";
    const char* target = command["device_id"] | "";
    return std::strcmp(action, "stop") == 0 && id[0] && std::strlen(id) <= 128 &&
        std::strcmp(target, deviceId) == 0;
}

}  // namespace motion
