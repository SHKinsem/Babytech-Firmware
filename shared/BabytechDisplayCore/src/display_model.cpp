#include "display_model.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace babytech::display {
namespace {

template <size_t Size>
void copyText(std::array<char, Size>& destination, const char* source) {
  destination.fill('\0');
  if (source == nullptr || Size == 0) return;
  std::strncpy(destination.data(), source, Size - 1);
}

uint16_t boundedWaterMl(int value) {
  if (value <= 0) return 0;
  return static_cast<uint16_t>(std::min(
      value, static_cast<int>(std::numeric_limits<uint16_t>::max())));
}

int16_t boundedTemperatureC(int value) {
  return static_cast<int16_t>(std::clamp(
      value, static_cast<int>(std::numeric_limits<int16_t>::min()),
      static_cast<int>(std::numeric_limits<int16_t>::max())));
}

template <typename Enum>
struct KeyValue {
  Enum value;
  const char* key;
};

constexpr KeyValue<DisplayStage> kStages[] = {
    {DisplayStage::Idle, "idle"},
    {DisplayStage::Ready, "ready"},
    {DisplayStage::NotReady, "noready"},
    {DisplayStage::UnscrewingCap, "unscrewing_cap"},
    {DisplayStage::DispensingWater, "dispensing_water"},
    {DisplayStage::DispensingPowder, "dispensing_powder"},
    {DisplayStage::ScrewingCap, "screwing_cap"},
    {DisplayStage::Mixing, "mixing"},
    {DisplayStage::Complete, "complete"},
    {DisplayStage::Cleaning, "cleaning"},
    {DisplayStage::Error, "error"},
    {DisplayStage::Offline, "offline"},
    {DisplayStage::Unknown, "unknown"},
};

constexpr KeyValue<DisplayCondition> kConditions[] = {
    {DisplayCondition::None, "none"},
    {DisplayCondition::LowWater, "low_water"},
    {DisplayCondition::WaterUnknown, "water_unknown"},
    {DisplayCondition::BottleUnknown, "bottle_unknown"},
    {DisplayCondition::LowFormula, "low_formula"},
    {DisplayCondition::EmptyBottleMissing, "empty_bottle_missing"},
    {DisplayCondition::PreparedBottlePresent, "prepared_bottle_present"},
    {DisplayCondition::BabyMissing, "baby_missing"},
    {DisplayCondition::WifiSetup, "wifi_setup"},
    {DisplayCondition::ControllerOffline, "controller_offline"},
    {DisplayCondition::ProtocolIncompatible, "protocol_incompatible"},
    {DisplayCondition::Heating, "heating"},
    {DisplayCondition::Cooling, "cooling"},
    {DisplayCondition::Ready, "ready"},
};

constexpr KeyValue<DisplayError> kErrors[] = {
    {DisplayError::None, "NONE"},
    {DisplayError::CapUnscrewTimeout, "E_CAP_UNSCREW_TIMEOUT"},
    {DisplayError::WaterDispenseTimeout, "E_WATER_DISPENSE_TIMEOUT"},
    {DisplayError::PowderDispenseTimeout, "E_POWDER_DISPENSE_TIMEOUT"},
    {DisplayError::CapScrewTimeout, "E_CAP_SCREW_TIMEOUT"},
    {DisplayError::MixingTimeout, "E_MIXING_TIMEOUT"},
    {DisplayError::LowWater, "E_LOW_WATER"},
    {DisplayError::OverTemperature, "E_OVER_TEMPERATURE"},
    {DisplayError::BottleRemoved, "E_BOTTLE_REMOVED"},
    {DisplayError::WaterSensorInvalid, "E_WATER_SENSOR_INVALID"},
    {DisplayError::PowderSensorInvalid, "E_POWDER_SENSOR_INVALID"},
    {DisplayError::TemperatureSensorInvalid,
     "E_TEMPERATURE_SENSOR_INVALID"},
    {DisplayError::CanFault, "E_CAN_FAULT"},
    {DisplayError::NetworkLost, "E_NETWORK_LOST"},
    {DisplayError::Unknown, "E_UNKNOWN"},
    {DisplayError::PowderMotorFault, "E_POWDER_MOTOR_FAULT"},
};

constexpr KeyValue<DisplayIntent> kIntents[] = {
    {DisplayIntent::None, "none"},
    {DisplayIntent::StartFeeding, "start_feeding"},
};

template <typename Enum, size_t Size>
const char* keyFor(Enum value, const KeyValue<Enum> (&entries)[Size],
                   const char* fallback) {
  for (const auto& entry : entries) {
    if (entry.value == value) return entry.key;
  }
  return fallback;
}

template <typename Enum, size_t Size>
Enum valueFor(const char* key, const KeyValue<Enum> (&entries)[Size],
              Enum fallback) {
  if (key == nullptr) return fallback;
  for (const auto& entry : entries) {
    if (std::strcmp(entry.key, key) == 0) return entry.value;
  }
  return fallback;
}

DisplayCondition footerConditionFor(const DisplaySnapshotInputs& inputs) {
  if (!inputs.feedingContextConfigured && !inputs.formulaActive) {
    return DisplayCondition::BabyMissing;
  }
  if (!inputs.lowWaterValid) return DisplayCondition::WaterUnknown;
  if (inputs.lowWater) return DisplayCondition::LowWater;
  if (inputs.powderGrams <= 50 ||
      inputs.powderGrams < inputs.requiredPowderGrams) {
    return DisplayCondition::LowFormula;
  }
  if (!inputs.bottleStateValid && !inputs.formulaActive) {
    return DisplayCondition::BottleUnknown;
  }
  if (inputs.bottleState == DisplayBottleState::None) {
    return DisplayCondition::EmptyBottleMissing;
  }
  if (inputs.bottleState == DisplayBottleState::Full) {
    return DisplayCondition::PreparedBottlePresent;
  }
  if (inputs.isHeating) return DisplayCondition::Heating;
  if (inputs.isCooling) return DisplayCondition::Cooling;
  return DisplayCondition::Ready;
}

}  // namespace

DisplaySnapshot buildDisplaySnapshot(const DisplaySnapshotInputs& inputs) {
  DisplaySnapshot snapshot;
  snapshot.stage = inputs.machineStage;
  snapshot.error = inputs.errorActive ? inputs.error : DisplayError::None;
  snapshot.cloudConnected = inputs.cloudConnected;
  snapshot.thermalSimulated = inputs.thermalSimulated;
  snapshot.startEnabled = inputs.startEnabled && !inputs.errorActive &&
                          !inputs.provisioning && inputs.cloudConnected &&
                          inputs.feedingContextConfigured;
  snapshot.waterMl = boundedWaterMl(inputs.waterMl);
  snapshot.temperatureC = boundedTemperatureC(inputs.temperatureC);
  copyText(snapshot.babyName, inputs.babyName);
  copyText(snapshot.formulaBrand, inputs.formulaBrand);

  if (inputs.errorActive) {
    snapshot.stage = DisplayStage::Error;
  } else if (inputs.provisioning) {
    snapshot.primaryCondition = DisplayCondition::WifiSetup;
  } else if (!inputs.cloudConnected) {
    snapshot.stage = DisplayStage::Offline;
  } else if (!inputs.feedingContextConfigured && !inputs.formulaActive) {
    snapshot.primaryCondition = DisplayCondition::BabyMissing;
  }

  snapshot.footerCondition = footerConditionFor(inputs);
  return snapshot;
}

bool displaySnapshotsEqual(const DisplaySnapshot& left,
                           const DisplaySnapshot& right) {
  return left.schemaVersion == right.schemaVersion &&
         left.stage == right.stage &&
         left.primaryCondition == right.primaryCondition &&
         left.footerCondition == right.footerCondition &&
         left.error == right.error &&
         left.cloudConnected == right.cloudConnected &&
         left.startEnabled == right.startEnabled &&
         left.thermalSimulated == right.thermalSimulated &&
         left.waterMl == right.waterMl &&
         left.temperatureC == right.temperatureC &&
         left.babyName == right.babyName &&
         left.formulaBrand == right.formulaBrand;
}

const char* displayStageKey(DisplayStage value) {
  return keyFor(value, kStages, "unknown");
}

DisplayStage displayStageFromKey(const char* key) {
  return valueFor(key, kStages, DisplayStage::Unknown);
}

const char* displayConditionKey(DisplayCondition value) {
  return keyFor(value, kConditions, "none");
}

DisplayCondition displayConditionFromKey(const char* key) {
  return valueFor(key, kConditions, DisplayCondition::None);
}

const char* displayErrorKey(DisplayError value) {
  return keyFor(value, kErrors, "E_UNKNOWN");
}

DisplayError displayErrorFromKey(const char* key) {
  return valueFor(key, kErrors, DisplayError::Unknown);
}

const char* displayIntentKey(DisplayIntent value) {
  return keyFor(value, kIntents, "none");
}

DisplayIntent displayIntentFromKey(const char* key) {
  return valueFor(key, kIntents, DisplayIntent::None);
}

}  // namespace babytech::display
