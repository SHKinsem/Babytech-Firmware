#include "babytech_display_view.h"
#include "generated/feeding_flow_ui.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
using namespace babytech::display;

static lv_color_t pixels[480 * 320];
static lv_color_t buffer[480 * 40];
static void flush(lv_disp_drv_t* driver, const lv_area_t* area, lv_color_t* colors) {
  for (int y = area->y1; y <= area->y2; ++y)
    for (int x = area->x1; x <= area->x2; ++x)
      pixels[y * 480 + x] = *colors++;
  lv_disp_flush_ready(driver);
}
static lv_obj_t* label(lv_obj_t* parent, const char* text) {
  if (lv_obj_check_type(parent, &lv_label_class) &&
      std::strcmp(lv_label_get_text(parent), text) == 0) return parent;
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(parent); ++i)
    if (auto* found = label(lv_obj_get_child(parent, i), text)) return found;
  return nullptr;
}
int main(int argc, char** argv) {
  lv_init();
  lv_disp_draw_buf_t draw; lv_disp_draw_buf_init(&draw, buffer, nullptr, 480 * 40);
  lv_disp_drv_t driver; lv_disp_drv_init(&driver);
  driver.hor_res = 480; driver.ver_res = 320; driver.draw_buf = &draw; driver.flush_cb = flush;
  lv_disp_drv_register(&driver);
  BabytechDisplayView view; assert(view.begin(true));
  auto* init = lv_obj_get_parent(label(lv_scr_act(), "Initialize"));
  auto* start = lv_obj_get_parent(label(lv_scr_act(), "Start feeding"));
  assert(lv_obj_has_state(init, LV_STATE_DISABLED));
  DisplaySnapshot s; s.stage = DisplayStage::NotReady;
  s.waterMl = 180; s.temperatureC = 40; s.thermalSimulated = true;
  view.update(s, true);
  const auto* notReady = feedingFlowUiStage("noready");
  assert(std::strcmp(notReady->detail, "Not ready") == 0);
  assert(std::strcmp(notReady->fallbackDetail, "Not ready") == 0);
  assert(label(lv_scr_act(), "Check bottle, water and formula") == nullptr);
  assert(!lv_obj_has_state(init, LV_STATE_DISABLED));
  assert(lv_obj_has_state(start, LV_STATE_DISABLED));
  lv_obj_update_layout(lv_scr_act());
  lv_area_t a; lv_obj_get_coords(init, &a);
  assert(a.x1 == 336 && a.y1 == 112 && a.x2 == 463 && a.y2 == 159);
  lv_obj_get_coords(start, &a);
  assert(a.x1 == 246 && a.y1 == 241 && a.x2 == 463 && a.y2 == 308);
  DisplayIntent intent;
  lv_event_send(start, LV_EVENT_CLICKED, nullptr); assert(!view.takeIntent(intent));
  lv_event_send(init, LV_EVENT_CLICKED, nullptr);
  assert(view.takeIntent(intent) && intent == DisplayIntent::Initialize);
  view.update(s, true, true); // identical snapshot but ACK now pending
  assert(lv_obj_has_state(init, LV_STATE_DISABLED));
  lv_event_send(init, LV_EVENT_CLICKED, nullptr); assert(!view.takeIntent(intent));
  view.update(s, true, false); assert(!lv_obj_has_state(init, LV_STATE_DISABLED));
  if (argc == 2) {
    lv_tick_inc(300); lv_timer_handler(); // let the default disabled-style transition finish
    lv_refr_now(nullptr);
    FILE* image = std::fopen(argv[1], "wb"); assert(image);
    std::fprintf(image, "P6\n480 320\n255\n");
    for (auto color : pixels) {
      const auto rgb = lv_color_to32(color);
      std::fputc((rgb >> 16) & 255, image); std::fputc((rgb >> 8) & 255, image); std::fputc(rgb & 255, image);
    }
    std::fclose(image);
  }
  s.stage = DisplayStage::Ready; s.startEnabled = true; view.update(s, true);
  assert(lv_obj_has_state(init, LV_STATE_DISABLED) && !lv_obj_has_state(start, LV_STATE_DISABLED));
  lv_event_send(start, LV_EVENT_CLICKED, nullptr);
  assert(view.takeIntent(intent) && intent == DisplayIntent::StartFeeding);
  s.startEnabled = false;
  for (auto stage : {DisplayStage::Mixing, DisplayStage::Complete, DisplayStage::Offline}) {
    s.stage = stage; view.update(s, true);
    assert(lv_obj_has_state(init, LV_STATE_DISABLED));
  }
  s.stage = DisplayStage::Error; view.update(s, true);
  assert(!lv_obj_has_state(init, LV_STATE_DISABLED));
  view.update(s, false); assert(lv_obj_has_state(init, LV_STATE_DISABLED));
  s.primaryCondition = DisplayCondition::ProtocolIncompatible; view.update(s, true);
  assert(lv_obj_has_state(init, LV_STATE_DISABLED));
  lv_obj_clean(lv_scr_act());
  BabytechDisplayView local; assert(local.begin());
  assert(label(lv_scr_act(), "Initialize") == nullptr);
  std::cout << "PASS LVGL button layout, intents, offline/busy/stage gates, unchanged Start and default local UI\n";
}
