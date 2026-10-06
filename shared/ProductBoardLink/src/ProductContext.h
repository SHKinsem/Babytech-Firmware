#pragma once

#include <cstddef>
#include <cstdint>

namespace babytech { namespace boardlink {

// Full product data, never the shortened UI labels. An empty cache is not a
// tombstone: callers must retain the load result separately.
struct ProductContext {
    char deviceId[65] = {};
    uint32_t profileVersion = 0;
    bool cleared = false;
    char babyId[97] = {};
    char babyName[321] = {};
    char formulaBrand[481] = {};
    uint16_t waterMl = 0;
    uint8_t temperatureC = 0;
    float powderGPer100Ml = 0;
};

// Accepts the existing Cloud feeding_context payload (including optional
// updated_at). Strict JSON, UTF-8, identity/range and 2047-byte wire checks.
// Failed decoding leaves output unchanged. Does not authorize Start or write NVS.
bool decodeProductContext(const uint8_t* bytes, size_t length,
                          const char* expectedDeviceId, ProductContext& output);

// Canonical semantic bytes for equality and subsequent SHA-256 barriers; not a
// Flash record or authentication proof. Ignores JSON order/spacing/updated_at.
// Schema byte 1, cleared byte, length-prefixed (u16 LE) device ID, version u32 LE;
// an active context adds baby ID/name/brand, water u16 LE, temp u8, ratio f32 LE.
constexpr size_t kContextIdentityMaxSize = 981;
bool validProductContext(const ProductContext& context);
size_t encodeContextIdentity(const ProductContext& context, uint8_t* output, size_t capacity);
bool decodeContextIdentity(const uint8_t* bytes, size_t length, ProductContext& output);
bool sameProductContext(const ProductContext& left, const ProductContext& right);

} }
