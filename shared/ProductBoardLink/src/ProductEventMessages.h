#pragma once

#include "MotionStateRecord.h"
#include "BoardProtocolV4.h"

namespace babytech { namespace boardlink {

struct TerminalEvent {
    ProductRequest request;
    char eventId[59]{};
    float targetPowderG = 0;
    bool completed = false;
    uint32_t uptimeMs = 0;
    char reason[65]{}, errorCode[65]{};
};

struct CloudReceipt {
    char deviceId[65]{}, eventId[59]{};
};

// Existing event JSON, plus the already approved cloud-only command_seq.
// Local identity is recovered from the canonical event/local command IDs.
// These pure codecs do not authorize transport, persistence or result deletion.
// Outputs are unchanged on failure; successful Messages have zero envelope IDs.
bool terminalEventFromSlot(const v4::Pairing& pairing, const MotionExecutionSlot& slot,
                           TerminalEvent& output);
bool encodeTerminalEvent(const v4::Pairing& pairing, const TerminalEvent& event,
                         v4::Message& output);
bool decodeTerminalEvent(const v4::Message& message, const v4::Pairing& pairing,
                         TerminalEvent& output);
bool encodeCloudReceipt(const CloudReceipt& receipt, v4::Message& output);
bool decodeCloudReceipt(const uint8_t* bytes, size_t length, const char* expectedDeviceId,
                        CloudReceipt& output);
bool decodeCloudReceipt(const v4::Message& message, const char* expectedDeviceId,
                        CloudReceipt& output);

} }
