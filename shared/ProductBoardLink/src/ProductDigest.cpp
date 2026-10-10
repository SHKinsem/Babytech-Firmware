#include "ProductRequest.h"

#include <mbedtls/sha256.h>
#include <mbedtls/version.h>
#include <cstring>

namespace babytech { namespace boardlink {
namespace {
bool digest(const uint8_t* bytes, size_t length, uint8_t (&output)[kProductDigestSize]) {
    uint8_t result[kProductDigestSize];
#if MBEDTLS_VERSION_MAJOR >= 3
    const int error = mbedtls_sha256(bytes, length, result, 0);
#else
    const int error = mbedtls_sha256_ret(bytes, length, result, 0);
#endif
    if (error) return false;
    std::memcpy(output, result, sizeof(result));
    return true;
}
}

bool requestDigest(const ProductRequest& request, uint8_t (&output)[kProductDigestSize]) {
    uint8_t bytes[kRequestIdentityMaxSize];
    const size_t length = encodeRequestIdentity(request, bytes, sizeof(bytes));
    return length && digest(bytes, length, output);
}

bool contextDigest(const ProductContext& context, uint8_t (&output)[kProductDigestSize]) {
    uint8_t bytes[kContextIdentityMaxSize];
    const size_t length = encodeContextIdentity(context, bytes, sizeof(bytes));
    return length && digest(bytes, length, output);
}

} }
