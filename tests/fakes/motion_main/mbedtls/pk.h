#pragma once

// Only the PEM/SPKI P-256 and SHA-256 DER signature APIs used by WifiOta.
// Never substitute an always-successful signature verifier.
#include "md.h"
#include <climits>
#include <cstring>
#include <string>
#include <vector>
#if defined(__APPLE__)
#include <Security/Security.h>
#elif defined(__linux__)
#include <openssl/pem.h>
#else
#error Motion main tests require a real system signature backend
#endif

struct mbedtls_pk_context {
#if defined(__APPLE__)
    SecKeyRef native = nullptr;
#else
    EVP_PKEY* native = nullptr;
#endif
};

inline void mbedtls_pk_init(mbedtls_pk_context* ctx) {
    if (ctx) ctx->native = nullptr;
}

inline void mbedtls_pk_free(mbedtls_pk_context* ctx) {
    if (!ctx) return;
#if defined(__APPLE__)
    if (ctx->native) CFRelease(ctx->native);
#else
    EVP_PKEY_free(ctx->native);
#endif
    ctx->native = nullptr;
}

namespace motion_main_crypto {
// Reject malformed PEM, noncanonical base64 and anything except P-256 SPKI.
inline bool p256Point(const unsigned char* pem, size_t size,
                         std::vector<unsigned char>& point) {
    if (!pem || !size) return false;
    if (pem[size - 1] == 0) --size;
    const std::string text(reinterpret_cast<const char*>(pem), size);
    const std::string begin = "-----BEGIN PUBLIC KEY-----";
    const std::string end = "-----END PUBLIC KEY-----";
    if (text.compare(0, begin.size(), begin) != 0) return false;
    const size_t endAt = text.find(end, begin.size());
    if (endAt == std::string::npos) return false;
    for (size_t i = endAt + end.size(); i < text.size(); ++i)
        if (text[i] != '\r' && text[i] != '\n') return false;
    std::string encoded;
    for (size_t i = begin.size(); i < endAt; ++i) {
        if (text[i] != '\r' && text[i] != '\n') encoded += text[i];
    }
    if (encoded.empty() || encoded.size() % 4) return false;
    const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> der;
    for (size_t i = 0; i < encoded.size(); i += 4) {
        unsigned values[4]{};
        unsigned padding = 0;
        for (size_t j = 0; j < 4; ++j) {
            if (encoded[i + j] == '=') {
                if (j < 2 || i + 4 != encoded.size()) return false;
                ++padding;
            } else {
                const char c = encoded[i + j];
                const char* found = c ? std::strchr(alphabet, c) : nullptr;
                if (!found || padding) return false;
                values[j] = static_cast<unsigned>(found - alphabet);
            }
        }
        if (padding > 2 || (padding == 2 && (values[1] & 15)) ||
            (padding == 1 && (values[2] & 3))) return false;
        der.push_back(static_cast<unsigned char>((values[0] << 2) | (values[1] >> 4)));
        if (padding < 2)
            der.push_back(static_cast<unsigned char>((values[1] << 4) | (values[2] >> 2)));
        if (!padding)
            der.push_back(static_cast<unsigned char>((values[2] << 6) | values[3]));
    }
    static const unsigned char header[] = {
        0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01,
        0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00
    };
    if (der.size() != sizeof(header) + 65 ||
        std::memcmp(der.data(), header, sizeof(header)) != 0 ||
        der[sizeof(header)] != 4) return false;
    point.assign(der.begin() + sizeof(header), der.end());
    return true;
}
} // namespace motion_main_crypto

inline int mbedtls_pk_parse_public_key(mbedtls_pk_context* ctx,
                                        const unsigned char* pem, size_t size) {
    if (!ctx) return -1;
    mbedtls_pk_free(ctx);
    std::vector<unsigned char> point;
    if (!motion_main_crypto::p256Point(pem, size, point)) return -1;
#if defined(__APPLE__)
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, point.data(),
                                   static_cast<CFIndex>(point.size()));
    if (!data) return -1;
    const void* names[] = {kSecAttrKeyType, kSecAttrKeyClass};
    const void* values[] = {kSecAttrKeyTypeECSECPrimeRandom, kSecAttrKeyClassPublic};
    CFDictionaryRef attributes = CFDictionaryCreate(kCFAllocatorDefault,
        names, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFErrorRef error = nullptr;
    if (attributes) ctx->native = SecKeyCreateWithData(data, attributes, &error);
    if (error) CFRelease(error);
    if (attributes) CFRelease(attributes);
    CFRelease(data);
#else
    if (size > static_cast<size_t>(INT_MAX)) return -1;
    BIO* bio = BIO_new_mem_buf(pem, static_cast<int>(size));
    if (!bio) return -1;
    ctx->native = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
#endif
    return ctx->native ? 0 : -1;
}

inline int mbedtls_pk_verify(mbedtls_pk_context* ctx, mbedtls_md_type_t type,
                              const unsigned char* hash, size_t hashSize,
                              const unsigned char* signature, size_t signatureSize) {
    if (!ctx || !ctx->native || type != MBEDTLS_MD_SHA256 || !hash ||
        hashSize != 32 || !signature || !signatureSize || signatureSize > 80) return -1;
#if defined(__APPLE__)
    if (!SecKeyIsAlgorithmSupported(ctx->native, kSecKeyOperationTypeVerify,
        kSecKeyAlgorithmECDSASignatureDigestX962SHA256)) return -1;
    CFDataRef digest = CFDataCreate(kCFAllocatorDefault, hash, 32);
    CFDataRef der = CFDataCreate(kCFAllocatorDefault, signature,
                                 static_cast<CFIndex>(signatureSize));
    CFErrorRef error = nullptr;
    const bool valid = digest && der && SecKeyVerifySignature(ctx->native,
        kSecKeyAlgorithmECDSASignatureDigestX962SHA256, digest, der, &error);
    if (error) CFRelease(error);
    if (der) CFRelease(der);
    if (digest) CFRelease(digest);
    return valid ? 0 : -1;
#else
    EVP_PKEY_CTX* verifier = EVP_PKEY_CTX_new(ctx->native, nullptr);
    if (!verifier) return -1;
    const bool valid = EVP_PKEY_verify_init(verifier) == 1 &&
        EVP_PKEY_CTX_set_signature_md(verifier, EVP_sha256()) == 1 &&
        EVP_PKEY_verify(verifier, signature, signatureSize, hash, hashSize) == 1;
    EVP_PKEY_CTX_free(verifier);
    return valid ? 0 : -1;
#endif
}
