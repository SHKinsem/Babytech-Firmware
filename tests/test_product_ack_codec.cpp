#include "ProductAckCodec.h"

#include <cassert>
#include <cstdio>
#include <cstring>

void verify(const motion::ProductAckSnapshot& snapshot) {
    DynamicJsonDocument document(768);
    motion::writeProductAck(document.to<JsonObject>(), snapshot);
    assert(!document.overflowed());
    char payload[1536];
    const size_t length = serializeJson(document, payload, sizeof(payload));
    assert(length > 0 && length < sizeof(payload));
    DynamicJsonDocument parsed(768);
    assert(!deserializeJson(parsed, payload, length));
    assert(std::strcmp(parsed["command_id"], snapshot.commandId.c_str()) == 0);
    assert(std::strcmp(parsed["command"], snapshot.command.c_str()) == 0);
    assert(std::strcmp(parsed["device_id"], snapshot.deviceId.c_str()) == 0);
    assert(parsed["accepted"] == snapshot.accepted);
    assert(std::strcmp(parsed["status"], snapshot.status.c_str()) == 0);
    assert(std::strcmp(parsed["progress"], snapshot.progress.c_str()) == 0);
    if (snapshot.reason.empty()) assert(parsed["reason"].isNull());
    else assert(std::strcmp(parsed["reason"], snapshot.reason.c_str()) == 0);
    if (snapshot.progress == "error")
        assert(std::strcmp(parsed["error_code"], snapshot.errorCode.c_str()) == 0);
    else assert(parsed["error_code"].isNull());
}

int main() {
    motion::ProductAckSnapshot accepted;
    accepted.commandId = "cmd-1";
    accepted.command = "prepare";
    accepted.deviceId = "bt-184DCE6E27AC";
    accepted.accepted = true;
    accepted.status = "accepted";
    accepted.progress = "unscrewing_cap";
    verify(accepted);

    accepted.commandId.assign(128, 'c');
    accepted.command.assign(40, 'a');
    accepted.accepted = false;
    accepted.status = "rejected";
    accepted.reason.assign(80, 'r');
    accepted.progress = "error";
    accepted.errorCode.assign(80, 'e');
    verify(accepted);
    std::puts("PASS product ACK JSON codec and MQTT payload bound");
}
