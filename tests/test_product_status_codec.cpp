#include "ProductStatusCodec.h"

#include <cassert>
#include <cstdio>
#include <cstring>

void verify(const motion::ProductStatusSnapshot& snapshot, bool ready) {
    DynamicJsonDocument document(3072);
    motion::writeProductStatus(document.to<JsonObject>(), snapshot);
    assert(!document.overflowed());
    char payload[2048];
    const size_t length = serializeJson(document, payload, sizeof(payload));
    assert(length > 0 && length < sizeof(payload));
    DynamicJsonDocument parsed(3072);
    assert(!deserializeJson(parsed, payload, length));
    assert(std::strcmp(parsed["device_id"], snapshot.deviceId.c_str()) == 0);
    assert(parsed["powder_recipe_version"] == 2);
    assert(std::strcmp(parsed["progress"], ready ? "ready" : "noready") == 0);
    assert(parsed["low_water"].isNull() != ready);
    assert(std::strcmp(parsed["water_status"], ready ? "normal" : "unknown") == 0);
    assert(std::strcmp(parsed["powder_status"], !snapshot.powderValid ? "unknown" :
                        (snapshot.powderGrams > 50 ? "normal" : "low")) == 0);
    assert(parsed["thermal_simulated"] == true);
    assert(parsed["measured_water_temp"].isNull());
    assert(parsed["is_water_ready"] == snapshot.isWaterReady);
    assert(parsed["dispensed_water_ml"].isNull());
    assert(std::strcmp(parsed["water_delivery_basis"], "estimated_turns") == 0);
    assert(parsed["actuator_dosing_stub"] == true);
    assert(std::strcmp(parsed["actuator_issue"], ready ? "none" : "non_consumable_demo_disabled") == 0);
    assert(parsed["bottle_presence_sensor_enabled"] == false);
    assert(parsed["bottle_state_valid"] == false);
    assert(std::strcmp(parsed["bottle_clamp_status"], "unknown") == 0);
    assert(parsed["bottle_present_at_load_position"].isNull());
}

int main() {
    motion::ProductStatusSnapshot snapshot;
    snapshot.deviceId = "bt-184DCE6E27AC";
    snapshot.firmwareVersion = "motion-v1-test";
    assert(!snapshot.isWaterReady);
    verify(snapshot, false);

    snapshot.progress = "ready";
    snapshot.lowWaterValid = true;
    snapshot.powderValid = true;
    snapshot.powderGrams = 350;
    snapshot.executionAuthorized = true;
    snapshot.canStart = true;
    snapshot.actuatorOperational = true;
    snapshot.actuatorConfigValid = true;
    snapshot.actuatorBusHealthy = true;
    snapshot.actuatorPositionReferenced = true;
    snapshot.feedingContextConfigured = true;
    snapshot.babyId.assign(96, 'b');
    snapshot.babyName.assign(320, 'n');
    snapshot.mqttHost.assign(127, 'h');
    snapshot.ipAddress = "192.168.255.255";
    snapshot.mqttPort = 1883;
    snapshot.mqttAuthEnabled = true;
    verify(snapshot, true);
    snapshot.powderGrams = 50;
    verify(snapshot, true);
    snapshot.powderValid = false;
    verify(snapshot, true);
    for (const char* progress : {"complete", "mixing", "error"}) {
        snapshot.progress = progress;
        DynamicJsonDocument document(3072);
        motion::writeProductStatus(document.to<JsonObject>(), snapshot);
        assert(document["bottle_presence_sensor_enabled"] == false);
        assert(document["bottle_state_valid"] == false);
        assert(std::strcmp(document["bottle_clamp_status"], "unknown") == 0);
        assert(document["bottle_present_at_load_position"].isNull());
    }
    std::puts("PASS product status JSON codec and MQTT payload bound");
}
