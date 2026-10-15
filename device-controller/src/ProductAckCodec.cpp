#include "ProductAckCodec.h"

namespace motion {

void writeProductAck(JsonObject target, const ProductAckSnapshot& snapshot) {
    target["command_id"] = snapshot.commandId;
    target["command"] = snapshot.command;
    target["device_id"] = snapshot.deviceId;
    target["accepted"] = snapshot.accepted;
    target["status"] = snapshot.status;
    if (snapshot.reason.empty()) target["reason"] = nullptr;
    else target["reason"] = snapshot.reason;
    target["progress"] = snapshot.progress;
    if (snapshot.progress == "error") target["error_code"] = snapshot.errorCode;
}

}  // namespace motion
