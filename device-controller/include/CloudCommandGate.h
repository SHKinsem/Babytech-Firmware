#pragma once

#include <cstdint>
#include <cstring>

namespace motion {

inline bool cloudCommandAllowedDuringOta(const char* command) {
    return command && std::strcmp(command, "stop") == 0;
}

inline bool cloudInboundEligible(bool wifiConnected, bool mqttConnected,
                                 uint32_t messageGeneration, uint32_t currentGeneration) {
    return wifiConnected && mqttConnected && messageGeneration == currentGeneration;
}

}  // namespace motion
