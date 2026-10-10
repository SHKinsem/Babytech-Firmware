#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

using String = std::string;
uint32_t millis();
struct HostSerial {
    void println(const char*) {}
};
inline HostSerial Serial;
