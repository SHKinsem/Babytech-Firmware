#include "FakeProductCrypto.h"
#include "mbedtls/sha256.h"

#include <cstring>
#include <limits>
#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#elif defined(__linux__)
#include <openssl/sha.h>
#else
#error A system SHA-256 backend is required
#endif

namespace fake_product_crypto {
bool fail = false;
unsigned calls = 0;
void reset() { fail = false; calls = 0; }
}

#if MBEDTLS_VERSION_MAJOR == 2
extern "C" int mbedtls_sha256_ret(const unsigned char* input, size_t length,
                                 unsigned char output[32], int is224) {
#else
extern "C" int mbedtls_sha256(const unsigned char* input, size_t length,
                             unsigned char output[32], int is224) {
#endif
    ++fake_product_crypto::calls;
    if (fake_product_crypto::fail || is224 || !output || (!input && length)) return -1;
    unsigned char digest[32];
#if defined(__APPLE__)
    if (length > std::numeric_limits<CC_LONG>::max()) return -1;
    if (!CC_SHA256(input, static_cast<CC_LONG>(length), digest)) return -1;
#else
    if (!SHA256(input, length, digest)) return -1;
#endif
    std::memcpy(output, digest, sizeof(digest));
    return 0;
}
