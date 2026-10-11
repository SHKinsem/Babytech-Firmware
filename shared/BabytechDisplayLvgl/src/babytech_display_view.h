#pragma once

#include <cstdint>

#include <lvgl.h>

#include "display_model.h"

namespace babytech::display {

class BabytechDisplayView {
 public:
  bool begin(bool allowInitialization = false);
  void poll(uint32_t nowMs);
  void update(const DisplaySnapshot& snapshot, bool controllerConnected,
              bool intentPending = false);
  bool takeIntent(DisplayIntent& intent);

 private:
  static void startButtonEvent(lv_event_t* event);
  static void initializeButtonEvent(lv_event_t* event);

  void createUi();
  void applySnapshot(const DisplaySnapshot& snapshot,
                     bool controllerConnected);
  void applyTone(DisplayStage stage);

  bool ready_ = false;
  bool hasSnapshot_ = false;
  bool lastControllerConnected_ = false;
  bool allowInitialization_ = false;
  bool lastIntentPending_ = false;
  uint32_t lastTickMs_ = 0;
  DisplaySnapshot lastSnapshot_{};
  DisplayIntent pendingIntent_ = DisplayIntent::None;
  char resolvedDetail_[96]{};

  lv_obj_t* cloudLabel_ = nullptr;
  lv_obj_t* statusCard_ = nullptr;
  lv_obj_t* statusTitleLabel_ = nullptr;
  lv_obj_t* statusDetailLabel_ = nullptr;
  lv_obj_t* amountLabel_ = nullptr;
  lv_obj_t* targetTempLabel_ = nullptr;
  lv_obj_t* recipeLabel_ = nullptr;
  lv_obj_t* startButton_ = nullptr;
  lv_obj_t* startButtonLabel_ = nullptr;
  lv_obj_t* initializeButton_ = nullptr;
};

}  // namespace babytech::display
