// Source SHA-256: efd101bc82a9f906b78e9386631fbfb721884b1addbcb3103ddaa871e9c386ff
// Generated from Shared/feeding_flow_ui/feeding_flow_ui.json.
// DO NOT EDIT: update the shared contract and rebuild instead.
#pragma once

#include <cstring>

enum class FeedingFlowUiTone { Neutral, Ready, Active, Success, Error };

struct FeedingFlowUiStage {
  const char* key;
  const char* title;
  const char* detail;
  const char* fallbackDetail;
  const char* action;
  float progress;
  FeedingFlowUiTone tone;
};

static constexpr FeedingFlowUiStage kFeedingFlowUiStages[] = {
  {"idle", "Standby", "Checking readiness", "Checking readiness", "Start feeding", 0.00f, FeedingFlowUiTone::Neutral},
  {"ready", "Ready", "Ready for {baby_name}", "Ready to prepare", "Start feeding", 0.00f, FeedingFlowUiTone::Ready},
  {"noready", "Not ready", "Not ready", "Not ready", "Not ready", 0.00f, FeedingFlowUiTone::Neutral},
  {"unscrewing_cap", "Preparing", "Opening cap", "Opening cap", "Preparing", 0.25f, FeedingFlowUiTone::Active},
  {"dispensing_water", "Preparing", "Adding water", "Adding water", "Preparing", 0.40f, FeedingFlowUiTone::Active},
  {"dispensing_powder", "Preparing", "Adding formula", "Adding formula", "Preparing", 0.60f, FeedingFlowUiTone::Active},
  {"screwing_cap", "Preparing", "Closing cap", "Closing cap", "Preparing", 0.70f, FeedingFlowUiTone::Active},
  {"mixing", "Preparing", "Mixing formula", "Mixing formula", "Preparing", 0.85f, FeedingFlowUiTone::Active},
  {"complete", "Bottle ready", "Check temperature before feeding", "Check temperature before feeding", "Bottle ready", 1.00f, FeedingFlowUiTone::Success},
  {"unknown", "Status unknown", "Check device status", "Check device status", "Not ready", 0.00f, FeedingFlowUiTone::Neutral},
  {"cleaning", "Clean mode", "Power off before cleaning", "Power off before cleaning", "Clean mode", 0.00f, FeedingFlowUiTone::Active},
  {"error", "Needs attention", "Check device", "Check device", "Needs attention", 0.00f, FeedingFlowUiTone::Error},
  {"offline", "Offline", "Check connection", "Check connection", "Offline", 0.00f, FeedingFlowUiTone::Error},
};

inline const FeedingFlowUiStage* feedingFlowUiStage(const char* key) {
  for (const auto& stage : kFeedingFlowUiStages) {
    if (std::strcmp(stage.key, key) == 0) return &stage;
  }
  return &kFeedingFlowUiStages[0];
}

struct FeedingFlowUiText { const char* key; const char* text; };

static constexpr FeedingFlowUiText kFeedingFlowUiConditions[] = {
  {"low_water", "Refill water"},
  {"water_unknown", "Water status unknown"},
  {"bottle_unknown", "Bottle status unknown"},
  {"low_formula", "Add formula"},
  {"empty_bottle_missing", "Place empty bottle"},
  {"prepared_bottle_present", "Remove bottle"},
  {"baby_missing", "Select baby in app"},
  {"wifi_setup", "Connect Wi-Fi"},
  {"controller_offline", "Controller offline"},
  {"protocol_incompatible", "Update display firmware"},
  {"heating", "Water is heating"},
  {"cooling", "Water is cooling"},
  {"ready", "Ready to prepare"},
};

inline const char* feedingFlowUiCondition(const char* key) {
  for (const auto& item : kFeedingFlowUiConditions) {
    if (std::strcmp(item.key, key) == 0) return item.text;
  }
  return key;
}

static constexpr FeedingFlowUiText kFeedingFlowUiErrors[] = {
  {"E_CAP_UNSCREW_TIMEOUT", "Unable to open the bottle cap"},
  {"E_WATER_DISPENSE_TIMEOUT", "Water dispensing timed out"},
  {"E_POWDER_DISPENSE_TIMEOUT", "Formula dispensing timed out"},
  {"E_POWDER_MOTOR_FAULT", "Formula motor fault"},
  {"E_CAP_SCREW_TIMEOUT", "Unable to close the bottle cap"},
  {"E_MIXING_TIMEOUT", "Mixing timed out"},
  {"E_LOW_WATER", "Refill water"},
  {"E_OVER_TEMPERATURE", "Water temperature is too high"},
  {"E_BOTTLE_REMOVED", "Bottle was removed during preparation"},
  {"E_WATER_SENSOR_INVALID", "Water level reading is invalid"},
  {"E_POWDER_SENSOR_INVALID", "Formula level reading is invalid"},
  {"E_TEMPERATURE_SENSOR_INVALID", "Temperature reading is invalid"},
  {"E_CAN_FAULT", "Motor communication failed"},
  {"E_NETWORK_LOST", "Cloud connection was lost"},
  {"E_UNKNOWN", "Check device"},
};

inline const char* feedingFlowUiError(const char* key) {
  for (const auto& item : kFeedingFlowUiErrors) {
    if (std::strcmp(item.key, key) == 0) return item.text;
  }
  return key;
}

static constexpr FeedingFlowUiText kFeedingFlowUiActions[] = {
  {"initialize", "Initialize"},
  {"start", "Start feeding"},
  {"sending", "Sending"},
  {"starting", "Starting"},
  {"preparing", "Preparing"},
  {"complete", "Bottle ready"},
};

inline const char* feedingFlowUiAction(const char* key) {
  for (const auto& item : kFeedingFlowUiActions) {
    if (std::strcmp(item.key, key) == 0) return item.text;
  }
  return key;
}
