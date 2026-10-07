#pragma once

#include "ProductRequest.h"
#include "BoardProtocolV4.h"

namespace babytech { namespace boardlink {

enum class ContextStatus { Stored, Unchanged, Busy, Conflict, StorageFault };

// A matched stored/unchanged reply confirms the persistent version/content
// barrier, not mechanical readiness or completion of a feeding operation.
struct ContextResult {
    uint32_t replyTo = 0;
    char deviceId[65]{};
    uint32_t profileVersion = 0;
    bool cleared = false;
    uint8_t digest[kProductDigestSize]{};
    ContextStatus status = ContextStatus::StorageFault;
};

bool encodeContextMessage(const ProductContext& context, v4::Message& output);
bool decodeContextMessage(const v4::Message& message, const char* expectedDeviceId,
                          ProductContext& output);
bool encodeContextResult(const ContextResult& result, v4::Message& output);
bool decodeContextResult(const v4::Message& message, ContextResult& output);
bool matchesContextResult(const ContextResult& result, const ProductContext& context);

} }
