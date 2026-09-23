#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace babytech::display {

constexpr uint16_t kDisplaySchemaVersion = 3;
constexpr size_t kDisplayBabyNameCapacity = 32;
constexpr size_t kDisplayFormulaBrandCapacity = 32;

enum class DisplayStage : uint8_t {
  Idle = 0,
  Ready = 1,
  NotReady = 2,
  UnscrewingCap = 3,
  DispensingWater = 4,
  DispensingPowder = 5,
  ScrewingCap = 6,
  Mixing = 7,
  Complete = 8,
  Cleaning = 9,
  Error = 10,
  Offline = 11,
  Unknown = 12,
};

enum class DisplayCondition : uint8_t {
  None = 0,
  LowWater = 1,
  WaterUnknown = 2,
  BottleUnknown = 3,
  LowFormula = 4,
  EmptyBottleMissing = 5,
  PreparedBottlePresent = 6,
  BabyMissing = 7,
  WifiSetup = 8,
  ControllerOffline = 9,
  ProtocolIncompatible = 10,
  Heating = 11,
  Cooling = 12,
  Ready = 13,
};

enum class DisplayError : uint8_t {
  None = 0,
  CapUnscrewTimeout = 1,
  WaterDispenseTimeout = 2,
  PowderDispenseTimeout = 3,
  CapScrewTimeout = 4,
  MixingTimeout = 5,
  LowWater = 6,
  OverTemperature = 7,
  BottleRemoved = 8,
  WaterSensorInvalid = 9,
  PowderSensorInvalid = 10,
  TemperatureSensorInvalid = 11,
  CanFault = 12,
  NetworkLost = 13,
  Unknown = 14,
  PowderMotorFault = 15,
};

enum class DisplayIntent : uint8_t {
  None = 0,
  StartFeeding = 1,
};

enum class DisplayBottleState : uint8_t {
  None = 0,
  Empty = 1,
  Full = 2,
};

struct DisplaySnapshot {
  uint16_t schemaVersion = kDisplaySchemaVersion;
  DisplayStage stage = DisplayStage::Idle;
  DisplayCondition primaryCondition = DisplayCondition::None;
  DisplayCondition footerCondition = DisplayCondition::None;
  DisplayError error = DisplayError::None;
  bool cloudConnected = false;
  bool startEnabled = false;
  bool thermalSimulated = false;
  uint16_t waterMl = 0;
  int16_t temperatureC = 0;
  std::array<char, kDisplayBabyNameCapacity> babyName{};
  std::array<char, kDisplayFormulaBrandCapacity> formulaBrand{};
};

struct DisplaySnapshotInputs {
  DisplayStage machineStage = DisplayStage::Idle;
  DisplayError error = DisplayError::None;
  bool errorActive = false;
  bool provisioning = false;
  bool cloudConnected = false;
  bool thermalSimulated = false;
  bool feedingContextConfigured = false;
  bool formulaActive = false;
  bool bottleStateValid = false;
  DisplayBottleState bottleState = DisplayBottleState::None;
  bool lowWaterValid = false;
  bool lowWater = false;
  int powderGrams = 0;
  int requiredPowderGrams = 0;
  bool isHeating = false;
  bool isCooling = false;
  bool startEnabled = false;
  int waterMl = 0;
  int temperatureC = 0;
  const char* babyName = "";
  const char* formulaBrand = "";
};

DisplaySnapshot buildDisplaySnapshot(const DisplaySnapshotInputs& inputs);
bool displaySnapshotsEqual(const DisplaySnapshot& left,
                           const DisplaySnapshot& right);

const char* displayStageKey(DisplayStage value);
DisplayStage displayStageFromKey(const char* key);

const char* displayConditionKey(DisplayCondition value);
DisplayCondition displayConditionFromKey(const char* key);

const char* displayErrorKey(DisplayError value);
DisplayError displayErrorFromKey(const char* key);

const char* displayIntentKey(DisplayIntent value);
DisplayIntent displayIntentFromKey(const char* key);

}  // namespace babytech::display
