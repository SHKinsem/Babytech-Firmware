#pragma once

#include "ProductContext.h"

namespace babytech { namespace boardlink {

enum class LegacyContextLoad { Ready, Missing, Corrupt, IoError };

// Read-only inspection of the OLD productctx/payload NVS string. Missing never
// authorizes fresh-device initialization; Ready does not authorize migration or
// Start. SDK errors (including wrong NVS type) are IoError; identity/codec
// rejection is Corrupt. Every failure leaves output unchanged.
LegacyContextLoad loadLegacyProductContext(const char* expectedDeviceId,
                                          ProductContext& output);

} }
