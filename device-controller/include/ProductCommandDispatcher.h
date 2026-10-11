#pragma once

#include <ArduinoJson.h>
#include <cstdint>

#include "ProductSession.h"

namespace motion {

struct ProductCommandOutcome {
    bool accepted = false;
    const char* status = "rejected";
    const char* reason = "not_ready";
};

ProductCommandOutcome executeProductCommand(JsonVariantConst command, ProductSession& product,
                                           bool otaActive, uint32_t now);

}  // namespace motion
