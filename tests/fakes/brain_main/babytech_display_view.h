#pragma once
#include "FakeMainIo.h"

struct lv_event_t {};
struct lv_obj_t {};
constexpr int LV_EVENT_CLICKED = 1, LV_PART_MAIN = 0;
inline int lv_font_montserrat_14 = 14;
inline void (*fakeStopCallback)(lv_event_t*) = nullptr;
inline int lv_event_get_code(lv_event_t*) { return LV_EVENT_CLICKED; }
inline lv_obj_t* lv_scr_act() { static lv_obj_t value; return &value; }
inline lv_obj_t* lv_btn_create(lv_obj_t*) { return lv_scr_act(); }
inline lv_obj_t* lv_label_create(lv_obj_t*) { return lv_scr_act(); }
inline int lv_color_hex(unsigned value) { return int(value); }
inline void lv_obj_set_size(lv_obj_t*, int, int) {}
inline void lv_obj_set_pos(lv_obj_t*, int, int) {}
inline void lv_obj_set_style_radius(lv_obj_t*, int, int) {}
inline void lv_obj_set_style_bg_color(lv_obj_t*, int, int) {}
inline void lv_obj_set_style_shadow_width(lv_obj_t*, int, int) {}
inline void lv_obj_set_style_text_color(lv_obj_t*, int, int) {}
inline void lv_obj_set_style_text_font(lv_obj_t*, const int*, int) {}
inline void lv_obj_center(lv_obj_t*) {}
inline void lv_label_set_text(lv_obj_t*, const char*) {}
inline void lv_obj_add_event_cb(lv_obj_t*, void (*callback)(lv_event_t*), int, void*) {
    fakeStopCallback = callback;
}
namespace babytech { namespace display {
class BabytechDisplayView {
public:
    bool begin(bool) { return true; }
    void poll(uint32_t) {
        if (fake_main::stopClick && fakeStopCallback) {
            fake_main::stopClick = false;
            lv_event_t event;
            fakeStopCallback(&event);
        }
    }
    void update(const DisplaySnapshot& value, bool connected, bool pending) {
        fake_main::shown = value;
        fake_main::shownConnected = connected;
        fake_main::shownPending = pending;
    }
    bool takeIntent(DisplayIntent& value) {
        value = fake_main::intent;
        fake_main::intent = DisplayIntent::None;
        return value != DisplayIntent::None;
    }
};
} }
