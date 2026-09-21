#pragma once
// Host stand-in for the tiny slice of the Arduino core that the motion module
// actually uses: the String class and millis(). Nothing here touches hardware.
//
// The implementation is intentionally minimal but must cover every operation
// MotorControl.cpp performs on statusJson(): reserve, += for integers, chars,
// C strings and other Strings, plus the fixed precision float constructor.

#include <stddef.h>
#include <stdint.h>

#include <cstdio>
#include <string>

// Milliseconds clock. The value is owned by the fake CAN driver so tests can
// drive it deterministically (see tests/fakes/fake_x42s.*).
unsigned long millis();

class String {
public:
    String() {}
    String(const char* text) : value_(text ? text : "") {}
    String(const std::string& text) : value_(text) {}
    explicit String(char c) : value_(1, c) {}
    explicit String(int v) { assignNumber("%d", v); }
    explicit String(unsigned int v) { assignNumber("%u", v); }
    explicit String(long v) { assignNumber("%ld", v); }
    explicit String(unsigned long v) { assignNumber("%lu", v); }
    String(float value, int decimals = 2) { assignFixed(value, decimals); }
    String(double value, int decimals = 2) { assignFixed(value, decimals); }

    void reserve(size_t bytes) { value_.reserve(bytes); }
    unsigned int length() const { return static_cast<unsigned int>(value_.size()); }
    const char* c_str() const { return value_.c_str(); }
    // std::string view of the buffer, used by the tests to assert statusJson().
    const std::string& str() const { return value_; }

    String& operator+=(const String& other) { value_ += other.value_; return *this; }
    String& operator+=(const char* text) { if (text) value_ += text; return *this; }
    String& operator+=(char c) { value_ += c; return *this; }
    String& operator+=(int v) { appendNumber("%d", v); return *this; }
    String& operator+=(unsigned int v) { appendNumber("%u", v); return *this; }
    String& operator+=(long v) { appendNumber("%ld", v); return *this; }
    String& operator+=(unsigned long v) { appendNumber("%lu", v); return *this; }

    bool operator==(const char* other) const {
        return other != nullptr && value_ == other;
    }
    bool operator!=(const char* other) const { return !(*this == other); }

private:
    template <typename T>
    void assignNumber(const char* fmt, T v) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), fmt, v);
        value_ = buffer;
    }
    template <typename T>
    void appendNumber(const char* fmt, T v) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), fmt, v);
        value_ += buffer;
    }
    void assignFixed(double v, int decimals) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, v);
        value_ = buffer;
    }

    std::string value_;
};
