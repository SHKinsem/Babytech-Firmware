#pragma once

#include <ArduinoJson.h>
#include <cstdint>
#include <string>

namespace motion {

struct ProductStatusSnapshot {
    std::string deviceId;
    std::string firmwareVersion;
    std::string progress = "noready";
    std::string errorCode = "NONE";
    bool isPreparing = false;
    int targetTemp = 45;
    bool isWaterReady = false;
    bool lowWaterValid = false;
    bool lowWater = false;
    bool powderValid = false;
    int powderGrams = 0;
    bool actuatorOperational = false;
    bool actuatorConfigValid = false;
    bool actuatorBusHealthy = false;
    bool actuatorPositionReferenced = false;
    bool executionAuthorized = false;
    bool canStart = false;
    bool feedingContextConfigured = false;
    std::string babyId;
    std::string babyName;
    uint32_t profileVersion = 0;
    std::string ipAddress;
    std::string mqttHost;
    uint16_t mqttPort = 0;
    bool mqttAuthEnabled = false;
    int wifiRssi = 0;
};

void writeProductStatus(JsonObject target, const ProductStatusSnapshot& snapshot);

}  // namespace motion
