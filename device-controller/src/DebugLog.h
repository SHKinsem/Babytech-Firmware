#pragma once
// Fixed-capacity, RAM-only diagnostic log for the motion board.
//
// Why it exists: the CAN trace explains what happened on the bus, but not what
// the board itself was doing when something went wrong. This log keeps a short,
// ordered history of state transitions and HTTP results so the browser can show
// "what happened" without a serial console attached.
//
// Hard rules (do not relax without re-reading the contract):
//   * RAM only. Nothing is written to flash, and no serial output is added.
//   * Every buffer is fixed: the ring, the event name, the level and the detail
//     text are all bounded, so logging can never grow the heap or a stack frame.
//   * Single-threaded use from the main loop and from the HTTP handlers, which
//     run in the same task on this board.
//   * Header-only and free of Arduino/ESP-IDF dependencies, so the ring, the
//     escaping and the JSON writer are unit tested on the host.
//
// It observes; it never decides. Nothing here can change motion policy, send a
// frame or alter an HTTP response.

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace motion {

class DebugLog {
public:
    // Sizes first: the event type below is built from them.
    static constexpr uint8_t kCapacity = 48;   // events kept (ring)
    static constexpr size_t kDetailMax = 180;  // bounded detail text
    static constexpr size_t kEventMax = 24;
    static constexpr size_t kLevelMax = 8;
    static constexpr size_t kBootIdMax = 20;

    // One event. Declared before the members that use it: a nested type cannot be
    // referenced before its declaration, not even inside member function bodies.
    struct Entry {
        uint32_t seq = 0;
        uint32_t atMs = 0;
        bool truncated = false;
        char level[kLevelMax] = {};
        char event[kEventMax] = {};
        char detail[kDetailMax + 1] = {};
    };

    // Response buffer size that holds the whole ring when nothing needs escaping
    // (event overhead + level + name + detail). It is NOT a worst-case bound: a
    // detail of control bytes escapes to six characters each, so writeJson() is
    // written to fit ANY buffer by emitting the newest events that fit and
    // reporting the rest as "omitted" rather than returning an empty document.
    static constexpr size_t kJsonMax = kCapacity * (kDetailMax + 128) + 256;

    // Starts a fresh session: clears the ring and remembers the boot identity the
    // caller generated (a hex string; truncation is explicit).
    void begin(const char* bootId) {
        head_ = 0;
        count_ = 0;
        sequence_ = 0;
        bootId_[0] = '\0';
        if (bootId) copyText(bootId_, kBootIdMax, bootId);
    }

    // Appends one event. `detail` may be null or empty. A detail longer than
    // kDetailMax is truncated AND flagged, so a reader is never misled about it.
    // `truncated` reports a cut the caller already knows about (see addf).
    void add(uint32_t nowMs, const char* level, const char* event, const char* detail = nullptr,
             bool truncated = false) {
        Entry& entry = entries_[head_];
        const char* levelText = level ? level : "info";
        const char* eventText = event ? event : "event";
        copyText(entry.level, kLevelMax, levelText);
        copyText(entry.event, kEventMax, eventText);
        const size_t length = detail ? strlen(detail) : 0;
        const size_t kept = length > kDetailMax ? kDetailMax : length;
        if (detail) memcpy(entry.detail, detail, kept);
        entry.detail[kept] = '\0';
        // Any cut - detail, name or level - is reported, never silently hidden.
        entry.truncated = truncated || length > kDetailMax ||
            strlen(levelText) > kLevelMax - 1 || strlen(eventText) > kEventMax - 1;
        entry.atMs = nowMs;
        entry.seq = ++sequence_;
        head_ = static_cast<uint8_t>((head_ + 1) % kCapacity);
        if (count_ < kCapacity) ++count_;
    }

    // printf-style variant for details that need numbers. The formatting buffer
    // is bounded by kDetailMax, so this cannot blow up a stack frame; when
    // vsnprintf reports that the text did not fit, the event carries the
    // truncation flag instead of pretending the detail is complete.
    void addf(uint32_t nowMs, const char* level, const char* event, const char* format, ...) {
        char buffer[kDetailMax + 1];
        buffer[0] = '\0';
        bool cut = false;
        if (format) {
            va_list args;
            va_start(args, format);
            const int written = vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            buffer[sizeof(buffer) - 1] = '\0';
            if (written < 0 || static_cast<size_t>(written) >= sizeof(buffer)) cut = true;
        }
        add(nowMs, level, event, buffer, cut);
    }

    uint32_t sequence() const { return sequence_; }
    uint8_t count() const { return count_; }
    bool empty() const { return count_ == 0; }
    const char* bootId() const { return bootId_; }

    // Oldest first, 0 .. count()-1.
    const Entry& at(uint8_t index) const {
        const uint8_t start = static_cast<uint8_t>((head_ + kCapacity - count_) % kCapacity);
        return entries_[static_cast<uint8_t>((start + index) % kCapacity)];
    }

    // Writes the JSON document for the current ring into `out` (NUL-terminated)
    // and returns its length, or 0 when nothing usable could be written.
    //
    // Escaping can expand one byte to six (\u00XX), so a buffer sized with
    // kJsonMax may not hold every event. Rather than returning an empty document,
    // the NEWEST events that fit are written and the number left out is reported
    // as "omitted": the reader is told what it did not get.
    size_t writeJson(char* out, size_t capacity, uint32_t nowMs) const {
        if (!out || capacity < 2) return 0;
        out[0] = '\0';
        // Reserve room for the document head/tail and the "omitted" field.
        const size_t budget = capacity > 200 ? capacity - 200 : 0;
        size_t used = 0;
        uint8_t first = count_;   // oldest event to include
        for (uint8_t i = count_; i > 0; --i) {
            const size_t cost = entryCost(at(static_cast<uint8_t>(i - 1))) + 1;
            if (used + cost > budget) break;
            used += cost;
            first = static_cast<uint8_t>(i - 1);
        }
        if (count_ && used == 0) {
            // Not even the newest event fits this buffer: refuse instead of
            // emitting something misleading.
            return 0;
        }
        const size_t omitted = first;
        Writer writer{out, capacity, 0, false};
        writer.text("{\"bootId\":\"");
        writer.escaped(bootId_);
        writer.text("\",\"uptimeMs\":");
        writer.number(nowMs);
        writer.text(",\"sequence\":");
        writer.number(sequence_);
        writer.text(",\"capacity\":");
        writer.number(kCapacity);
        writer.text(",\"count\":");
        writer.number(static_cast<unsigned long>(count_ - omitted));
        writer.text(",\"omitted\":");
        writer.number(static_cast<unsigned long>(omitted));
        writer.text(",\"events\":[");
        for (uint8_t i = first; i < count_; ++i) {
            const Entry& entry = at(i);
            if (i != first) writer.put(',');
            writer.text("{\"seq\":");
            writer.number(entry.seq);
            writer.text(",\"atMs\":");
            writer.number(entry.atMs);
            writer.text(",\"level\":\"");
            writer.escaped(entry.level);
            writer.text("\",\"event\":\"");
            writer.escaped(entry.event);
            writer.text("\",\"detail\":\"");
            writer.escaped(entry.detail);
            writer.text("\",\"truncated\":");
            writer.text(entry.truncated ? "true" : "false");
            writer.put('}');
        }
        writer.text("]}");
        if (writer.overflow) {
            out[0] = '\0';
            return 0;
        }
        return writer.length;
    }

private:
    // Length of one escaped string as JSON (worst case six characters per byte).
    static size_t escapedLength(const char* value) {
        if (!value) return 0;
        size_t length = 0;
        for (const char* p = value; *p != '\0'; ++p) {
            const unsigned char c = static_cast<unsigned char>(*p);
            if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t') length += 2;
            else if (c < 0x20) length += 6;
            else length += 1;
        }
        return length;
    }

    // Serialized size of one event, used to decide how many fit in the buffer.
    // The fixed part covers the field names, both 10-digit numbers, the quotes
    // and the truncated flag; it is rounded up on purpose (overestimating only
    // omits one event earlier, underestimating would overflow the buffer).
    static size_t entryCost(const Entry& entry) {
        // {"seq":N,"atMs":N,"level":"","event":"","detail":"","truncated":false}
        return 96 + escapedLength(entry.level) + escapedLength(entry.event) +
            escapedLength(entry.detail);
    }

    static void copyText(char* out, size_t capacity, const char* text) {
        if (!out || capacity == 0) return;
        size_t i = 0;
        if (text) {
            for (; text[i] != '\0' && i + 1 < capacity; ++i) out[i] = text[i];
        }
        out[i] = '\0';
    }

    // Minimal bounded JSON writer: no allocation, no printf for strings, control
    // characters escaped as \u00XX so the document is always valid JSON.
    struct Writer {
        char* out;
        size_t capacity;
        size_t length;
        bool overflow;

        void put(char c) {
            if (length + 1 >= capacity) {
                overflow = true;
                return;
            }
            out[length++] = c;
            out[length] = '\0';
        }
        void text(const char* value) {
            if (!value) return;
            for (const char* p = value; *p != '\0'; ++p) put(*p);
        }
        void escaped(const char* value) {
            if (!value) return;
            for (const char* p = value; *p != '\0'; ++p) {
                const unsigned char c = static_cast<unsigned char>(*p);
                switch (c) {
                    case '"': text("\\\""); break;
                    case '\\': text("\\\\"); break;
                    case '\n': text("\\n"); break;
                    case '\r': text("\\r"); break;
                    case '\t': text("\\t"); break;
                    default:
                        if (c < 0x20) {
                            char escape[7];
                            snprintf(escape, sizeof(escape), "\\u%04X", c);
                            text(escape);
                        } else {
                            put(static_cast<char>(c));  // UTF-8 bytes pass through
                        }
                        break;
                }
            }
        }
        void number(unsigned long value) {
            char digits[16];
            snprintf(digits, sizeof(digits), "%lu", value);
            text(digits);
        }
    };

    Entry entries_[kCapacity] = {};
    uint8_t head_ = 0;      // next slot to write
    uint8_t count_ = 0;     // events currently held
    uint32_t sequence_ = 0; // total events ever added (monotonic, never reset)
    char bootId_[kBootIdMax] = {};
};

}  // namespace motion
