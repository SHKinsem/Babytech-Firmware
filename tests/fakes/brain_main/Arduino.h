#pragma once

#include "../cloud_link/Arduino.h"
#include "../../../shared/ProductBoardLink/test/arduino_stubs/Arduino.h"

void delay(uint32_t milliseconds);
constexpr int INPUT_PULLUP = 2, LOW = 0;
inline void pinMode(int, int) {}
inline int fakeBootLevel = 1;
inline int digitalRead(int) { return fakeBootLevel; }
