#pragma once
#include <stdint.h>
#include <cstring>
constexpr int OUTPUT=1, INPUT_PULLUP=2;
inline void pinMode(int,int) {}
void delay(unsigned long ms);
template<class T> T constrain(T value,T low,T high) { return value<low?low:value>high?high:value; }
struct FakeSerial {
    template<class... T> void printf(const char*,T...) {}
    template<class T> void print(T) {}
    template<class T> void println(T) {}
};
extern FakeSerial Serial;
