#include "brain_status.h"
#include <cstring>

namespace babytech { namespace brain {
namespace {
class EscapedWriter {
public:
    explicit EscapedWriter(char* output = nullptr) : output_(output) {}
    size_t write(uint8_t byte) {
        if (byte < 0x20) {
            static constexpr char hex[] = "0123456789abcdef";
            const char escaped[] = {'\\', 'u', '0', '0', hex[byte >> 4], hex[byte & 15]};
            if (output_) std::memcpy(output_ + size_, escaped, sizeof(escaped));
            size_ += sizeof(escaped);
        } else {
            if (output_) output_[size_] = char(byte);
            ++size_;
        }
        return 1;
    }
    size_t write(const uint8_t* bytes, size_t size) {
        for (size_t index = 0; index < size; ++index) write(bytes[index]);
        return size;
    }
    size_t size() const { return size_; }
private:
    char* output_;
    size_t size_ = 0;
};
}

bool encodeStatusJson(const JsonDocument& document, char* output, size_t capacity) {
    if (!output || document.overflowed()) return false;
    EscapedWriter counter;
    serializeJson(document, counter);
    if (counter.size() >= capacity) return false;
    EscapedWriter writer(output);
    serializeJson(document, writer);
    output[writer.size()] = 0;
    return true;
}

void writeStatus(JsonObject out, const char* deviceId, const char* firmwareVersion,
                 const boardlink::Status* last, bool connected,
                 const cloud::SessionSnapshot& session, const char* challenge,
                 bool commandsEnabled, bool canStart) {
    connected = connected && last;
    commandsEnabled = commandsEnabled && connected;
    canStart = canStart && commandsEnabled;
    const bool knownError = last && std::strcmp(last->productError, "NONE") != 0;
    const char* progress = connected ? last->productProgress : knownError ? "error" : "noready";
    // Old App versions also use progress, so Ready needs explicit start permission.
    if (!canStart && !std::strcmp(progress, "ready")) progress = "noready";
    out["device_id"] = deviceId;
    out["firmware_mode"] = "cloud";
    out["firmware_version"] = firmwareVersion;
    out["hardware_profile"] = "real";
    out["topic_mode"] = "namespaced";
    out["command_protocol"] = 4;
    out["commands_enabled"] = commandsEnabled;
    out["command_session"] = session.id;
    out["device_uptime_ms"] = session.uptimeMs;
    if (challenge) out["command_session_challenge"] = challenge;
    out["motion_connected"] = connected;
    out["motion_status_stale"] = !connected;
    out["progress"] = progress;
    out["can_start"] = canStart;
    out["is_preparing"] = last && last->isPreparing;
    out["error_code"] = knownError ? last->productError : "NONE";
    out["error_message"] = knownError ? "Motion requires inspection or initialization" : "";
    out["powder_recipe_version"] = 2;
    out["thermal_simulated"] = !last || last->snapshot.thermalSimulated;
    out["measured_water_temp"] = nullptr;
    out["water_temp"] = nullptr;
    out["is_water_ready"] = false;
    out["is_heating"] = false;
    out["is_cooling"] = false;
    if (connected) {
        out["target_temp"] = last->snapshot.temperatureC;
    } else {
        out["target_temp"] = nullptr;
    }
    const bool waterValid = connected && last->lowWaterValid;
    out["water_status"] = waterValid ? (last->lowWater ? "low" : "normal") : "unknown";
    if (waterValid) out["low_water"] = last->lowWater;
    else out["low_water"] = nullptr;
    out["water_remained"] = nullptr;
    const bool powderValid = connected && last->powderValid;
    out["powder_status"] = powderValid ? (last->powderGrams > 50 ? "normal" : "low") : "unknown";
    if (powderValid) out["powder_remained"] = last->powderGrams;
    else out["powder_remained"] = nullptr;
    out["bottle_presence_sensor_enabled"] = false;
    out["bottle_state_valid"] = false;
    out["bottle_present_at_load_position"] = nullptr;
    out["bottle_clamp_status"] = "unknown";
    out["cap_hall_detected"] = nullptr;
    out["dispensed_water_ml"] = nullptr;
    out["water_delivery_basis"] = "estimated_turns";
    out["actuator_operational"] = connected && last->actuatorOperational;
    out["actuator_config_valid"] = connected && last->actuatorConfigValid;
    out["actuator_bus_healthy"] = connected && last->actuatorBusHealthy;
    out["actuator_position_referenced"] = connected && last->actuatorPositionReferenced;
    out["actuator_dosing_stub"] = true;
    out["actuator_schema_version"] = 0;
    out["actuator_profile_revision"] = 0;
    out["actuator_issue"] = connected ? (commandsEnabled ? "" : "read_only_integration") : "motion_link_lost";
    out["feeding_context_configured"] = connected && last->feedingContextConfigured;
    out["feeding_context_baby_id"] = last ? last->babyId : "";
    out["feeding_context_baby_name"] = last ? last->snapshot.babyName.data() : "";
    out["feeding_context_profile_version"] = last ? last->contextVersion : 0;
    out["mqtt_auth_enabled"] = true;
    out["mqtt_tls_enabled"] = false;
}

void writeSimulationStatus(JsonObject out, const char* deviceId, const char* firmwareVersion,
                           const cloud::SessionSnapshot& session, const SimulationStatus& simulation,
                           const char* challenge) {
    writeStatus(out, deviceId, firmwareVersion, nullptr, false, session, challenge);
    out["hardware_profile"] = "simulation";
    out["motion_connected"] = false;
    out["commands_enabled"] = simulation.commandsEnabled;
    out["can_start"] = simulation.canStart && simulation.commandsEnabled && !simulation.running;
    out["is_preparing"] = simulation.running;
    out["progress"] = simulation.running ? "mixing" : simulation.complete ? "complete" :
        (simulation.canStart && simulation.commandsEnabled ? "ready" : "noready");
    out["bottle_clamp_status"] = "unknown";
    out["actuator_issue"] = "brain_simulation";
    out["thermal_simulated"] = true;
    const auto* context = simulation.context;
    out["feeding_context_configured"] = context && !context->cleared;
    out["feeding_context_baby_id"] = context && !context->cleared ? context->babyId : "";
    out["feeding_context_baby_name"] = context && !context->cleared ? context->babyName : "";
    out["feeding_context_profile_version"] = context ? context->profileVersion : 0;
    if (simulation.request) out["target_temp"] = simulation.request->temperatureC;
    else if (context && !context->cleared) out["target_temp"] = context->temperatureC;
    // All physical measurements and actuator validity remain absent/false.
}

} }
