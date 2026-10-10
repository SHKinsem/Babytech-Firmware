#pragma once

#include <ArduinoJson.h>
#include "ProductSession.h"

namespace motion {

bool decodeFeedingContext(JsonVariantConst source, FeedingContext& context);
bool decodeFeedingContextClear(JsonVariantConst source, uint32_t& profileVersion);

}  // namespace motion
