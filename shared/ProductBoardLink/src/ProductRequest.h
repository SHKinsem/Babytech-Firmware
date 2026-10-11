#pragma once

#include "BoardSessionV4.h"
#include "ProductContext.h"

namespace babytech { namespace boardlink {

// Stop is deliberately outside the ordinary durable request sequence.
enum class ProductCommand : uint8_t {
    None = 0, Initialize = 1, Prepare = 2, Clean = 3, SetTargetTemp = 4,
    ResetError = 5, CheckFirmwareUpdate = 6
};

struct ProductRequest {
    v4::Source source = v4::Source::LocalTouch;
    ProductCommand command = ProductCommand::None;
    uint64_t sequence = 0;
    char deviceId[65] = {};
    char commandId[129] = {};
    char babyId[97] = {};
    uint32_t profileVersion = 0;
    uint16_t waterMl = 0;
    uint8_t temperatureC = 0;
    float powderGPer100Ml = 0;
};

constexpr size_t kRequestIdentityMaxSize = 316;
constexpr size_t kProductDigestSize = 32;
bool validProductIdentity(const char (&deviceId)[65], const char (&commandId)[129]);
bool validProductRequest(const ProductRequest& request);
size_t encodeRequestIdentity(const ProductRequest& request, uint8_t* output, size_t capacity);
bool decodeRequestIdentity(const uint8_t* bytes, size_t length, ProductRequest& output);
bool sameProductRequest(const ProductRequest& left, const ProductRequest& right);
bool requestDigest(const ProductRequest& request, uint8_t (&output)[kProductDigestSize]);
bool contextDigest(const ProductContext& context, uint8_t (&output)[kProductDigestSize]);
// Requires a validated Brain pairing and nonzero ordinary sequence. Atomic on failure.
bool makeLocalCommandId(const v4::Pairing& pairing, uint64_t sequence, char (&output)[129]);

} }
