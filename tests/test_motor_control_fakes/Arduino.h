#pragma once
#include "../fakes/Arduino.h"
#define OUTPUT 1
#define INPUT_PULLUP 2
inline void pinMode(int, int) {}
void delay(unsigned long ms);
template <typename T> T constrain(T value, T low, T high) {
    return value < low ? low : value > high ? high : value;
}
struct MotorTestSerial {
    template <typename T> void print(T) {}
    template <typename T> void println(T) {}
    template <typename... T> void printf(const char*, T...) {}
};
extern MotorTestSerial Serial;
