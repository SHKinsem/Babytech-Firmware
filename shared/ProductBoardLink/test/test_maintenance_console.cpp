#include "MaintenanceConsole.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using babytech::boardlink::MaintenanceLineReader;
using babytech::boardlink::MaintenanceSession;
using Result = MaintenanceLineReader::Result;

namespace {
size_t checks = 0;
const char* group = "";
const uint8_t delimiters[] = {'\r', '\n'};

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        std::cerr << group << ':' << __LINE__ << ": " << #condition << '\n'; \
        std::exit(EXIT_FAILURE); \
    } \
} while (false)

void response(const char* actual, const char* expected) {
    CHECK(actual != nullptr);
    CHECK(std::string(actual) == expected);
}

// Dispatch only completed lines, just as a Serial owner must. Recording every
// line also detects leaked OTA CODE suffixes without implementing an OTA path.
struct Console {
    MaintenanceLineReader reader;
    MaintenanceSession session;
    std::vector<std::string> lines;
    std::vector<std::string> replies;
    bool safe = true;

    Result byte(uint8_t value, uint32_t now) {
        const Result result = reader.feed(value, now);
        if (result == Result::Line) {
            lines.emplace_back(reader.line());
            const char* reply = session.handle(reader.line(), safe);
            replies.emplace_back(reply ? reply : "<unknown>");
        } else if (result == Result::Rejected) {
            CHECK(reader.line()[0] == '\0');
        }
        return result;
    }

    void pending(const std::string& text, uint32_t now = 0) {
        for (char value : text)
            CHECK(byte(static_cast<uint8_t>(value), now) == Result::None);
    }

    void command(const std::string& text, const char* reply, uint32_t now = 0) {
        pending(text, now);
        CHECK(byte('\n', now) == Result::Line);
        CHECK(lines.back() == text);
        CHECK(replies.back() == reply);
    }
};

void exactCommands() {
    group = "exact commands";
    const char* commands[] = {"MAINT BEGIN", "MAINT STATUS", "MAINT END"};
    for (bool active : {false, true}) {
        MaintenanceSession session;
        if (active) response(session.handle("MAINT BEGIN", true), "active");
        CHECK(session.handle(nullptr, true) == nullptr);
        CHECK(session.active() == active);
        for (const char* command : commands) {
            const std::string text(command);
            std::vector<std::string> invalid = {
                " " + text, text + " ", text + " extra", "x" + text,
                text + "x", text + ";MAINT END", "MAINT  " + text.substr(6)
            };
            for (size_t length = 0; length < text.size(); ++length)
                invalid.push_back(text.substr(0, length));
            for (size_t index = 0; index < text.size(); ++index) {
                std::string changed = text;
                changed[index] = text[index] == ' ' ? '_' :
                    static_cast<char>(text[index] - 'A' + 'a');
                invalid.push_back(changed);
            }
            for (const auto& input : invalid) {
                for (bool safe : {false, true}) {
                    CHECK(session.handle(input.c_str(), safe) == nullptr);
                    CHECK(session.active() == active);
                }
            }
        }
        for (const char* input : {"OTA CODE", "OTA CODE 123456", "MAINT IMPORT",
                                  "MAINT", "{}", "BEGIN", "STATUS", "END"}) {
            CHECK(session.handle(input, true) == nullptr);
            CHECK(session.active() == active);
        }
    }
    Console console;
    console.command("MAINT STATUS", "inactive");
    console.command("MAINT BEGIN", "active");
    console.command("MAINT STATUS", "active");
    console.command("MAINT END", "inactive");
}

void lifecycle() {
    group = "session lifecycle";
    MaintenanceSession session;
    CHECK(!session.active());
    response(session.handle("MAINT END", false), "inactive");
    response(session.handle("MAINT BEGIN", false), "unsafe");
    CHECK(!session.active());
    response(session.handle("MAINT BEGIN", true), "active");
    for (unsigned iteration = 0; iteration < 32; ++iteration) {
        for (bool safe : {false, true}) {
            response(session.handle("MAINT STATUS", safe), "active");
            response(session.handle("MAINT BEGIN", safe), "active");
            CHECK(session.handle("MAINT IMPORT {}", safe) == nullptr);
            CHECK(session.handle(nullptr, safe) == nullptr);
            CHECK(session.active());
        }
        response(session.handle("MAINT END", false), "unsafe");
        CHECK(session.active());
    }
    response(session.handle("MAINT END", true), "inactive");
    for (bool safe : {false, true}) {
        response(session.handle("MAINT END", safe), "inactive");
        response(session.handle("MAINT STATUS", safe), "inactive");
        CHECK(!session.active());
    }
    response(session.handle("MAINT BEGIN", false), "unsafe");
    response(session.handle("MAINT BEGIN", true), "active");
    CHECK(session.active());
}

void delimitersAndCapacity() {
    group = "delimiters and capacity";
    static_assert(MaintenanceLineReader::kCapacity == 768, "console capacity contract");
    static_assert(MaintenanceLineReader::kPollBytes == 64, "Serial poll budget contract");
    static_assert(MaintenanceLineReader::kTimeoutMs == 2000, "timeout contract");
    Console console;
    for (char delimiter : std::string("\r\n\n\r\r\n"))
        CHECK(console.byte(static_cast<uint8_t>(delimiter), 0) == Result::None);
    CHECK(console.lines.empty());
    for (uint8_t delimiter : delimiters) {
        console.pending("MAINT STATUS");
        CHECK(console.byte(delimiter, 0) == Result::Line);
        CHECK(console.lines.back() == "MAINT STATUS");
        CHECK(console.byte('\n', 0) == Result::None);
        std::string printable;
        for (unsigned value = 0x20; value <= 0x7e; ++value)
            printable += static_cast<char>(value);
        printable.append(MaintenanceLineReader::kCapacity - 1 - printable.size(), 'x');
        CHECK(printable.size() == MaintenanceLineReader::kCapacity - 1);
        console.pending(printable);
        CHECK(console.byte(delimiter, 0) == Result::Line);
        CHECK(console.lines.back() == printable);
        CHECK(console.reader.line()[printable.size()] == '\0');
        CHECK(console.replies.back() == "<unknown>");
        console.command("MAINT STATUS", "inactive");
    }
}

void invalidBytes() {
    group = "invalid bytes";
    for (unsigned value = 0; value <= 0xff; ++value) {
        if ((value >= 0x20 && value <= 0x7e) || value == '\r' || value == '\n') continue;
        for (bool active : {false, true}) {
            const std::string command = active ? "MAINT END" : "MAINT BEGIN";
            for (size_t position : {size_t(0), size_t(5), command.size()}) {
                for (uint8_t delimiter : delimiters) {
                    Console console;
                    if (active) response(console.session.handle("MAINT BEGIN", true), "active");
                    console.pending(command.substr(0, position));
                    CHECK(console.byte(static_cast<uint8_t>(value), 0) == Result::None);
                    console.pending(command.substr(position));
                    CHECK(console.byte(delimiter, 0) == Result::Rejected);
                    CHECK(console.lines.empty());
                    CHECK(console.session.active() == active);
                    CHECK(console.byte('\n', 0) == Result::None);
                    console.command(command, active ? "inactive" : "active");
                    CHECK(console.session.active() != active);
                }
            }
        }
    }
}

void overflow() {
    group = "overflow drops entire line";
    for (size_t length : {MaintenanceLineReader::kCapacity,
                          MaintenanceLineReader::kCapacity + 1, size_t(4096)}) {
        for (const char* suffix : {"", "OTA CODE", "OTA CODE 123456",
                                   "MAINT BEGIN", "MAINT END", "MAINT STATUS"}) {
            for (bool active : {false, true}) {
                for (uint8_t delimiter : delimiters) {
                    Console console;
                    if (active) response(console.session.handle("MAINT BEGIN", true), "active");
                    console.pending(std::string(length, 'x'));
                    console.pending(suffix);
                    CHECK(console.byte(delimiter, 0) == Result::Rejected);
                    CHECK(console.lines.empty());
                    CHECK(console.session.active() == active);
                    console.command("OTA CODE 123456", "<unknown>");
                    console.command("MAINT STATUS", active ? "active" : "inactive");
                }
            }
        }
    }
}

void timeouts() {
    group = "timeout boundaries and uint32 wrap";
    const uint32_t timeout = MaintenanceLineReader::kTimeoutMs;
    for (uint32_t start : {uint32_t(0), uint32_t(12345),
                           std::numeric_limits<uint32_t>::max() - 1000}) {
        for (uint32_t gap : {timeout - 1, timeout, timeout + 1}) {
            const uint32_t later = start + gap;
            Console completed;
            completed.pending("MAINT BEGIN", start);
            CHECK(completed.byte('\n', later) ==
                  (gap < timeout ? Result::Line : Result::Rejected));
            CHECK(completed.session.active() == (gap < timeout));

            Console fragmented;
            fragmented.pending("MAINT ", start);
            fragmented.pending("BEGIN", later);
            CHECK(fragmented.byte('\r', later) ==
                  (gap < timeout ? Result::Line : Result::Rejected));
            CHECK(fragmented.session.active() == (gap < timeout));

            if (gap < timeout) continue;
            for (bool active : {false, true}) {
                for (const char* suffix : {"MAINT BEGIN", "MAINT END", "MAINT STATUS", "OTA CODE 123456"}) {
                    Console console;
                    if (active) response(console.session.handle("MAINT BEGIN", true), "active");
                    console.pending("x", start);
                    console.pending(suffix, later);
                    // Expiry is sticky until a delimiter, even after another gap.
                    console.pending(suffix, later + timeout);
                    CHECK(console.byte('\n', later + timeout) == Result::Rejected);
                    CHECK(console.lines.empty());
                    CHECK(console.session.active() == active);
                    console.command("MAINT STATUS", active ? "active" : "inactive", later + timeout);
                }
            }
        }
    }
    Console idle;
    idle.command("MAINT BEGIN", "active");
    CHECK(idle.byte('\n', 1000000) == Result::None);
    idle.command("MAINT STATUS", "active", 1000000);
    idle.pending("MAINT END", 1000000);
    CHECK(idle.session.active());
    CHECK(idle.byte('\n', 1000000 + timeout) == Result::Rejected);
    CHECK(idle.session.active());
    idle.command("MAINT END", "inactive", 1000000 + timeout);
}

void fragmentedFeeds() {
    group = "fragmented feeds and inter-byte timeout";
    const std::string stream = "\r\nMAINT BEGIN\r\nMAINT STATUS\nMAINT BEGIN\r"
                               "unknown\nMAINT STATUS\r\nMAINT END\n\nMAINT STATUS\n";
    const std::vector<std::string> expectedLines = {
        "MAINT BEGIN", "MAINT STATUS", "MAINT BEGIN", "unknown",
        "MAINT STATUS", "MAINT END", "MAINT STATUS"
    };
    const std::vector<std::string> expectedReplies = {
        "active", "active", "active", "<unknown>", "active", "inactive", "inactive"
    };
    for (size_t chunk = 1; chunk <= MaintenanceLineReader::kPollBytes; ++chunk) {
        Console console;
        uint32_t now = std::numeric_limits<uint32_t>::max() - 100;
        for (size_t offset = 0; offset < stream.size(); offset += chunk) {
            for (size_t index = offset; index < stream.size() && index < offset + chunk; ++index) {
                const Result result = console.byte(static_cast<uint8_t>(stream[index]), now);
                CHECK(result != Result::Rejected);
            }
            now += MaintenanceLineReader::kTimeoutMs - 1;
        }
        CHECK(console.lines == expectedLines);
        CHECK(console.replies == expectedReplies);
        CHECK(!console.session.active());
    }
}
}  // namespace

int main() {
    exactCommands();
    lifecycle();
    delimitersAndCapacity();
    invalidBytes();
    overflow();
    timeouts();
    fragmentedFeeds();
    std::cout << "Maintenance console: 7 groups, " << checks << " checks passed\n";
    return EXIT_SUCCESS;
}
