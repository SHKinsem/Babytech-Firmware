#pragma once

#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

namespace motion_main_crypto_fixture {
namespace detail {
struct ShaScope {
    mbedtls_sha256_context value;
    ShaScope() { mbedtls_sha256_init(&value); }
    ~ShaScope() { mbedtls_sha256_free(&value); }
    ShaScope(const ShaScope&) = delete;
    ShaScope& operator=(const ShaScope&) = delete;
};
struct KeyScope {
    mbedtls_pk_context value;
    KeyScope() { mbedtls_pk_init(&value); }
    ~KeyScope() { mbedtls_pk_free(&value); }
    KeyScope(const KeyScope&) = delete;
    KeyScope& operator=(const KeyScope&) = delete;
};
} // namespace detail

// SDK crypto regression only, not WifiOta HTTP/auth/Flash/bootloader acceptance.
// This disposable P-256 key and DER signature were generated offline for this
// message. No production OTA key, signing secret or runtime command is used.
inline unsigned run() {
    unsigned checks = 0;
    const auto require = [&checks](bool ok, const char* message) {
        ++checks;
        if (!ok) throw std::runtime_error(std::string("Motion crypto: ") + message);
    };
    static constexpr unsigned char abc[] = {'a', 'b', 'c'};
    static constexpr unsigned char expectedSha[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    std::array<unsigned char, 32> oneShot{}, incremental{};
    require(mbedtls_sha256_ret(abc, sizeof(abc), oneShot.data(), 0) == 0, "one-shot SHA failed");
    require(std::memcmp(oneShot.data(), expectedSha, sizeof(expectedSha)) == 0, "SHA abc vector mismatch");
    detail::ShaScope sha;
    require(mbedtls_sha256_starts_ret(&sha.value, 0) == 0, "SHA starts failed");
    require(mbedtls_sha256_update_ret(&sha.value, abc, 1) == 0, "SHA first chunk failed");
    require(mbedtls_sha256_update_ret(&sha.value, nullptr, 0) == 0, "SHA empty chunk failed");
    require(mbedtls_sha256_update_ret(&sha.value, abc + 1, 2) == 0, "SHA second chunk failed");
    require(mbedtls_sha256_finish_ret(&sha.value, incremental.data()) == 0, "SHA finish failed");
    require(incremental == oneShot, "incremental and one-shot SHA differ");
    require(std::memcmp(incremental.data(), expectedSha, sizeof(expectedSha)) == 0,
            "incremental SHA abc vector mismatch");

    // RFC 4231 test case 1: key = twenty 0x0b bytes, data = "Hi There".
    std::array<unsigned char, 20> hmacKey;
    hmacKey.fill(0x0b);
    static constexpr unsigned char hmacData[] = "Hi There";
    static constexpr unsigned char expectedHmac[32] = {
        0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53,
        0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
        0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7,
        0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7
    };
    std::array<unsigned char, 32> hmac{};
    const auto* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    require(info != nullptr, "SHA256 HMAC descriptor missing");
    require(mbedtls_md_hmac(info, hmacKey.data(), hmacKey.size(), hmacData,
                             sizeof(hmacData) - 1, hmac.data()) == 0, "HMAC failed");
    require(std::memcmp(hmac.data(), expectedHmac, sizeof(expectedHmac)) == 0,
            "HMAC RFC 4231 vector mismatch");

    static constexpr unsigned char testPublicKey[] =
        "-----BEGIN PUBLIC KEY-----\n"
        "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEQDuURygOP7+yjtNjFCZR3K6hjyUX\n"
        "XxY/fUEgJSbkH/bGOm6A4hKZRXd4LRN3RXQljbAtezUm5yCBnUpkQaKnaw==\n"
        "-----END PUBLIC KEY-----\n";
    static constexpr unsigned char signedMessage[] = "Babytech Motion main crypto fixture v1\n";
    static constexpr unsigned char expectedMessageSha[32] = {
        0xed, 0x2a, 0x6c, 0x6c, 0x03, 0x0d, 0x9e, 0x45,
        0xb8, 0x0b, 0x8c, 0x18, 0xd9, 0x9b, 0xff, 0x3b,
        0x13, 0x83, 0x6f, 0xd9, 0xdd, 0x42, 0x19, 0xa7,
        0x84, 0xab, 0xbf, 0xc9, 0x49, 0x3a, 0xb6, 0x42
    };
    static constexpr std::array<unsigned char, 72> signature{{
        0x30, 0x46, 0x02, 0x21, 0x00, 0xcc, 0x82, 0xd4,
        0x39, 0xc4, 0x8e, 0x27, 0x23, 0x65, 0x89, 0x93,
        0x99, 0xfe, 0x8b, 0x4b, 0x86, 0xd9, 0x4b, 0xc7,
        0x2d, 0x7d, 0x64, 0x58, 0xe4, 0xc5, 0xba, 0x62,
        0x35, 0xba, 0x37, 0x3c, 0xaf, 0x02, 0x21, 0x00,
        0xfd, 0x62, 0x85, 0x24, 0x98, 0x21, 0x01, 0x90,
        0x95, 0x40, 0x7c, 0xf7, 0x96, 0x6d, 0xa6, 0xe4,
        0xe3, 0xa7, 0x41, 0x6e, 0xf5, 0x9c, 0x21, 0x7f,
        0x3a, 0xa2, 0x96, 0x97, 0xa9, 0xfd, 0x5d, 0x07
    }};
    detail::KeyScope key;
    require(mbedtls_pk_parse_public_key(&key.value, testPublicKey, sizeof(testPublicKey)) == 0,
            "fixed test public key parse failed");
    std::array<unsigned char, 32> digest{};
    require(mbedtls_sha256_ret(signedMessage, sizeof(signedMessage) - 1, digest.data(), 0) == 0,
            "signed-message SHA failed");
    require(std::memcmp(digest.data(), expectedMessageSha, sizeof(expectedMessageSha)) == 0,
            "offline signed-message digest mismatch");
    const auto verify = [&](const std::array<unsigned char, 32>& hash,
                            const unsigned char* der, size_t size) {
        return mbedtls_pk_verify(&key.value, MBEDTLS_MD_SHA256, hash.data(), hash.size(), der, size);
    };
    require(verify(digest, signature.data(), signature.size()) == 0, "valid offline signature rejected");
    auto alteredHash = digest;
    alteredHash[0] ^= 1;
    require(verify(alteredHash, signature.data(), signature.size()) != 0, "tampered hash accepted");
    auto alteredSignature = signature;
    alteredSignature.back() ^= 1;
    require(verify(digest, alteredSignature.data(), alteredSignature.size()) != 0, "tampered signature accepted");
    alteredSignature = signature;
    alteredSignature[0] = 0x31;
    require(verify(digest, alteredSignature.data(), alteredSignature.size()) != 0, "malformed DER accepted");
    require(verify(digest, signature.data(), signature.size() - 1) != 0, "truncated DER accepted");
    require(verify(digest, signature.data(), signature.size()) == 0,
            "negative checks corrupted subsequent valid verification");
    return checks;
}
} // namespace motion_main_crypto_fixture
