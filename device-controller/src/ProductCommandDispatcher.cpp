#include "ProductCommandDispatcher.h"

#include "CloudCommandGate.h"

#include <cmath>
#include <cstring>
#include <utility>

namespace motion {
namespace {
bool numberInRange(JsonVariantConst value, double minimum, double maximum) {
    if (!(value.is<int>() || value.is<double>())) return false;
    const double number = value.as<double>();
    return std::isfinite(number) && number >= minimum && number <= maximum;
}
}

ProductCommandOutcome executeProductCommand(JsonVariantConst document, ProductSession& product,
                                           bool otaActive, uint32_t now) {
    ProductCommandOutcome result;
    const char* command = document["command"] | "";
    if (otaActive && !cloudCommandAllowedDuringOta(command)) {
        result.reason = "ota_active";
    } else if (std::strcmp(command, "prepare") == 0) {
        // Defaults apply only to absent optional fields, never malformed values.
        if (document.containsKey("ratio")) {
            result.reason = "invalid_powder_g_per_100ml";
            return result;
        }
        if (document.containsKey("water_ml") && !numberInRange(document["water_ml"], 30, 500)) {
            result.reason = "invalid_water_ml";
            return result;
        }
        if (document.containsKey("temp") && !numberInRange(document["temp"], 35, 60)) {
            result.reason = "invalid_temp";
            return result;
        }
        if (!numberInRange(document["powder_g_per_100ml"], 1, 50)) {
            result.reason = "invalid_powder_g_per_100ml";
            return result;
        }
        ProductRun run;
        run.commandId = document["command_id"] | "";
        run.babyId = document["baby_id"] | "";
        run.profileVersion = document["feeding_context_profile_version"] | 0;
        run.recipe.waterMl = document.containsKey("water_ml") ? document["water_ml"].as<int>() : 180;
        run.recipe.temperatureC = document.containsKey("temp") ? document["temp"].as<int>() : 45;
        run.recipe.powderGPer100Ml = document["powder_g_per_100ml"].as<float>();
        const int targetTemperature = run.recipe.temperatureC;
        result.accepted = product.startCloud(std::move(run), now, result.reason);
        if (result.accepted) {
            product.setTargetTemp(targetTemperature);
            result.status = "accepted";
        }
    } else if (std::strcmp(command, "stop") == 0) {
        bool wasActive = false;
        result.accepted = product.stop(now, wasActive);
        result.status = result.accepted ? (wasActive ? "accepted" : "already_idle") : "failed";
        result.reason = result.accepted ? nullptr : "stop_unconfirmed";
    } else if (std::strcmp(command, "clean") == 0) {
        result.accepted = product.clean(now, result.reason);
        if (result.accepted) result.status = "accepted";
    } else if (std::strcmp(command, "set_target_temp") == 0) {
        if (!numberInRange(document["temp"], 35, 60)) result.reason = "invalid_temp";
        else if (product.ownsMotion()) result.reason = "busy";
        else {
            product.setTargetTemp(document["temp"].as<int>());
            result.accepted = true;
            result.status = "accepted";
            result.reason = nullptr;
        }
    } else if (std::strcmp(command, "reset_error") == 0) {
        if (std::strcmp(product.progress(), "error") != 0) {
            result.accepted = true;
            result.status = "already_clear";
            result.reason = nullptr;
        } else result.reason = "manual_initialization_required";
    } else if (std::strcmp(command, "check_firmware_update") == 0) {
        result.reason = "cloud_ota_not_supported";
    } else result.reason = "unknown_command";
    return result;
}

}  // namespace motion
