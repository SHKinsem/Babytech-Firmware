#pragma once

#include <cstddef>
#include <limits>
#if defined(__APPLE__)
#include <CommonCrypto/CommonHMAC.h>
#elif defined(__linux__)
#include <openssl/hmac.h>
#else
#error Motion main tests require a real system cryptography backend
#endif

enum mbedtls_md_type_t { MBEDTLS_MD_NONE = 0, MBEDTLS_MD_SHA256 = 6 };
struct mbedtls_md_info_t { mbedtls_md_type_t type; };

inline const mbedtls_md_info_t* mbedtls_md_info_from_type(mbedtls_md_type_t type) {
    static const mbedtls_md_info_t sha256{MBEDTLS_MD_SHA256};
    return type == MBEDTLS_MD_SHA256 ? &sha256 : nullptr;
}

inline int mbedtls_md_hmac(const mbedtls_md_info_t* info,
                            const unsigned char* key, size_t keySize,
                            const unsigned char* input, size_t size,
                            unsigned char* output) {
    if (!info || info->type != MBEDTLS_MD_SHA256 || !output ||
        (!key && keySize) || (!input && size)) return -1;
    static const unsigned char empty = 0;
    if (!key) key = &empty;
    if (!input) input = &empty;
#if defined(__APPLE__)
    CCHmac(kCCHmacAlgSHA256, key, keySize, input, size, output);
    return 0;
#else
    if (keySize > static_cast<size_t>(std::numeric_limits<int>::max())) return -1;
    unsigned length = 0;
    return HMAC(EVP_sha256(), key, static_cast<int>(keySize), input, size,
                output, &length) && length == 32 ? 0 : -1;
#endif
}

