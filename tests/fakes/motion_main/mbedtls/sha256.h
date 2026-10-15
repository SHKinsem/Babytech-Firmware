#pragma once

// Host SDK adapter, not a digest stub. Apple uses libSystem; Linux needs -lcrypto.
#include <cstddef>
#include <cstdint>
#include <limits>
#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#elif defined(__linux__)
#include <openssl/evp.h>
#else
#error Motion main tests require a real system cryptography backend
#endif

struct mbedtls_sha256_context {
#if defined(__APPLE__)
    CC_SHA256_CTX native{};
#else
    EVP_MD_CTX* native = nullptr;
#endif
    bool active = false;
};

inline void mbedtls_sha256_init(mbedtls_sha256_context* ctx) {
    if (ctx) *ctx = mbedtls_sha256_context{};
}

inline void mbedtls_sha256_free(mbedtls_sha256_context* ctx) {
    if (!ctx) return;
#if defined(__linux__)
    EVP_MD_CTX_free(ctx->native);
#endif
    *ctx = mbedtls_sha256_context{};
}

inline int mbedtls_sha256_starts_ret(mbedtls_sha256_context* ctx, int is224) {
    if (!ctx) return -1;
    ctx->active = false;
    if (is224) return -1;
#if defined(__APPLE__)
    ctx->active = CC_SHA256_Init(&ctx->native) == 1;
#else
    if (!ctx->native) ctx->native = EVP_MD_CTX_new();
    ctx->active = ctx->native && EVP_DigestInit_ex(ctx->native, EVP_sha256(), nullptr) == 1;
#endif
    return ctx->active ? 0 : -1;
}

inline int mbedtls_sha256_update_ret(mbedtls_sha256_context* ctx,
                                      const unsigned char* input, size_t size) {
    if (!ctx || !ctx->active || (!input && size)) return -1;
    if (!size) return 0;
#if defined(__APPLE__)
    while (size) {
        const size_t chunk = size > std::numeric_limits<CC_LONG>::max()
            ? std::numeric_limits<CC_LONG>::max() : size;
        if (CC_SHA256_Update(&ctx->native, input, static_cast<CC_LONG>(chunk)) != 1) {
            ctx->active = false;
            return -1;
        }
        input += chunk;
        size -= chunk;
    }
#else
    if (EVP_DigestUpdate(ctx->native, input, size) != 1) {
        ctx->active = false;
        return -1;
    }
#endif
    return 0;
}

inline int mbedtls_sha256_finish_ret(mbedtls_sha256_context* ctx,
                                      unsigned char output[32]) {
    if (!ctx || !ctx->active || !output) return -1;
    ctx->active = false;
#if defined(__APPLE__)
    return CC_SHA256_Final(output, &ctx->native) == 1 ? 0 : -1;
#else
    unsigned size = 0;
    return EVP_DigestFinal_ex(ctx->native, output, &size) == 1 && size == 32 ? 0 : -1;
#endif
}

inline int mbedtls_sha256_ret(const unsigned char* input, size_t size,
                                unsigned char output[32], int is224) {
    if ((!input && size) || !output || is224) return -1;
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    const bool ok = mbedtls_sha256_starts_ret(&ctx, 0) == 0 &&
        mbedtls_sha256_update_ret(&ctx, input, size) == 0 &&
        mbedtls_sha256_finish_ret(&ctx, output) == 0;
    mbedtls_sha256_free(&ctx);
    return ok ? 0 : -1;
}

