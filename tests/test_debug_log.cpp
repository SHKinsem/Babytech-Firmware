// Host tests for the RAM-only diagnostic log (device-controller/src/DebugLog.h).
//
// The logger is header-only and Arduino-free exactly so these properties can be
// checked without hardware: chronological order, the fixed ring bound, explicit
// truncation, correct JSON escaping, boot identity, and the guarantee that the
// JSON buffer main.cpp sizes with kJsonMax is always sufficient.
//
// Tiny hand-rolled harness on purpose: no external test framework.

#include "DebugLog.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace motion;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::printf("    FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

static std::string jsonOf(const DebugLog& log, size_t capacity = DebugLog::kJsonMax) {
    std::string buffer(capacity, '\0');
    const size_t length = log.writeJson(&buffer[0], buffer.size(), 1234);
    if (length == 0) return std::string();
    buffer.resize(length);
    return buffer;
}

static bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

static void test_empty_log_and_boot_identity() {
    DebugLog log;
    log.begin("ABCD1234");
    CHECK(log.count() == 0);
    CHECK(log.sequence() == 0);
    CHECK(std::string(log.bootId()) == "ABCD1234");

    const std::string json = jsonOf(log);
    CHECK(contains(json, "\"bootId\":\"ABCD1234\""));
    CHECK(contains(json, "\"uptimeMs\":1234"));
    CHECK(contains(json, "\"sequence\":0"));
    CHECK(contains(json, "\"capacity\":48"));
    CHECK(contains(json, "\"count\":0"));
    CHECK(contains(json, "\"events\":[]"));

    // A new boot starts a new session: no event, sequence or id survives it.
    log.add(10, "info", "boot", "first");
    log.begin("FFFF0000");
    CHECK(log.count() == 0);
    CHECK(log.sequence() == 0);
    CHECK(std::string(log.bootId()) == "FFFF0000");
    CHECK(!contains(jsonOf(log), "first"));
}

static void test_order_and_sequence() {
    DebugLog log;
    log.begin("B1");
    log.add(100, "info", "boot", "reset=1");
    log.add(200, "warn", "http.result", "/api/command code=409 error=not_enabled");
    log.addf(300, "info", "queue.state", "run=%lu state=%u", 7UL, 1U);

    CHECK(log.count() == 3);
    CHECK(log.sequence() == 3);
    CHECK(log.at(0).seq == 1);   // oldest first
    CHECK(log.at(0).atMs == 100);
    CHECK(log.at(2).seq == 3);
    CHECK(std::string(log.at(2).detail) == "run=7 state=1");

    const std::string json = jsonOf(log);
    const size_t first = json.find("\"seq\":1");
    const size_t second = json.find("\"seq\":2");
    const size_t third = json.find("\"seq\":3");
    CHECK(first != std::string::npos && second != std::string::npos && third != std::string::npos);
    CHECK(first < second && second < third);  // chronological in the document
    CHECK(contains(json, "\"level\":\"warn\""));
    CHECK(contains(json, "\"truncated\":false"));
}

static void test_ring_bound_keeps_newest() {
    DebugLog log;
    log.begin("B2");
    char detail[32];
    for (int i = 1; i <= 60; ++i) {
        std::snprintf(detail, sizeof(detail), "event-%d", i);
        log.add(static_cast<uint32_t>(i), "info", "poll", detail);
    }
    // The sequence keeps counting, the ring only holds the newest entries.
    CHECK(log.sequence() == 60);
    CHECK(log.count() == DebugLog::kCapacity);
    CHECK(std::string(log.at(0).detail) == "event-13");
    CHECK(std::string(log.at(DebugLog::kCapacity - 1).detail) == "event-60");
    const std::string json = jsonOf(log);
    CHECK(!contains(json, "event-12"));
    CHECK(contains(json, "event-60"));
    CHECK(contains(json, "\"count\":48"));
    CHECK(contains(json, "\"sequence\":60"));
}

static void test_detail_truncation_is_explicit() {
    DebugLog log;
    log.begin("B3");
    std::string longDetail(DebugLog::kDetailMax + 90, 'x');
    longDetail += "TAIL";
    log.add(1, "info", "big", longDetail.c_str());
    log.add(2, "info", "small", "ok");

    CHECK(std::strlen(log.at(0).detail) == DebugLog::kDetailMax);
    CHECK(log.at(0).truncated);
    CHECK(!log.at(1).truncated);
    CHECK(!contains(jsonOf(log), "TAIL"));
    CHECK(contains(jsonOf(log), "\"truncated\":true"));
    // Names are bounded too, and never left unterminated.
    log.add(3, "info", "an-event-name-that-is-far-too-long", nullptr);
    CHECK(std::strlen(log.at(2).event) == DebugLog::kEventMax - 1);
    CHECK(std::strlen(log.at(2).detail) == 0);
}

static void test_json_escaping() {
    DebugLog log;
    log.begin("B4");
    const char detail[] = "quote=\" backslash=\\ newline=\n tab=\t ctrl=\001 end";
    log.add(5, "warn", "escape", detail);
    const std::string json = jsonOf(log);

    CHECK(contains(json, "quote=\\\""));
    CHECK(contains(json, "backslash=\\\\"));
    CHECK(contains(json, "newline=\\n"));
    CHECK(contains(json, "tab=\\t"));
    CHECK(contains(json, "ctrl=\\u0001"));
    CHECK(!contains(json, "ctrl=\001"));
    // No raw control byte may survive inside the document.
    bool rawControl = false;
    for (const char c : json) {
        if (static_cast<unsigned char>(c) < 0x20) rawControl = true;
    }
    CHECK(!rawControl);
    // Quotes in a field name are escaped as well.
    log.add(6, "info", "ev\"ent", nullptr);
    CHECK(contains(jsonOf(log), "ev\\\"ent"));
}

static void test_json_buffer_bounds() {
    DebugLog log;
    log.begin("B5");
    // A buffer that cannot hold even one event must fail cleanly instead of
    // emitting half a document the caller might send.
    char small[8];
    std::memset(small, 'x', sizeof(small));
    CHECK(log.writeJson(small, sizeof(small), 1) == 0);
    CHECK(small[0] == '\0');
    CHECK(log.writeJson(nullptr, 64, 1) == 0);

    // A full ring of PLAIN maximum-length details fits the buffer the firmware
    // sizes with kJsonMax: nothing is omitted and the document is complete.
    std::string detail(DebugLog::kDetailMax, 'y');
    for (uint8_t i = 0; i < DebugLog::kCapacity; ++i) {
        log.add(i, "error", "worst-case-event-name", detail.c_str());
    }
    CHECK(log.count() == DebugLog::kCapacity);
    const std::string json = jsonOf(log, DebugLog::kJsonMax);
    CHECK(!json.empty());
    CHECK(json.size() < DebugLog::kJsonMax);
    CHECK(json.rfind("]}") == json.size() - 2);
    CHECK(contains(json, "\"omitted\":0"));
    CHECK(contains(json, "\"count\":48"));
}

// Escaping expands one byte to six characters, so the same ring CAN exceed the
// buffer. The document must stay valid and say how many events it left out -
// never come back empty.
static void test_escaped_ring_reports_omitted() {
    DebugLog log;
    log.begin("B6");
    std::string evil;
    for (size_t i = 0; i < DebugLog::kDetailMax; ++i) evil.push_back(static_cast<char>(i % 31 + 1));
    for (uint8_t i = 0; i < DebugLog::kCapacity; ++i) {
        log.addf(i, "info", "escaped", "%s", evil.c_str());
    }
    const std::string json = jsonOf(log, DebugLog::kJsonMax);
    CHECK(!json.empty());
    CHECK(json.size() < DebugLog::kJsonMax);
    CHECK(json.rfind("]}") == json.size() - 2);
    CHECK(contains(json, "\\u0001"));
    // The newest events are the ones kept, and the omitted count is explicit.
    CHECK(contains(json, "\"omitted\":"));
    const size_t omittedAt = json.find("\"omitted\":");
    const int omitted = std::atoi(json.c_str() + omittedAt + 10);
    CHECK(omitted > 0);
    CHECK(omitted < DebugLog::kCapacity);
    const std::string countNeedle = "\"count\":" + std::to_string(DebugLog::kCapacity - omitted);
    CHECK(contains(json, countNeedle));
    // The newest event is always in the document.
    CHECK(contains(json, "\"truncated\":false"));
}

// A detail that does not fit the formatting buffer is flagged, not silently
// presented as complete.
static void test_addf_flags_its_own_truncation() {
    DebugLog log;
    log.begin("B7");
    std::string longText(DebugLog::kDetailMax + 120, 'z');
    log.addf(1, "info", "addf", "text=%s", longText.c_str());
    CHECK(log.at(0).truncated);
    CHECK(std::strlen(log.at(0).detail) == DebugLog::kDetailMax);

    log.addf(2, "info", "addf", "small=%d", 7);
    CHECK(!log.at(1).truncated);
    CHECK(std::string(log.at(1).detail) == "small=7");

    // A cut name or level is flagged as well.
    log.add(3, "info", "an-event-name-that-is-far-too-long", "ok");
    CHECK(log.at(2).truncated);
    log.add(4, "a-very-long-level-name", "ok", "detail");
    CHECK(log.at(3).truncated);
}

int main() {
    test_empty_log_and_boot_identity();
    std::printf("%s  empty log, boot identity, session reset\n", g_failures == 0 ? "PASS" : "FAIL");
    const int afterEmpty = g_failures;
    test_order_and_sequence();
    std::printf("%s  chronological order and monotonic sequence\n", g_failures == afterEmpty ? "PASS" : "FAIL");
    const int afterOrder = g_failures;
    test_ring_bound_keeps_newest();
    std::printf("%s  fixed ring keeps the newest entries\n", g_failures == afterOrder ? "PASS" : "FAIL");
    const int afterRing = g_failures;
    test_detail_truncation_is_explicit();
    std::printf("%s  detail truncation flagged, names bounded\n", g_failures == afterRing ? "PASS" : "FAIL");
    const int afterTrunc = g_failures;
    test_json_escaping();
    std::printf("%s  JSON escaping of quotes, backslashes and control bytes\n", g_failures == afterTrunc ? "PASS" : "FAIL");
    const int afterEscape = g_failures;
    test_json_buffer_bounds();
    std::printf("%s  bounded JSON writer and kJsonMax sufficiency\n", g_failures == afterEscape ? "PASS" : "FAIL");
    const int afterBounds = g_failures;
    test_escaped_ring_reports_omitted();
    std::printf("%s  escaped worst case stays valid and reports omitted\n", g_failures == afterBounds ? "PASS" : "FAIL");
    const int afterEscaped = g_failures;
    test_addf_flags_its_own_truncation();
    std::printf("%s  addf flags its own truncation\n", g_failures == afterEscaped ? "PASS" : "FAIL");
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
