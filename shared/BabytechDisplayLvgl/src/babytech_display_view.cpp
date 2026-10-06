#include "babytech_display_view.h"

#include <cstdio>
#include <cstring>

#include "generated/feeding_flow_ui.h"

namespace babytech::display {
namespace {

constexpr uint32_t kPageBackground = 0x242831;
constexpr uint32_t kCircleBackground = 0x303D4C;
constexpr uint32_t kCardBackground = 0x333C4B;
constexpr uint32_t kPrimaryText = 0xF5EEE8;
constexpr uint32_t kSecondaryText = 0xD3DCDD;
constexpr uint32_t kMutedText = 0xA8B8C0;
constexpr uint32_t kAction = 0x32ADB6;
constexpr uint32_t kDisabled = 0x4E5965;
constexpr uint32_t kRingNeutral = 0x758795;
constexpr uint32_t kRingReady = 0x58C7C4;
constexpr uint32_t kRingActive = 0xE7B46A;
constexpr uint32_t kRingSuccess = 0x6AC99B;
constexpr uint32_t kRingError = 0xE4887E;
constexpr uint32_t kWhite = 0xFFFFFF;

void setTextStyle(lv_obj_t* object, const lv_font_t* font, uint32_t color) {
  lv_obj_set_style_text_font(object, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(object, lv_color_hex(color), LV_PART_MAIN);
}

void setContainerStyle(lv_obj_t* object, uint32_t background,
                       lv_opa_t opacity = LV_OPA_COVER) {
  lv_obj_set_style_bg_color(object, lv_color_hex(background), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(object, opacity, LV_PART_MAIN);
  lv_obj_set_style_border_width(object, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(object, 0, LV_PART_MAIN);
  lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

void setCardStyle(lv_obj_t* object, uint32_t background) {
  setContainerStyle(object, background);
  lv_obj_set_style_radius(object, 18, LV_PART_MAIN);
  lv_obj_set_style_border_width(object, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(object, lv_color_hex(0x5D6D7A), LV_PART_MAIN);
}

void resolveDetail(const FeedingFlowUiStage& stage, const char* babyName,
                   char* output, size_t outputSize) {
  const char* marker = std::strstr(stage.detail, "{baby_name}");
  if (marker == nullptr) {
    std::snprintf(output, outputSize, "%s", stage.detail);
    return;
  }
  if (babyName == nullptr || babyName[0] == '\0') {
    std::snprintf(output, outputSize, "%s", stage.fallbackDetail);
    return;
  }
  const int prefixLength = static_cast<int>(marker - stage.detail);
  std::snprintf(output, outputSize, "%.*s%s%s", prefixLength, stage.detail,
                babyName, marker + std::strlen("{baby_name}"));
}

FeedingFlowUiTone toneFor(DisplayStage stage) {
  return feedingFlowUiStage(displayStageKey(stage))->tone;
}

bool isFeedingStage(DisplayStage stage) {
  return stage == DisplayStage::UnscrewingCap ||
         stage == DisplayStage::DispensingWater ||
         stage == DisplayStage::DispensingPowder ||
         stage == DisplayStage::ScrewingCap || stage == DisplayStage::Mixing;
}

}  // namespace

bool BabytechDisplayView::begin(bool allowInitialization) {
  if (ready_) return true;
  if (lv_scr_act() == nullptr) return false;
  allowInitialization_ = allowInitialization;
  createUi();
  ready_ = true;
  return true;
}

void BabytechDisplayView::poll(uint32_t nowMs) {
  if (!ready_) return;
  if (lastTickMs_ == 0) {
    lastTickMs_ = nowMs;
  } else {
    lv_tick_inc(nowMs - lastTickMs_);
    lastTickMs_ = nowMs;
  }
  lv_timer_handler();
}

void BabytechDisplayView::update(const DisplaySnapshot& snapshot,
                                 bool controllerConnected, bool intentPending) {
  if (!ready_) return;
  if (hasSnapshot_ && lastControllerConnected_ == controllerConnected &&
      lastIntentPending_ == intentPending &&
      displaySnapshotsEqual(lastSnapshot_, snapshot)) {
    return;
  }
  applySnapshot(snapshot, controllerConnected);
  if (initializeButton_) {
    if (displayInitializeEnabled(snapshot, controllerConnected, intentPending))
      lv_obj_clear_state(initializeButton_, LV_STATE_DISABLED);
    else
      lv_obj_add_state(initializeButton_, LV_STATE_DISABLED);
  }
  lastSnapshot_ = snapshot;
  lastControllerConnected_ = controllerConnected;
  lastIntentPending_ = intentPending;
  hasSnapshot_ = true;
}

bool BabytechDisplayView::takeIntent(DisplayIntent& intent) {
  if (pendingIntent_ == DisplayIntent::None) return false;
  intent = pendingIntent_;
  pendingIntent_ = DisplayIntent::None;
  return true;
}

void BabytechDisplayView::startButtonEvent(lv_event_t* event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  auto* view = static_cast<BabytechDisplayView*>(lv_event_get_user_data(event));
  if (view != nullptr && !lv_obj_has_state(view->startButton_, LV_STATE_DISABLED) &&
      view->pendingIntent_ == DisplayIntent::None)
    view->pendingIntent_ = DisplayIntent::StartFeeding;
}

void BabytechDisplayView::initializeButtonEvent(lv_event_t* event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  auto* view = static_cast<BabytechDisplayView*>(lv_event_get_user_data(event));
  if (view != nullptr && !lv_obj_has_state(view->initializeButton_, LV_STATE_DISABLED) &&
      view->pendingIntent_ == DisplayIntent::None)
    view->pendingIntent_ = DisplayIntent::Initialize;
}

void BabytechDisplayView::createUi() {
  lv_obj_t* screen = lv_scr_act();
  setContainerStyle(screen, kPageBackground);

  lv_obj_t* title = lv_label_create(screen);
  lv_label_set_text(title, "BabyTech");
  setTextStyle(title, &lv_font_montserrat_20, kPrimaryText);
  lv_obj_set_pos(title, 18, 11);

  cloudLabel_ = lv_label_create(screen);
  lv_label_set_text(cloudLabel_, "Cloud offline");
  setTextStyle(cloudLabel_, &lv_font_montserrat_14, kMutedText);
  lv_obj_align(cloudLabel_, LV_ALIGN_TOP_RIGHT, -18, 15);

  statusCard_ = lv_obj_create(screen);
  lv_obj_set_size(statusCard_, 164, 164);
  lv_obj_set_pos(statusCard_, 158, 40);
  setContainerStyle(statusCard_, kCircleBackground);
  lv_obj_set_style_radius(statusCard_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_border_width(statusCard_, 6, LV_PART_MAIN);
  lv_obj_set_style_border_color(statusCard_, lv_color_hex(kRingNeutral),
                                LV_PART_MAIN);

  statusTitleLabel_ = lv_label_create(statusCard_);
  lv_label_set_text(statusTitleLabel_, feedingFlowUiStage("idle")->title);
  setTextStyle(statusTitleLabel_, &lv_font_montserrat_14, kSecondaryText);
  lv_obj_set_size(statusTitleLabel_, 144, 40);
  lv_obj_set_pos(statusTitleLabel_, 10, 21);
  lv_obj_set_style_text_align(statusTitleLabel_, LV_TEXT_ALIGN_CENTER,
                              LV_PART_MAIN);
  lv_label_set_long_mode(statusTitleLabel_, LV_LABEL_LONG_WRAP);

  amountLabel_ = lv_label_create(statusCard_);
  lv_label_set_text(amountLabel_, "-- mL");
  setTextStyle(amountLabel_, &lv_font_montserrat_28, kPrimaryText);
  lv_obj_set_width(amountLabel_, 144);
  lv_obj_set_pos(amountLabel_, 10, 69);
  lv_obj_set_style_text_align(amountLabel_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

  targetTempLabel_ = lv_label_create(statusCard_);
  lv_label_set_text(targetTempLabel_, "Target --");
  setTextStyle(targetTempLabel_, &lv_font_montserrat_14, kSecondaryText);
  lv_obj_set_width(targetTempLabel_, 144);
  lv_obj_set_pos(targetTempLabel_, 10, 113);
  lv_obj_set_style_text_align(targetTempLabel_, LV_TEXT_ALIGN_CENTER,
                              LV_PART_MAIN);

  statusDetailLabel_ = lv_label_create(screen);
  lv_label_set_text(statusDetailLabel_, feedingFlowUiStage("idle")->detail);
  setTextStyle(statusDetailLabel_, &lv_font_montserrat_14, kSecondaryText);
  lv_obj_set_size(statusDetailLabel_, 444, 34);
  lv_obj_set_pos(statusDetailLabel_, 18, 205);
  lv_obj_set_style_text_align(statusDetailLabel_, LV_TEXT_ALIGN_CENTER,
                              LV_PART_MAIN);
  lv_label_set_long_mode(statusDetailLabel_, LV_LABEL_LONG_WRAP);

  lv_obj_t* recipeCard = lv_obj_create(screen);
  lv_obj_set_size(recipeCard, 218, 68);
  lv_obj_set_pos(recipeCard, 16, 241);
  setCardStyle(recipeCard, kCardBackground);

  lv_obj_t* recipeHeading = lv_label_create(recipeCard);
  lv_label_set_text(recipeHeading, "Current bottle");
  setTextStyle(recipeHeading, &lv_font_montserrat_14, kMutedText);
  lv_obj_set_pos(recipeHeading, 12, 11);

  recipeLabel_ = lv_label_create(recipeCard);
  lv_label_set_text(recipeLabel_, "--");
  setTextStyle(recipeLabel_, &lv_font_montserrat_14, kSecondaryText);
  lv_obj_set_size(recipeLabel_, 194, 20);
  lv_obj_set_pos(recipeLabel_, 12, 37);
  lv_label_set_long_mode(recipeLabel_, LV_LABEL_LONG_DOT);

  startButton_ = lv_btn_create(screen);
  lv_obj_set_size(startButton_, 218, 68);
  lv_obj_set_pos(startButton_, 246, 241);
  lv_obj_set_style_radius(startButton_, 18, LV_PART_MAIN);
  lv_obj_set_style_bg_color(startButton_, lv_color_hex(kAction), LV_PART_MAIN);
  constexpr lv_style_selector_t kDisabledButton =
      static_cast<lv_style_selector_t>(LV_PART_MAIN) |
      static_cast<lv_style_selector_t>(LV_STATE_DISABLED);
  lv_obj_set_style_bg_color(startButton_, lv_color_hex(kDisabled),
                            kDisabledButton);
  lv_obj_set_style_shadow_width(startButton_, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(startButton_, startButtonEvent, LV_EVENT_CLICKED, this);

  startButtonLabel_ = lv_label_create(startButton_);
  lv_label_set_text(startButtonLabel_, feedingFlowUiAction("start"));
  setTextStyle(startButtonLabel_, &lv_font_montserrat_20, kWhite);
  lv_obj_set_width(startButtonLabel_, 190);
  lv_label_set_long_mode(startButtonLabel_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(startButtonLabel_, LV_TEXT_ALIGN_CENTER,
                              LV_PART_MAIN);
  lv_obj_center(startButtonLabel_);
  lv_obj_add_state(startButton_, LV_STATE_DISABLED);

  if (allowInitialization_) {
    initializeButton_ = lv_btn_create(screen);
    lv_obj_set_size(initializeButton_, 128, 48);
    lv_obj_set_pos(initializeButton_, 336, 112);
    lv_obj_set_style_radius(initializeButton_, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(initializeButton_, lv_color_hex(kAction), LV_PART_MAIN);
    lv_obj_set_style_bg_color(initializeButton_, lv_color_hex(kDisabled), kDisabledButton);
    lv_obj_set_style_shadow_width(initializeButton_, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(initializeButton_, initializeButtonEvent, LV_EVENT_CLICKED, this);
    lv_obj_t* label = lv_label_create(initializeButton_);
    lv_label_set_text(label, feedingFlowUiAction("initialize"));
    setTextStyle(label, &lv_font_montserrat_14, kWhite);
    lv_obj_center(label);
    lv_obj_add_state(initializeButton_, LV_STATE_DISABLED);
  }
}

void BabytechDisplayView::applySnapshot(const DisplaySnapshot& snapshot,
                                        bool controllerConnected) {
  const DisplayStage effectiveStage =
      controllerConnected ? snapshot.stage : DisplayStage::Offline;
  const FeedingFlowUiStage* stage =
      feedingFlowUiStage(displayStageKey(effectiveStage));

  const DisplayCondition primaryCondition = !controllerConnected
      ? DisplayCondition::ControllerOffline
      : snapshot.primaryCondition;
  DisplayStage visibleStage = effectiveStage;
  if (primaryCondition == DisplayCondition::ControllerOffline ||
      primaryCondition == DisplayCondition::WifiSetup) {
    visibleStage = DisplayStage::Offline;
  } else if (primaryCondition == DisplayCondition::ProtocolIncompatible) {
    visibleStage = DisplayStage::Error;
  } else if (primaryCondition != DisplayCondition::None) {
    visibleStage = DisplayStage::NotReady;
  }
  lv_label_set_text(statusTitleLabel_,
                    feedingFlowUiStage(displayStageKey(visibleStage))->title);

  if (primaryCondition == DisplayCondition::ControllerOffline) {
    lv_label_set_text(statusDetailLabel_, stage->detail);
  } else if (primaryCondition != DisplayCondition::None) {
    lv_label_set_text(statusDetailLabel_,
                      feedingFlowUiCondition(displayConditionKey(primaryCondition)));
  } else if (effectiveStage == DisplayStage::Error &&
             snapshot.error != DisplayError::None) {
    lv_label_set_text(statusDetailLabel_,
                      feedingFlowUiError(displayErrorKey(snapshot.error)));
  } else if ((effectiveStage == DisplayStage::Idle ||
              effectiveStage == DisplayStage::Ready ||
              effectiveStage == DisplayStage::NotReady) &&
             snapshot.footerCondition != DisplayCondition::None &&
             snapshot.footerCondition != DisplayCondition::Ready) {
    lv_label_set_text(statusDetailLabel_,
                      feedingFlowUiCondition(
                          displayConditionKey(snapshot.footerCondition)));
  } else if (effectiveStage == DisplayStage::Complete &&
             snapshot.footerCondition == DisplayCondition::PreparedBottlePresent) {
    lv_label_set_text(statusDetailLabel_,
                      feedingFlowUiCondition("prepared_bottle_present"));
  } else {
    resolveDetail(*stage, snapshot.babyName.data(), resolvedDetail_,
                  sizeof(resolvedDetail_));
    lv_label_set_text(statusDetailLabel_, resolvedDetail_);
  }

  lv_label_set_text(startButtonLabel_,
                    visibleStage == DisplayStage::Complete
                        ? feedingFlowUiAction("complete")
                        : isFeedingStage(visibleStage)
                              ? feedingFlowUiAction("preparing")
                              : feedingFlowUiAction("start"));

  if (!controllerConnected) {
    lv_label_set_text(cloudLabel_, "Controller offline");
  } else {
    lv_label_set_text(cloudLabel_,
                      snapshot.cloudConnected ? "Cloud online" : "Cloud offline");
  }
  lv_obj_set_style_text_color(
      cloudLabel_,
      lv_color_hex(controllerConnected && snapshot.cloudConnected
                       ? kRingReady
                       : kRingError),
      LV_PART_MAIN);

  if (snapshot.waterMl > 0) {
    lv_label_set_text_fmt(amountLabel_, "%u mL",
                          static_cast<unsigned>(snapshot.waterMl));
  } else {
    lv_label_set_text(amountLabel_, "-- mL");
  }
  if (snapshot.temperatureC > 0) {
    lv_label_set_text_fmt(targetTempLabel_,
                          snapshot.thermalSimulated
                              ? "Target %d\xC2\xB0" "C sim"
                              : "Target %d\xC2\xB0" "C",
                          static_cast<int>(snapshot.temperatureC));
  } else {
    lv_label_set_text(targetTempLabel_, "Target --");
  }
  if (snapshot.babyName[0] != '\0') {
    lv_label_set_text_fmt(recipeLabel_, "%s  |  %s",
                          snapshot.formulaBrand.data(),
                          snapshot.babyName.data());
  } else {
    lv_label_set_text(recipeLabel_, snapshot.formulaBrand[0] != '\0'
                                        ? snapshot.formulaBrand.data()
                                        : "--");
  }
  applyTone(visibleStage);
  if (controllerConnected && snapshot.startEnabled) {
    lv_obj_clear_state(startButton_, LV_STATE_DISABLED);
    setTextStyle(startButtonLabel_, &lv_font_montserrat_20, kWhite);
  } else {
    lv_obj_add_state(startButton_, LV_STATE_DISABLED);
    setTextStyle(startButtonLabel_, &lv_font_montserrat_14, kSecondaryText);
  }
}

void BabytechDisplayView::applyTone(DisplayStage stage) {
  uint32_t ringColor = kRingNeutral;
  switch (toneFor(stage)) {
    case FeedingFlowUiTone::Ready:
      ringColor = kRingReady;
      break;
    case FeedingFlowUiTone::Active:
      ringColor = kRingActive;
      break;
    case FeedingFlowUiTone::Success:
      ringColor = kRingSuccess;
      break;
    case FeedingFlowUiTone::Error:
      ringColor = kRingError;
      break;
    case FeedingFlowUiTone::Neutral:
      break;
  }
  lv_obj_set_style_border_color(statusCard_, lv_color_hex(ringColor),
                                LV_PART_MAIN);
  lv_obj_set_style_text_color(statusTitleLabel_, lv_color_hex(ringColor),
                              LV_PART_MAIN);
}

}  // namespace babytech::display
