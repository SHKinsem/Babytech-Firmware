#include "ProductEventCodec.h"

#include <cassert>
#include <cstdio>
#include <cstring>

void verifyEvent(bool completed) {
    motion::ProductTerminal terminal;
    terminal.completed = completed;
    terminal.run.commandId = "cmd-1";
    terminal.run.source = "cloud_command";
    terminal.run.babyId = "baby-1";
    terminal.run.profileVersion = 3;
    terminal.run.recipe = {180, 45, 25.0f};
    terminal.run.targetPowderG = 45.0f;
    if (!completed) {
        terminal.reason = "stopped";
        terminal.errorCode = "E_STOPPED";
    }

    DynamicJsonDocument document(1024);
    motion::writeProductTerminalEvent(document.to<JsonObject>(), terminal,
                                      "bt-184DCE6E27AC", "event-1", 12345);
    assert(!document.overflowed());
    char payload[1024];
    const size_t length = serializeJson(document, payload, sizeof(payload));
    assert(length > 0 && length < sizeof(payload));
    DynamicJsonDocument parsed(1024);
    assert(!deserializeJson(parsed, payload, length));
    assert(std::strcmp(parsed["event_id"], "event-1") == 0);
    assert(std::strcmp(parsed["event"], completed ? "feeding_completed" : "feeding_failed") == 0);
    assert(std::strcmp(parsed["device_id"], "bt-184DCE6E27AC") == 0);
    assert(std::strcmp(parsed["command_id"], "cmd-1") == 0);
    assert(std::strcmp(parsed["source"], "cloud_command") == 0);
    assert(std::strcmp(parsed["baby_id"], "baby-1") == 0);
    assert(parsed["feeding_context_profile_version"] == 3);
    assert(parsed["water_ml"] == 180);
    assert(parsed["temp"] == 45);
    assert(parsed["powder_g_per_100ml"] == 25.0f);
    assert(parsed["target_powder_g"] == 45.0f);
    assert(std::strcmp(parsed["water_delivery_basis"], "estimated_turns") == 0);
    assert(parsed["dispensed_water_ml"].isNull());
    assert(parsed["uptime_ms"] == 12345);
    if (completed) {
        assert(parsed["reason"].isNull());
        assert(parsed["error_code"].isNull());
    } else {
        assert(std::strcmp(parsed["reason"], "stopped") == 0);
        assert(std::strcmp(parsed["error_code"], "E_STOPPED") == 0);
    }
}

int main() {
    verifyEvent(true);
    verifyEvent(false);

    motion::ProductTerminal longest;
    longest.run.commandId.assign(128, 'c');
    longest.run.source = "cloud_command";
    longest.run.babyId.assign(96, 'b');
    longest.run.recipe = {500, 60, 50.0f};
    longest.run.targetPowderG = 250.0f;
    longest.reason.assign(64, 'r');
    longest.errorCode.assign(64, 'e');
    char eventId[64];
    std::memset(eventId, 'x', sizeof(eventId) - 1);
    eventId[sizeof(eventId) - 1] = '\0';
    DynamicJsonDocument large(1024);
    motion::writeProductTerminalEvent(large.to<JsonObject>(), longest,
                                      "bt-184DCE6E27AC", eventId, 0);
    assert(!large.overflowed());
    assert(measureJson(large) < 1536);
    std::puts("PASS product terminal event JSON codec");
}
