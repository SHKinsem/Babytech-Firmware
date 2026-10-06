#include "ProductStatusCodec.h"

namespace motion {

void writeProductStatus(JsonObject target, const ProductStatusSnapshot& snapshot) {
    target["device_id"] = snapshot.deviceId;
    target["firmware_mode"] = "cloud";
    target["firmware_version"] = snapshot.firmwareVersion;
    target["hardware_profile"] = "real";
    target["topic_mode"] = "namespaced";
    target["powder_recipe_version"] = 2;
    target["progress"] = snapshot.progress;
    target["is_preparing"] = snapshot.isPreparing;
    target["error_code"] = snapshot.errorCode;
    if (snapshot.progress == "error")
        target["error_message"] = "Motion requires inspection or initialization";
    target["water_temp"] = snapshot.targetTemp;
    target["measured_water_temp"] = nullptr;
    target["thermal_simulated"] = true;
    target["target_temp"] = snapshot.targetTemp;
    target["is_water_ready"] = snapshot.isWaterReady;
    target["is_heating"] = false;
    target["is_cooling"] = false;
    if (snapshot.lowWaterValid) target["low_water"] = snapshot.lowWater;
    else target["low_water"] = nullptr;
    target["water_status"] = !snapshot.lowWaterValid ? "unknown" :
        (snapshot.lowWater ? "low" : "normal");
    target["water_remained"] = nullptr;
    target["powder_remained"] = snapshot.powderGrams;
    target["powder_status"] = !snapshot.powderValid ? "unknown" :
        (snapshot.powderGrams > 50 ? "normal" : "low");
    target["bottle_presence_sensor_enabled"] = false;
    target["bottle_state_valid"] = false;
    target["bottle_present_at_load_position"] = nullptr;
    target["bottle_clamp_status"] = snapshot.progress == "complete" ? "full" : "empty";
    target["cap_hall_detected"] = nullptr;
    target["dispensed_water_ml"] = nullptr;
    target["water_delivery_basis"] = "estimated_turns";
    target["actuator_operational"] = snapshot.actuatorOperational;
    target["actuator_config_valid"] = snapshot.actuatorConfigValid;
    target["actuator_bus_healthy"] = snapshot.actuatorBusHealthy;
    target["actuator_position_referenced"] = snapshot.actuatorPositionReferenced;
    target["actuator_dosing_stub"] = true;
    target["actuator_schema_version"] = 0;
    target["actuator_profile_revision"] = 0;
    target["actuator_issue"] = !snapshot.executionAuthorized ? "non_consumable_demo_disabled" :
        (snapshot.canStart ? "none" : "not_ready");
    target["feeding_context_configured"] = snapshot.feedingContextConfigured;
    target["feeding_context_baby_id"] = snapshot.babyId;
    target["feeding_context_baby_name"] = snapshot.babyName;
    target["feeding_context_profile_version"] = snapshot.profileVersion;
    target["ip_address"] = snapshot.ipAddress;
    target["mqtt_host"] = snapshot.mqttHost;
    target["mqtt_port"] = snapshot.mqttPort;
    target["mqtt_auth_enabled"] = snapshot.mqttAuthEnabled;
    target["mqtt_tls_enabled"] = false;
    target["wifi_rssi"] = snapshot.wifiRssi;
}

}  // namespace motion
