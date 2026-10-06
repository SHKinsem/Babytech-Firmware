#pragma once

#include <cstdint>

namespace babytech::display {

constexpr uint16_t kPortraitTouchWidth = 320;
constexpr uint16_t kPortraitTouchHeight = 480;
constexpr uint16_t kLandscapeWidth = kPortraitTouchHeight;
constexpr uint16_t kLandscapeHeight = kPortraitTouchWidth;
// Rotate the landscape panel 180 degrees for the inverted V1 mounting.
constexpr uint8_t kLandscapeMadctl = 0xE8;

inline bool portraitTouchToLandscape(uint16_t rawX, uint16_t rawY,
                                     uint16_t& x, uint16_t& y) {
  if (rawX >= kPortraitTouchWidth || rawY >= kPortraitTouchHeight) {
    return false;
  }
  // The GT1151 coordinates stay fixed to the physical panel when MADCTL
  // rotates the LCD output for the inverted mounting.
  x = kLandscapeWidth - 1 - rawY;
  y = rawX;
  return true;
}

}  // namespace babytech::display
