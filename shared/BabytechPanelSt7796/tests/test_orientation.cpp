#include <cassert>
#include <cstdint>

#include "babytech_st7796_orientation.h"

int main() {
  using babytech::display::portraitTouchToLandscape;

  static_assert(babytech::display::kLandscapeMadctl == 0xE8);
  uint16_t x = 0;
  uint16_t y = 0;
  assert(portraitTouchToLandscape(0, 0, x, y));
  assert(x == 479 && y == 0);
  assert(portraitTouchToLandscape(319, 0, x, y));
  assert(x == 479 && y == 319);
  assert(portraitTouchToLandscape(0, 479, x, y));
  assert(x == 0 && y == 0);
  assert(portraitTouchToLandscape(319, 479, x, y));
  assert(x == 0 && y == 319);
  assert(portraitTouchToLandscape(160, 240, x, y));
  assert(x == 239 && y == 160);

  // Measured on the inverted V1 panel at the center of Start feeding.
  assert(portraitTouchToLandscape(278, 121, x, y));
  assert(x == 358 && y == 278);

  assert(!portraitTouchToLandscape(320, 0, x, y));
  assert(!portraitTouchToLandscape(0, 480, x, y));
  return 0;
}
