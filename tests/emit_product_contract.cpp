#include "ProductAckCodec.h"
#include "ProductEventCodec.h"
#include "ProductStatusCodec.h"

#include <ArduinoJson.h>
#include <cstdio>
#include <string>

int main() {
    constexpr char deviceId[] = "bt-184DCE6E27AC";
    DynamicJsonDocument document(4096);
    JsonObject root = document.to<JsonObject>();

    motion::ProductStatusSnapshot status;
    status.deviceId = deviceId;
    status.firmwareVersion = "motion-v1-test";
    motion::writeProductStatus(root.createNestedObject("status"), status);

    motion::ProductAckSnapshot ack;
    ack.commandId = "fixture-command-1";
    ack.command = "prepare";
    ack.deviceId = deviceId;
    ack.accepted = true;
    ack.status = "accepted";
    ack.progress = "unscrewing_cap";
    motion::writeProductAck(root.createNestedObject("ack"), ack);

    motion::ProductTerminal completed;
    completed.completed = true;
    completed.run.commandId = "fixture-command-1";
    completed.run.source = "cloud_command";
    completed.run.babyId = "fixture-baby-1";
    completed.run.profileVersion = 2;
    completed.run.recipe = {180, 45, 25.0f};
    completed.run.targetPowderG = 45.0f;
    motion::writeProductTerminalEvent(root.createNestedObject("feeding_completed"),
                                      completed, deviceId, "fixture-motion-event-1", 12345);

    motion::ProductTerminal failed = completed;
    failed.completed = false;
    failed.run.commandId = "fixture-command-2";
    failed.reason = "stopped";
    failed.errorCode = "E_STOPPED";
    motion::writeProductTerminalEvent(root.createNestedObject("feeding_failed"),
                                      failed, deviceId, "fixture-motion-event-2", 23456);

    motion::ProductTerminal local = completed;
    local.run.commandId = "local-touch-1";
    local.run.source = "local_touch";
    motion::writeProductTerminalEvent(root.createNestedObject("feeding_local_completed"),
                                      local, deviceId, "fixture-motion-event-3", 34567);

    if (document.overflowed()) return 1;
    std::string payload;
    serializeJson(document, payload);
    std::puts(payload.c_str());
}
