#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace babytech { namespace boardlink { namespace detail {

// ArduinoJson 6 deliberately accepts relaxed JSON. This guard checks the
// bounded flat wire grammar; decoded object size must also equal fieldCount to
// reject duplicate keys (including differently escaped spellings).
class FlatJsonGuard {
public:
    FlatJsonGuard(const uint8_t* bytes, size_t length, size_t maxFields,
                  bool allowFraction = false)
        : bytes_(bytes), size_(length), maxFields_(maxFields), allowFraction_(allowFraction) {}

    bool object(size_t& fieldCount) {
        fieldCount = 0;
        skipSpace();
        if (!take('{')) return false;
        skipSpace();
        if (take('}')) return finish();
        do {
            skipSpace();
            if (!string()) return false;
            skipSpace();
            if (!take(':')) return false;
            skipSpace();
            if (!value() || ++fieldCount > maxFields_) return false;
            skipSpace();
            if (take('}')) return finish();
        } while (take(','));
        return false;
    }

private:
    static bool digit(uint8_t c) { return c >= '0' && c <= '9'; }
    static int hex(uint8_t c) {
        if (digit(c)) return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    bool take(uint8_t c) {
        if (at_ == size_ || bytes_[at_] != c) return false;
        ++at_;
        return true;
    }
    void skipSpace() {
        while (at_ < size_ && (bytes_[at_] == ' ' || bytes_[at_] == '\t' ||
               bytes_[at_] == '\r' || bytes_[at_] == '\n')) ++at_;
    }
    bool finish() { skipSpace(); return at_ == size_; }
    bool codeUnit(unsigned& unit) {
        unit = 0;
        for (size_t i = 0; i < 4; ++i) {
            if (at_ == size_) return false;
            const int nibble = hex(bytes_[at_++]);
            if (nibble < 0) return false;
            unit = (unit << 4) | unsigned(nibble);
        }
        return true;
    }
    bool string() {
        if (!take('"')) return false;
        while (at_ < size_) {
            const uint8_t c = bytes_[at_++];
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') continue;
            if (at_ == size_) return false;
            const uint8_t escape = bytes_[at_++];
            if (escape == 'u') {
                unsigned unit;
                if (!codeUnit(unit) || !unit) return false;
                if (unit >= 0xd800 && unit <= 0xdbff) {
                    if (!take('\\') || !take('u') || !codeUnit(unit) ||
                        unit < 0xdc00 || unit > 0xdfff) return false;
                } else if (unit >= 0xdc00 && unit <= 0xdfff) return false;
            } else if (escape != '"' && escape != '\\' && escape != '/' &&
                       escape != 'b' && escape != 'f' && escape != 'n' &&
                       escape != 'r' && escape != 't') return false;
        }
        return false;
    }
    bool literal(const char* text, size_t length) {
        if (length > size_ - at_ || std::memcmp(bytes_ + at_, text, length)) return false;
        at_ += length;
        return true;
    }
    bool digits() {
        const size_t start = at_;
        while (at_ < size_ && digit(bytes_[at_])) ++at_;
        return at_ != start;
    }
    bool value() {
        if (at_ == size_) return false;
        if (bytes_[at_] == '"') return string();
        if (bytes_[at_] == 't') return literal("true", 4);
        if (bytes_[at_] == 'f') return literal("false", 5);
        if (bytes_[at_] == 'n') return literal("null", 4);
        take('-');
        if (!take('0') && !digits()) return false;
        if (!allowFraction_) return true;
        if (take('.') && !digits()) return false;
        if (take('e') || take('E')) {
            if (!take('+')) take('-');
            if (!digits()) return false;
        }
        return true;
    }
    const uint8_t* bytes_;
    size_t size_;
    size_t maxFields_;
    bool allowFraction_;
    size_t at_ = 0;
};

} } }
