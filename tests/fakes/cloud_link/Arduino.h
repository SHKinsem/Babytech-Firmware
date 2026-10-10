#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>

#define F(value) value

class String {
public:
    String() = default;
    String(const char* value) : value_(value ? value : "") {}
    String(const std::string& value) : value_(value) {}
    const char* c_str() const { return value_.c_str(); }
    size_t length() const { return value_.size(); }
    bool isEmpty() const { return value_.empty(); }
    bool operator==(const char* value) const { return value_ == value; }
    char operator[](size_t i) const { return value_[i]; }
    long toInt() const { return std::strtol(c_str(), nullptr, 10); }
    void toCharArray(char* output, size_t capacity) const {
        if (!capacity) return;
        const size_t count = value_.size() < capacity - 1 ? value_.size() : capacity - 1;
        std::memcpy(output, value_.data(), count);
        output[count] = 0;
    }
    String& operator+=(const char* value) { value_ += value; return *this; }
    String& operator+=(const String& value) { value_ += value.value_; return *this; }
    template <typename T, typename = typename std::enable_if<std::is_integral<T>::value>::type>
    String& operator+=(T value) { value_ += std::to_string(value); return *this; }
private:
    std::string value_;
};

inline size_t cloudFakeStrlcpy(char* target, const char* source, size_t capacity) {
    const size_t length = std::strlen(source);
    if (capacity) {
        const size_t count = length < capacity - 1 ? length : capacity - 1;
        std::memcpy(target, source, count);
        target[count] = 0;
    }
    return length;
}
#define strlcpy cloudFakeStrlcpy

uint32_t millis();
struct FakeSerial { void println(const char* message); };
extern FakeSerial Serial;
