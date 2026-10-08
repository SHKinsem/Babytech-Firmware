#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <type_traits>
#include <vector>

class __FlashStringHelper;
#define F(value) reinterpret_cast<const __FlashStringHelper*>(value)
#define PROGMEM
using PGM_P = const char*;
inline unsigned char pgm_read_byte(const void* p) { return *static_cast<const unsigned char*>(p); }
inline const void* pgm_read_ptr(const void* p) { return *static_cast<const void* const*>(p); }
#define memcpy_P memcpy
#define strlen_P strlen
#define strcmp_P strcmp

class String {
public:
    String() = default;
    String(const char* v) : value_(v ? v : "") {}
    String(const char* v, unsigned int n) : value_(v, n) {}
    String(const __FlashStringHelper* v) : String(reinterpret_cast<const char*>(v)) {}
    String(const std::string& v) : value_(v) {}
    explicit String(char v) : value_(1, v) {}
    template<class T, typename std::enable_if<std::is_integral<T>::value, int>::type = 0>
    explicit String(T v) : value_(std::to_string(v)) {}
    String(double v, unsigned char digits = 2) {
        char b[128]; std::snprintf(b, sizeof b, "%.*f", int(digits), v); value_ = b;
    }
    const char* c_str() const { return value_.c_str(); }
    const std::string& str() const { return value_; }
    unsigned int length() const { return unsigned(value_.size()); }
    bool isEmpty() const { return value_.empty(); }
    bool reserve(unsigned int n) { value_.reserve(n); return true; }
    char operator[](size_t i) const { return value_[i]; }
    char& operator[](size_t i) { return value_[i]; }
    bool operator==(const String& b) const { return value_ == b.value_; }
    bool operator!=(const String& b) const { return !(*this == b); }
    bool equals(const String& b) const { return *this == b; }
    long toInt() const { return std::strtol(c_str(), nullptr, 10); }
    float toFloat() const { return std::strtof(c_str(), nullptr); }
    int indexOf(char c, unsigned from = 0) const { return index(value_.find(c, from)); }
    int indexOf(const String& s, unsigned from = 0) const { return index(value_.find(s.value_, from)); }
    bool startsWith(const String& s, unsigned from = 0) const { return value_.compare(from, s.length(), s.value_) == 0; }
    String substring(unsigned from, unsigned to) const { return String(value_.substr(std::min(size_t(from), value_.size()), to > from ? to - from : 0)); }
    String substring(unsigned from) const { return substring(from, length()); }
    void trim() {
        const auto start = value_.find_first_not_of(" \r\n\t");
        value_ = start == std::string::npos ? "" : value_.substr(start, value_.find_last_not_of(" \r\n\t") - start + 1);
    }
    bool concat(const char* v, unsigned int n) { value_.append(v, n); return true; }
    bool concat(const char* v) { value_ += v; return true; }
    bool concat(char c) { value_ += c; return true; }
    void toCharArray(char* out, unsigned int cap) const {
        if (!cap) return;
        const size_t n = std::min(size_t(cap - 1), value_.size());
        std::memcpy(out, value_.data(), n); out[n] = 0;
    }
    String& operator+=(const String& v) { value_ += v.value_; return *this; }
    String& operator+=(char v) { value_ += v; return *this; }
    template<class T, typename std::enable_if<std::is_integral<T>::value, int>::type = 0>
    String& operator+=(T v) { value_ += std::to_string(v); return *this; }
    friend String operator+(String a, const String& b) { a += b; return a; }
    friend String operator+(String a, char b) { a += b; return a; }
private:
    static int index(size_t n) { return n == std::string::npos ? -1 : int(n); }
    std::string value_;
};

inline size_t strlcpy(char* target, const char* source, size_t capacity) {
    const size_t n = std::strlen(source);
    if (capacity) { const size_t copy = std::min(n, capacity - 1); std::memcpy(target, source, copy); target[copy] = 0; }
    return n;
}
constexpr int LOW = 0, HIGH = 1, INPUT = 1, OUTPUT = 3, INPUT_PULLUP = 5, INPUT_PULLDOWN = 9;
constexpr uint32_t SERIAL_8N1 = 0x800001c;
template<class T> T constrain(T v, T low, T high) { return std::min(high, std::max(low, v)); }
unsigned long millis();
void delay(unsigned long);
void delayMicroseconds(unsigned);
void pinMode(int, int);
void digitalWrite(int, int);
int digitalRead(int);
void noInterrupts();
void interrupts();

namespace motion_io {
inline uint32_t now = 1000;
inline std::deque<uint8_t> uartRx, usbRx;
inline std::vector<uint8_t> uartTx;
inline std::string usbTx;
inline unsigned restarts = 0;
inline bool critical = false;
}
class HardwareSerial {
public:
    explicit HardwareSerial(uint8_t n = 0) : number_(n) {}
    size_t setRxBufferSize(size_t n) { return n; }
    size_t setTxBufferSize(size_t n) { return n; }
    void begin(unsigned long, uint32_t = SERIAL_8N1, int8_t = -1, int8_t = -1) {}
    void setTxTimeoutMs(unsigned) {}
    explicit operator bool() const { return true; }
    int available() { return int(rx().size()); }
    int availableForWrite() { return 64; }
    int read() { auto& q = rx(); if (q.empty()) return -1; const int c = q.front(); q.pop_front(); return c; }
    size_t write(const uint8_t* p, size_t n) {
        if (number_ == 0) motion_io::usbTx.append(reinterpret_cast<const char*>(p), n);
        else motion_io::uartTx.insert(motion_io::uartTx.end(), p, p + n);
        return n;
    }
    void print(const String& s) { write(reinterpret_cast<const uint8_t*>(s.c_str()), s.length()); }
    void print(int n) { print(String(n)); }
    void println(const String& s = String()) { print(s); print("\n"); }
    void println(int n) { println(String(n)); }
    template<class... A> void printf(const char* fmt, A... args) {
        char b[2048]; const int n = std::snprintf(b, sizeof b, fmt, args...);
        if (n > 0) write(reinterpret_cast<const uint8_t*>(b), std::min(size_t(n), sizeof b - 1));
    }
private:
    std::deque<uint8_t>& rx() { return number_ == 0 ? motion_io::usbRx : motion_io::uartRx; }
    uint8_t number_;
};
inline HardwareSerial Serial(0);
struct EspClass {
    uint64_t getEfuseMac() const { return 0xffeeddccbbaaULL; }
    void restart() { ++motion_io::restarts; }
};
inline EspClass ESP;
