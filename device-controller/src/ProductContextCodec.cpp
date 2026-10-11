#include "ProductContextCodec.h"

#include <cmath>
#include <cstring>

namespace motion {

bool decodeFeedingContext(JsonVariantConst source, FeedingContext& context) {
    if (!source.is<JsonObjectConst>() ||
        !source["baby_id"].is<const char*>() ||
        !source["water_ml"].is<int>() ||
        !source["temp"].is<int>() ||
        !(source["powder_g_per_100ml"].is<int>() ||
          source["powder_g_per_100ml"].is<float>()) ||
        !source["profile_version"].is<int>()) return false;
    const char* babyId = source["baby_id"] | "";
    const char* babyName = source["baby_name"] | "";
    const char* brand = source["formula_brand"] | "Friso";
    const int waterMl = source["water_ml"].as<int>();
    const int temperatureC = source["temp"].as<int>();
    const float powderRate = source["powder_g_per_100ml"].as<float>();
    const int profileVersion = source["profile_version"].as<int>();
    // Cloud allows 80/120 Unicode code points; each can occupy four UTF-8 bytes.
    if (!babyId[0] || std::strlen(babyId) > 96 || std::strlen(babyName) > 320 ||
        std::strlen(brand) > 480 || waterMl < 30 || waterMl > 500 ||
        temperatureC < 35 || temperatureC > 60 || !std::isfinite(powderRate) ||
        powderRate < 1.0f || powderRate > 50.0f || profileVersion <= 0) return false;
    context.babyId = babyId;
    context.babyName = babyName;
    context.formulaBrand = brand;
    context.recipe.waterMl = waterMl;
    context.recipe.temperatureC = temperatureC;
    context.recipe.powderGPer100Ml = powderRate;
    context.profileVersion = static_cast<uint32_t>(profileVersion);
    return true;
}

bool decodeFeedingContextClear(JsonVariantConst source, uint32_t& profileVersion) {
    if (!source.is<JsonObjectConst>() || !source["cleared"].is<bool>() ||
        !source["cleared"].as<bool>() || !source["profile_version"].is<int>()) return false;
    const int version = source["profile_version"].as<int>();
    if (version <= 0) return false;
    profileVersion = static_cast<uint32_t>(version);
    return true;
}

}  // namespace motion
