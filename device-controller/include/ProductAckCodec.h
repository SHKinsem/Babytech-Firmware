#pragma once

#include <ArduinoJson.h>
#include <string>

namespace motion {

struct ProductAckSnapshot {
    std::string commandId;
    std::string command;
    std::string deviceId;
    bool accepted = false;
    std::string status;
    std::string reason;
    std::string progress;
    std::string errorCode;
};

void writeProductAck(JsonObject target, const ProductAckSnapshot& snapshot);

}  // namespace motion
