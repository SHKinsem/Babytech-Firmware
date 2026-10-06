#pragma once

#include <stddef.h>
#include "version.h"

#ifdef __cplusplus
extern "C" {
#endif
#if MBEDTLS_VERSION_MAJOR == 2
int mbedtls_sha256_ret(const unsigned char* input, size_t length,
                       unsigned char output[32], int is224);
#elif MBEDTLS_VERSION_MAJOR == 3
int mbedtls_sha256(const unsigned char* input, size_t length,
                   unsigned char output[32], int is224);
#else
#error Unsupported fake mbedTLS major version
#endif
#ifdef __cplusplus
}
#endif
