#pragma once

#include "BoardProtocolV4.h"

namespace babytech { namespace boardlink {

// Shared read-only capture used by USB diagnostics and UART commissioning.
class BoardExportSource {
public:
    virtual ~BoardExportSource() = default;
    virtual bool begin(v4::Role role, const char* device, const char* challenge,
                       uint64_t boot, uint32_t nowMs) = 0;
    virtual size_t remaining() const = 0;
    virtual size_t peek(uint8_t* output, size_t capacity) const = 0;
    virtual void consume(size_t length) = 0;
    virtual void cancel() = 0;
};

} }
