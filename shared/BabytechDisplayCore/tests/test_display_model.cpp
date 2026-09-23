#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

#include "display_model.h"

using babytech::display::DisplayBottleState;
using babytech::display::DisplayCondition;
using babytech::display::DisplayError;
using babytech::display::DisplayIntent;
using babytech::display::DisplaySnapshotInputs;
using babytech::display::DisplayStage;
using babytech::display::buildDisplaySnapshot;
using babytech::display::displayConditionFromKey;
using babytech::display::displayConditionKey;
using babytech::display::displayErrorFromKey;
using babytech::display::displayErrorKey;
using babytech::display::displayIntentFromKey;
using babytech::display::displayIntentKey;
using babytech::display::displayStageFromKey;
using babytech::display::displayStageKey;
using babytech::display::displaySnapshotsEqual;

namespace {

static_assert(static_cast<uint8_t>(DisplayStage::Idle) == 0);
static_assert(static_cast<uint8_t>(DisplayStage::Unknown) == 12);
static_assert(static_cast<uint8_t>(DisplayCondition::None) == 0);
static_assert(static_cast<uint8_t>(DisplayCondition::Ready) == 13);
static_assert(static_cast<uint8_t>(DisplayError::None) == 0);
static_assert(static_cast<uint8_t>(DisplayError::Unknown) == 14);
static_assert(static_cast<uint8_t>(DisplayError::PowderMotorFault) == 15);
static_assert(static_cast<uint8_t>(DisplayIntent::None) == 0);
static_assert(static_cast<uint8_t>(DisplayIntent::StartFeeding) == 1);

DisplaySnapshotInputs readyInputs() {
  DisplaySnapshotInputs inputs;
  inputs.machineStage = DisplayStage::Ready;
  inputs.cloudConnected = true;
  inputs.feedingContextConfigured = true;
  inputs.bottleStateValid = true;
  inputs.bottleState = DisplayBottleState::Empty;
  inputs.lowWaterValid = true;
  inputs.powderGrams = 350;
  inputs.requiredPowderGrams = 9;
  inputs.startEnabled = true;
  inputs.waterMl = 180;
  inputs.temperatureC = 45;
  inputs.thermalSimulated = true;
  inputs.babyName = "Mia";
  inputs.formulaBrand = "Friso";
  return inputs;
}

void testReadySnapshot() {
  const auto snapshot = buildDisplaySnapshot(readyInputs());
  assert(snapshot.stage == DisplayStage::Ready);
  assert(snapshot.primaryCondition == DisplayCondition::None);
  assert(snapshot.footerCondition == DisplayCondition::Ready);
  assert(snapshot.error == DisplayError::None);
  assert(snapshot.cloudConnected);
  assert(snapshot.startEnabled);
  assert(snapshot.waterMl == 180);
  assert(snapshot.temperatureC == 45);
  assert(snapshot.thermalSimulated);
  assert(std::strcmp(snapshot.babyName.data(), "Mia") == 0);
  assert(std::strcmp(snapshot.formulaBrand.data(), "Friso") == 0);
}

void testPrimaryStatePrecedence() {
  auto inputs = readyInputs();
  inputs.errorActive = true;
  inputs.error = DisplayError::CanFault;
  inputs.provisioning = true;
  inputs.cloudConnected = false;
  const auto error = buildDisplaySnapshot(inputs);
  assert(error.stage == DisplayStage::Error);
  assert(error.error == DisplayError::CanFault);
  assert(error.primaryCondition == DisplayCondition::None);

  inputs.errorActive = false;
  const auto provisioning = buildDisplaySnapshot(inputs);
  assert(provisioning.stage == DisplayStage::Ready);
  assert(provisioning.primaryCondition == DisplayCondition::WifiSetup);

  inputs.provisioning = false;
  const auto offline = buildDisplaySnapshot(inputs);
  assert(offline.stage == DisplayStage::Offline);

  inputs.cloudConnected = true;
  inputs.feedingContextConfigured = false;
  const auto missingBaby = buildDisplaySnapshot(inputs);
  assert(missingBaby.stage == DisplayStage::Ready);
  assert(missingBaby.primaryCondition == DisplayCondition::BabyMissing);
}

void testUnsafePrimaryStatesDisableStart() {
  auto inputs = readyInputs();
  inputs.errorActive = true;
  assert(!buildDisplaySnapshot(inputs).startEnabled);

  inputs.errorActive = false;
  inputs.provisioning = true;
  assert(!buildDisplaySnapshot(inputs).startEnabled);

  inputs.provisioning = false;
  inputs.cloudConnected = false;
  assert(!buildDisplaySnapshot(inputs).startEnabled);

  inputs.cloudConnected = true;
  inputs.feedingContextConfigured = false;
  assert(!buildDisplaySnapshot(inputs).startEnabled);
}

void testFooterPrecedence() {
  auto inputs = readyInputs();

  inputs.feedingContextConfigured = false;
  assert(buildDisplaySnapshot(inputs).footerCondition ==
         DisplayCondition::BabyMissing);

  inputs.feedingContextConfigured = true;
  inputs.bottleStateValid = false;
  assert(buildDisplaySnapshot(inputs).footerCondition ==
         DisplayCondition::BottleUnknown);

  inputs.bottleStateValid = true;
  inputs.bottleState = DisplayBottleState::None;
  assert(buildDisplaySnapshot(inputs).footerCondition ==
         DisplayCondition::EmptyBottleMissing);

  inputs.bottleState = DisplayBottleState::Full;
  assert(buildDisplaySnapshot(inputs).footerCondition ==
         DisplayCondition::PreparedBottlePresent);

  inputs.bottleState = DisplayBottleState::Empty;
  inputs.lowWaterValid = false;
  assert(buildDisplaySnapshot(inputs).footerCondition ==
         DisplayCondition::WaterUnknown);

  inputs.lowWaterValid = true;
  inputs.lowWater = true;
  assert(buildDisplaySnapshot(inputs).footerCondition ==
         DisplayCondition::LowWater);

  inputs.lowWater = false;
  inputs.powderGrams = 8;
  assert(buildDisplaySnapshot(inputs).footerCondition ==
         DisplayCondition::LowFormula);
}

void testCompletedBottleHasRemovalCondition() {
  auto inputs = readyInputs();
  inputs.machineStage = DisplayStage::Complete;
  inputs.bottleState = DisplayBottleState::Full;
  inputs.startEnabled = false;
  const auto snapshot = buildDisplaySnapshot(inputs);
  assert(snapshot.stage == DisplayStage::Complete);
  assert(snapshot.primaryCondition == DisplayCondition::None);
  assert(snapshot.footerCondition == DisplayCondition::PreparedBottlePresent);
  assert(!snapshot.startEnabled);
}

void testConditionPriorityMatchesHome() {
  auto inputs = readyInputs();
  inputs.lowWater = true;
  inputs.powderGrams = 0;
  inputs.bottleState = DisplayBottleState::None;
  inputs.isHeating = true;
  assert(buildDisplaySnapshot(inputs).footerCondition ==
         DisplayCondition::LowWater);

  inputs.lowWater = false;
  assert(buildDisplaySnapshot(inputs).footerCondition ==
         DisplayCondition::LowFormula);

  inputs.powderGrams = 350;
  assert(buildDisplaySnapshot(inputs).footerCondition ==
         DisplayCondition::EmptyBottleMissing);

  inputs.bottleState = DisplayBottleState::Empty;
  assert(buildDisplaySnapshot(inputs).footerCondition ==
         DisplayCondition::Heating);
  assert(buildDisplaySnapshot(inputs).startEnabled);

  inputs.isHeating = false;
  inputs.isCooling = true;
  assert(buildDisplaySnapshot(inputs).footerCondition ==
         DisplayCondition::Cooling);

  inputs.isCooling = false;
  inputs.powderGrams = 50;
  assert(buildDisplaySnapshot(inputs).footerCondition ==
         DisplayCondition::LowFormula);
}

void testActiveFormulaDoesNotRequireContextOrStableBottle() {
  auto inputs = readyInputs();
  inputs.machineStage = DisplayStage::DispensingWater;
  inputs.feedingContextConfigured = false;
  inputs.formulaActive = true;
  inputs.bottleStateValid = false;
  inputs.bottleState = DisplayBottleState::Empty;
  const auto snapshot = buildDisplaySnapshot(inputs);
  assert(snapshot.stage == DisplayStage::DispensingWater);
  assert(snapshot.primaryCondition == DisplayCondition::None);
  assert(snapshot.footerCondition == DisplayCondition::Ready);
}

void testOwnedTextIsBoundedAndTerminated() {
  auto inputs = readyInputs();
  const std::string longName(80, 'N');
  const std::string longBrand(80, 'B');
  inputs.babyName = longName.c_str();
  inputs.formulaBrand = longBrand.c_str();
  const auto snapshot = buildDisplaySnapshot(inputs);
  assert(snapshot.babyName.back() == '\0');
  assert(snapshot.formulaBrand.back() == '\0');
  assert(std::strlen(snapshot.babyName.data()) ==
         snapshot.babyName.size() - 1);
  assert(std::strlen(snapshot.formulaBrand.data()) ==
         snapshot.formulaBrand.size() - 1);
}

void testNumericInputsAreBounded() {
  auto inputs = readyInputs();
  inputs.waterMl = -1;
  inputs.temperatureC = 100000;
  auto snapshot = buildDisplaySnapshot(inputs);
  assert(snapshot.waterMl == 0);
  assert(snapshot.temperatureC == 32767);

  inputs.waterMl = 100000;
  inputs.temperatureC = -100000;
  snapshot = buildDisplaySnapshot(inputs);
  assert(snapshot.waterMl == 65535);
  assert(snapshot.temperatureC == -32768);
}

void testStableKeyMappings() {
  assert(displayStageFromKey(displayStageKey(DisplayStage::DispensingPowder)) ==
         DisplayStage::DispensingPowder);
  assert(displayConditionFromKey(
             displayConditionKey(DisplayCondition::PreparedBottlePresent)) ==
         DisplayCondition::PreparedBottlePresent);
  assert(displayErrorFromKey(displayErrorKey(DisplayError::CanFault)) ==
         DisplayError::CanFault);
  assert(displayErrorFromKey("E_POWDER_MOTOR_FAULT") ==
         DisplayError::PowderMotorFault);
  assert(displayIntentFromKey(displayIntentKey(DisplayIntent::StartFeeding)) ==
         DisplayIntent::StartFeeding);

  assert(displayStageFromKey("new_stage") == DisplayStage::Unknown);
  assert(displayConditionFromKey("new_condition") == DisplayCondition::None);
  assert(displayErrorFromKey("new_error") == DisplayError::Unknown);
  assert(displayIntentFromKey("new_intent") == DisplayIntent::None);
}

void testSnapshotEqualityIncludesDisplayFields() {
  const auto first = buildDisplaySnapshot(readyInputs());
  auto second = first;
  assert(displaySnapshotsEqual(first, second));
  second.startEnabled = false;
  assert(!displaySnapshotsEqual(first, second));
}

}  // namespace

int main() {
  testReadySnapshot();
  testPrimaryStatePrecedence();
  testUnsafePrimaryStatesDisableStart();
  testFooterPrecedence();
  testCompletedBottleHasRemovalCondition();
  testConditionPriorityMatchesHome();
  testActiveFormulaDoesNotRequireContextOrStableBottle();
  testOwnedTextIsBoundedAndTerminated();
  testNumericInputsAreBounded();
  testStableKeyMappings();
  testSnapshotEqualityIncludesDisplayFields();
  std::cout << "display model tests passed\n";
  return 0;
}
