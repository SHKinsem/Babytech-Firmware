#include "ProductEventCodec.h"

namespace motion {

void writeProductTerminalEvent(JsonObject target, const ProductTerminal& terminal,
                               const char* deviceId, const char* eventId, uint32_t uptimeMs) {
    target["event_id"] = eventId;
    target["event"] = terminal.completed ? "feeding_completed" : "feeding_failed";
    target["device_id"] = deviceId;
    target["command_id"] = terminal.run.commandId.c_str();
    target["source"] = terminal.run.source.c_str();
    target["baby_id"] = terminal.run.babyId.c_str();
    target["feeding_context_profile_version"] = terminal.run.profileVersion;
    target["water_ml"] = terminal.run.recipe.waterMl;
    target["temp"] = terminal.run.recipe.temperatureC;
    target["powder_g_per_100ml"] = terminal.run.recipe.powderGPer100Ml;
    target["target_powder_g"] = terminal.run.targetPowderG;
    target["water_delivery_basis"] = "estimated_turns";
    target["dispensed_water_ml"] = nullptr;
    target["uptime_ms"] = uptimeMs;
    if (!terminal.reason.empty()) target["reason"] = terminal.reason.c_str();
    if (!terminal.errorCode.empty()) target["error_code"] = terminal.errorCode.c_str();
}

}  // namespace motion
