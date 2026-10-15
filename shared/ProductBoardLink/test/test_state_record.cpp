#include "BrainStateRecord.h"
#include "BoardPairingRecord.h"
#include "FakeProductCrypto.h"
#include "mbedtls/sha256.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace babytech;
using namespace babytech::boardlink;
using Bytes = std::vector<uint8_t>;
static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
    std::exit(1); } } while (false)

template<class T, class = void> struct HasStop : std::false_type {};
template<class T> struct HasStop<T, std::void_t<decltype(T::Stop)>> : std::true_type {};
static_assert(!HasStop<ProductCommand>::value, "Stop cannot be a durable ordinary command");
static_assert(kBrainStateMaxSize == 1491, "Review durable-record capacity changes");

template<class T> Bytes snapshot(const T& value) {
    const auto* at = reinterpret_cast<const uint8_t*>(&value);
    return Bytes(at, at + sizeof(value));
}
Bytes unhex(const char* value) {
    Bytes result;
    while (*value) {
        unsigned byte = 0;
        CHECK(std::sscanf(value, "%2x", &byte) == 1);
        result.push_back(uint8_t(byte)); value += 2;
    }
    return result;
}
v4::Pairing pairing() {
    v4::Pairing p;
    std::strcpy(p.deviceId, "dev-1");
    std::strcpy(p.epoch, "0123456789abcdef0123456789abcdef");
    std::strcpy(p.localPhysicalId, "123456789abc");
    std::strcpy(p.peerPhysicalId, "abcdef123456");
    return p;
}
ProductContext context() {
    ProductContext c;
    std::strcpy(c.deviceId, "dev-1"); c.profileVersion = 7;
    std::strcpy(c.babyId, "baby-1"); std::strcpy(c.babyName, "Mia");
    std::strcpy(c.formulaBrand, "Friso");
    c.waterMl = 180; c.temperatureC = 40; c.powderGPer100Ml = 13.1f;
    return c;
}
ProductRequest request() {
    ProductRequest r;
    r.source = v4::Source::CloudCommand; r.command = ProductCommand::Prepare;
    r.sequence = 42; std::strcpy(r.deviceId, "dev-1"); std::strcpy(r.commandId, "cmd-A");
    std::strcpy(r.babyId, "baby-1"); r.profileVersion = 7;
    r.waterMl = 180; r.temperatureC = 40; r.powderGPer100Ml = 13.1f;
    return r;
}
BrainState state(bool pending = true) {
    BrainState s; s.pairing = pairing(); s.hasContext = true; s.context = context();
    s.localSequence = 42; s.pending = pending;
    if (pending) {
        s.pendingRequest = request(); s.pendingRequest.source = v4::Source::LocalTouch;
        CHECK(makeLocalCommandId(s.pairing, 42, s.pendingRequest.commandId));
        CHECK(requestDigest(s.pendingRequest, s.pendingDigest));
    }
    return s;
}
template<class T, class Encode> Bytes encode(const T& value, size_t maximum, Encode fn) {
    Bytes bytes(maximum, 0xa5);
    const size_t n = fn(value, bytes.data(), bytes.size());
    CHECK(n && n <= maximum); bytes.resize(n); return bytes;
}
template<class T, class Decode> void reject(const Bytes& bytes, T original, Decode fn) {
    const Bytes before = snapshot(original);
    CHECK(!fn(bytes.data(), bytes.size(), original));
    CHECK(snapshot(original) == before);
}
template<class T, class Encode> void rejectEncode(const T& value, size_t capacity, Encode fn) {
    Bytes output(capacity + 16, 0xa5); const Bytes before = output;
    CHECK(fn(value, output.data(), capacity) == 0); CHECK(output == before);
}
template<class T, class Encode, class Decode, class Equal>
void codec(const T& value, size_t maximum, Encode enc, Decode dec, Equal equal) {
    const Bytes bytes = encode(value, maximum, enc);
    T out{}; CHECK(dec(bytes.data(), bytes.size(), out)); CHECK(equal(value, out));
    for (size_t n = 0; n < bytes.size(); ++n) {
        reject(Bytes(bytes.begin(), bytes.begin() + n), value, dec);
        rejectEncode(value, n, enc);
    }
    Bytes extra = bytes; extra.push_back(0); reject(extra, value, dec);
    extra.resize(maximum + 1); reject(extra, value, dec);
    auto before = snapshot(out);
    CHECK(!dec(nullptr, bytes.size(), out)); CHECK(snapshot(out) == before);
    CHECK(!enc(value, nullptr, maximum));
    Bytes exact(bytes.size() + 1, 0x5a);
    CHECK(enc(value, exact.data(), bytes.size()) == bytes.size());
    CHECK(exact.back() == 0x5a);
}

void golden() {
    // Independent Python: struct.pack('<...') + UTF-8/u16 lengths, hashlib.sha256.
    const Bytes contextBytes = unhex("010005006465762d31070000000600626162792d3103004d69610500467269736fb400289a995141");
    const Bytes requestBytes = unhex("0101022a0000000000000005006465762d310500636d642d410600626162792d3107000000b400289a995141");
    CHECK(encode(context(), kContextIdentityMaxSize, encodeContextIdentity) == contextBytes);
    CHECK(encode(request(), kRequestIdentityMaxSize, encodeRequestIdentity) == requestBytes);
    uint8_t digest[32];
    CHECK(contextDigest(context(), digest));
    CHECK(Bytes(digest, digest + 32) == unhex("1ddbcc3f5b0d76f63dc1379471c30f9f30cd736586757cfc617c688436170be1"));
    CHECK(requestDigest(request(), digest));
    CHECK(Bytes(digest, digest + 32) == unhex("3b066eda8fb23571235b64e708155275db41df1f4a360b0a008bb6c6ae87cdb9"));
    auto r = request(); r.powderGPer100Ml = static_cast<float>(13.1000001);
    CHECK(encode(r, kRequestIdentityMaxSize, encodeRequestIdentity) == requestBytes);
    r.powderGPer100Ml = std::nextafter(13.1f, 50.f);
    CHECK(encode(r, kRequestIdentityMaxSize, encodeRequestIdentity) != requestBytes);
    CHECK(requestDigest(r, digest));
    CHECK(Bytes(digest, digest + 32) != unhex("3b066eda8fb23571235b64e708155275db41df1f4a360b0a008bb6c6ae87cdb9"));
    for (unsigned field = 0; field < 10; ++field) {
        auto changed = request();
        if (field == 0) changed.source = v4::Source::LocalTouch;
        if (field == 1) ++changed.sequence;
        if (field == 2) std::strcpy(changed.deviceId, "dev-2");
        if (field == 3) std::strcpy(changed.commandId, "cmd-B");
        if (field == 4) std::strcpy(changed.babyId, "baby-2");
        if (field == 5) ++changed.profileVersion;
        if (field == 6) ++changed.waterMl;
        if (field == 7) ++changed.temperatureC;
        if (field == 8) changed.powderGPer100Ml = 14.f;
        if (field == 9) {
            changed = ProductRequest{}; changed.command = ProductCommand::Clean;
            changed.sequence = 42; std::strcpy(changed.deviceId, "dev-1");
            std::strcpy(changed.commandId, "cmd-A");
        }
        CHECK(requestDigest(changed, digest));
        CHECK(Bytes(digest, digest + 32) != unhex("3b066eda8fb23571235b64e708155275db41df1f4a360b0a008bb6c6ae87cdb9"));
        CHECK(!sameProductRequest(changed, request()));
    }
}

void contextIdentity() {
    const auto original = context();
    codec(original, kContextIdentityMaxSize, encodeContextIdentity, decodeContextIdentity, sameProductContext);
    auto cleared = ProductContext{}; std::strcpy(cleared.deviceId, "dev-1");
    cleared.profileVersion = INT32_MAX; cleared.cleared = true;
    codec(cleared, kContextIdentityMaxSize, encodeContextIdentity, decodeContextIdentity, sameProductContext);
    auto max = original;
    std::memset(max.deviceId, 'd', 64); max.deviceId[64] = 0;
    std::memset(max.babyId, 'b', 96); max.babyId[96] = 0;
    std::memset(max.babyName, 'n', 320); max.babyName[320] = 0;
    std::memset(max.formulaBrand, 'f', 480); max.formulaBrand[480] = 0;
    CHECK(encode(max, kContextIdentityMaxSize, encodeContextIdentity).size() == 981);
    codec(max, kContextIdentityMaxSize, encodeContextIdentity, decodeContextIdentity, sameProductContext);
    auto unicode = original;
    std::strcpy(unicode.babyId, "b\xc3\xa9"); std::strcpy(unicode.babyName, "\xe5\xae\x9d\xe5\xae\x9d");
    std::strcpy(unicode.formulaBrand, "\xf0\x9f\x8d\xbc");
    codec(unicode, kContextIdentityMaxSize, encodeContextIdentity, decodeContextIdentity, sameProductContext);
    const Bytes good = encode(original, kContextIdentityMaxSize, encodeContextIdentity);
    for (auto index : {0u, 1u}) {
        auto bad = good; bad[index] = 2; reject(bad, original, decodeContextIdentity);
    }
    for (auto index : {2u, 13u, 21u, 26u}) {
        auto bad = good; bad[index] = 0xff; bad[index + 1] = 0xff;
        reject(bad, original, decodeContextIdentity);
    }
    for (auto index : {4u, 15u, 23u, 28u}) {
        for (uint8_t byte : {uint8_t(0), uint8_t(0xff), uint8_t(0xc0), uint8_t(0x80)}) {
            auto bad = good; bad[index] = byte; reject(bad, original, decodeContextIdentity);
        }
    }
    for (auto version : {uint32_t(0), uint32_t(INT32_MAX) + 1, UINT32_MAX}) {
        auto c = original; c.profileVersion = version;
        rejectEncode(c, kContextIdentityMaxSize, encodeContextIdentity);
        auto bad = good;
        for (unsigned i = 0; i < 4; ++i) bad[9 + i] = uint8_t(version >> (8 * i));
        reject(bad, original, decodeContextIdentity);
    }
    for (unsigned field = 0; field < 4; ++field) {
        auto c = original;
        if (field == 0) std::memset(c.deviceId, 'd', sizeof(c.deviceId));
        if (field == 1) std::memset(c.babyId, 'b', sizeof(c.babyId));
        if (field == 2) std::memset(c.babyName, 'n', sizeof(c.babyName));
        if (field == 3) std::memset(c.formulaBrand, 'f', sizeof(c.formulaBrand));
        rejectEncode(c, kContextIdentityMaxSize, encodeContextIdentity);
    }
    auto bad = good; bad[1] = 1; reject(bad, original, decodeContextIdentity);
    auto c = cleared; c.waterMl = 30; rejectEncode(c, kContextIdentityMaxSize, encodeContextIdentity);
    c = original; c.babyName[0] = 0; c.formulaBrand[0] = 0;
    codec(c, kContextIdentityMaxSize, encodeContextIdentity, decodeContextIdentity, sameProductContext);
    for (const char* invalid : {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82", "\x80"}) {
        for (unsigned field = 0; field < 3; ++field) {
            c = original;
            std::strcpy(field == 0 ? c.babyId : field == 1 ? c.babyName : c.formulaBrand, invalid);
            rejectEncode(c, kContextIdentityMaxSize, encodeContextIdentity);
        }
    }
}

void requests() {
    const auto original = request();
    codec(original, kRequestIdentityMaxSize, encodeRequestIdentity, decodeRequestIdentity, sameProductRequest);
    for (unsigned command = 0; command < 256; ++command) {
        ProductRequest r; r.source = v4::Source::LocalTouch; r.sequence = 1;
        std::strcpy(r.deviceId, "dev-1"); std::strcpy(r.commandId, "local-id");
        r.command = static_cast<ProductCommand>(command);
        if (r.command == ProductCommand::Prepare) r = original;
        if (r.command == ProductCommand::SetTargetTemp) r.temperatureC = 40;
        const bool expected = command >= 1 && command <= 6;
        CHECK(validProductRequest(r) == expected);
        if (expected) codec(r, kRequestIdentityMaxSize, encodeRequestIdentity, decodeRequestIdentity, sameProductRequest);
        else rejectEncode(r, kRequestIdentityMaxSize, encodeRequestIdentity);
    }
    for (unsigned source = 0; source < 256; ++source) {
        auto r = original; r.source = static_cast<v4::Source>(source);
        CHECK(validProductRequest(r) == (source == 1 || source == 2));
    }
    ProductRequest initialize; initialize.command = ProductCommand::Initialize; initialize.sequence = 1;
    std::strcpy(initialize.deviceId, "dev-1"); std::strcpy(initialize.commandId, "cmd-init");
    CHECK(validProductRequest(initialize));
    initialize.source = v4::Source::CloudCommand;
    CHECK(!validProductRequest(initialize));
    rejectEncode(initialize, kRequestIdentityMaxSize, encodeRequestIdentity);
    initialize.source = v4::Source::LocalTouch;
    auto initBytes = encode(initialize, kRequestIdentityMaxSize, encodeRequestIdentity);
    initBytes[1] = uint8_t(v4::Source::CloudCommand);
    reject(initBytes, initialize, decodeRequestIdentity);
    for (unsigned field = 0; field < 6; ++field) {
        auto dirty = initialize;
        if (field == 0) dirty.babyId[0] = 'b';
        if (field == 1) dirty.profileVersion = 1;
        if (field == 2) dirty.waterMl = 180;
        if (field == 3) dirty.temperatureC = 40;
        if (field == 4) dirty.powderGPer100Ml = 13.f;
        if (field == 5) dirty.powderGPer100Ml = -0.f;
        CHECK(!validProductRequest(dirty));
        rejectEncode(dirty, kRequestIdentityMaxSize, encodeRequestIdentity);
    }
    for (uint64_t seq : {uint64_t(0), uint64_t(INT64_MAX) + 1, UINT64_MAX}) {
        auto r = original; r.sequence = seq; CHECK(!validProductRequest(r));
        rejectEncode(r, kRequestIdentityMaxSize, encodeRequestIdentity);
    }
    auto r = original; r.sequence = INT64_MAX;
    codec(r, kRequestIdentityMaxSize, encodeRequestIdentity, decodeRequestIdentity, sameProductRequest);
    for (unsigned field = 0; field < 3; ++field) {
        r = original;
        char* p = field == 0 ? r.deviceId : field == 1 ? r.commandId : r.babyId;
        const size_t capacity = field == 0 ? sizeof(r.deviceId) : field == 1 ? sizeof(r.commandId) : sizeof(r.babyId);
        std::memset(p, 'a', capacity); CHECK(!validProductRequest(r));
        p[capacity - 1] = 0; CHECK(validProductRequest(r));
        p[0] = 0; CHECK(!validProductRequest(r));
        p[0] = char(0xff); CHECK(!validProductRequest(r));
    }
    for (const char* id : {"-dev", "_dev", "dev.id", "dev/id", "dev id"}) {
        r = original; std::strcpy(r.deviceId, id); CHECK(!validProductRequest(r));
    }
    for (uint32_t version : {uint32_t(0), uint32_t(INT32_MAX) + 1, UINT32_MAX}) {
        r = original; r.profileVersion = version; CHECK(!validProductRequest(r));
    }
    for (unsigned water : {0u, 1u, 29u, 30u, 500u, 501u, 65535u}) {
        r = original; r.waterMl = uint16_t(water);
        CHECK(validProductRequest(r) == (water >= 30 && water <= 500));
    }
    for (unsigned temperature : {0u, 1u, 34u, 35u, 60u, 61u, 255u}) {
        r = original; r.temperatureC = uint8_t(temperature);
        CHECK(validProductRequest(r) == (temperature >= 35 && temperature <= 60));
    }
    for (float ratio : {0.f, -1.f, .999f, 1.f, 50.f, 50.001f,
                        std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::quiet_NaN()}) {
        r = original; r.powderGPer100Ml = ratio;
        CHECK(validProductRequest(r) == (std::isfinite(ratio) && ratio >= 1 && ratio <= 50));
    }
    // C++ numeric fields are already normalized; assigning bool yields 0/1, not a JSON type.
    r = original; r.waterMl = true; CHECK(!validProductRequest(r));
    r = original; r.temperatureC = true; CHECK(!validProductRequest(r));
    r = original; r.powderGPer100Ml = false; CHECK(!validProductRequest(r));
    auto max = original;
    std::memset(max.deviceId, 'd', 64); max.deviceId[64] = 0;
    std::memset(max.commandId, 'c', 128); max.commandId[128] = 0;
    std::memset(max.babyId, 'b', 96); max.babyId[96] = 0;
    CHECK(encode(max, kRequestIdentityMaxSize, encodeRequestIdentity).size() == 316);
    codec(max, kRequestIdentityMaxSize, encodeRequestIdentity, decodeRequestIdentity, sameProductRequest);
    auto good = encode(original, kRequestIdentityMaxSize, encodeRequestIdentity);
    for (size_t index : {size_t(0), size_t(1), size_t(2)}) {
        auto bad = good; bad[index] = 255; reject(bad, original, decodeRequestIdentity);
    }
    for (size_t index : {size_t(11), size_t(18), size_t(25)}) {
        auto bad = good; bad[index] = 255; bad[index + 1] = 255; reject(bad, original, decodeRequestIdentity);
    }
    for (size_t index : {size_t(13), size_t(20), size_t(27)}) {
        auto bad = good; bad[index] = 0; reject(bad, original, decodeRequestIdentity);
        bad[index] = 255; reject(bad, original, decodeRequestIdentity);
    }
    r = original; std::strcpy(r.commandId, "\xe5\x91\xbd\xe4\xbb\xa4");
    std::strcpy(r.babyId, "\xc3\xa9");
    codec(r, kRequestIdentityMaxSize, encodeRequestIdentity, decodeRequestIdentity, sameProductRequest);
}

void localIds() {
    const auto p = pairing(); char id[129]{};
    CHECK(makeLocalCommandId(p, INT64_MAX, id));
    CHECK(std::string(id) == "local-0123456789abcdef0123456789abcdef-9223372036854775807");
    CHECK(makeLocalCommandId(p, 1, id));
    CHECK(std::string(id) == "local-0123456789abcdef0123456789abcdef-1");
    for (uint64_t seq : {uint64_t(0), uint64_t(INT64_MAX) + 1, UINT64_MAX}) {
        const Bytes before = snapshot(id); CHECK(!makeLocalCommandId(p, seq, id)); CHECK(snapshot(id) == before);
    }
    for (unsigned field = 0; field < 7; ++field) {
        auto bad = p;
        if (field == 0) bad.role = v4::Role::Motion;
        if (field == 1) bad.role = static_cast<v4::Role>(0);
        if (field == 2) std::memset(bad.epoch, '0', 32);
        if (field == 3) bad.epoch[0] = 'G';
        if (field == 4) bad.epoch[32] = 'a';
        if (field == 5) std::strcpy(bad.peerPhysicalId, bad.localPhysicalId);
        if (field == 6) bad.deviceId[0] = '-';
        const Bytes before = snapshot(id); CHECK(!makeLocalCommandId(bad, 1, id)); CHECK(snapshot(id) == before);
    }
}

void cryptoFailures() {
    fake_product_crypto::reset();
    uint8_t output[32]; std::memset(output, 0x5a, sizeof(output));
    const Bytes before = snapshot(output);
    fake_product_crypto::fail = true;
    CHECK(!requestDigest(request(), output)); CHECK(snapshot(output) == before);
    CHECK(!contextDigest(context(), output)); CHECK(snapshot(output) == before);
    CHECK(fake_product_crypto::calls == 2);
    fake_product_crypto::reset();
    auto invalid = request(); invalid.sequence = 0;
    CHECK(!requestDigest(invalid, output)); CHECK(snapshot(output) == before);
    auto c = context(); c.profileVersion = 0;
    CHECK(!contextDigest(c, output)); CHECK(snapshot(output) == before);
    CHECK(fake_product_crypto::calls == 0);
    fake_product_crypto::fail = true;
#if MBEDTLS_VERSION_MAJOR == 2
    CHECK(mbedtls_sha256_ret(reinterpret_cast<const uint8_t*>("abc"), 3, output, 0) != 0);
#else
    CHECK(mbedtls_sha256(reinterpret_cast<const uint8_t*>("abc"), 3, output, 0) != 0);
#endif
    CHECK(snapshot(output) == before); fake_product_crypto::reset();
}

void putLE(Bytes& bytes, size_t at, uint64_t value, size_t width) {
    CHECK(at + width <= bytes.size());
    for (size_t i = 0; i < width; ++i) bytes[at + i] = uint8_t(value >> (8 * i));
}
size_t get16(const Bytes& bytes, size_t at) {
    CHECK(at + 2 <= bytes.size()); return bytes[at] | (size_t(bytes[at + 1]) << 8);
}
void repairCrc(Bytes& bytes, size_t start = 0, size_t length = 0) {
    if (!length) length = bytes.size() - start;
    CHECK(length >= 12 && start + length <= bytes.size());
    // Test-side ISO-HDLC CRC, independently anchored by the Python/zlib golden record.
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        if (i >= 8 && i < 12) continue;
        crc ^= bytes[start + i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    putLE(bytes, start + 8, crc ^ UINT32_MAX, 4);
}

void recordSemantics() {
    const auto original = state();
    const Bytes good = encode(original, kBrainStateMaxSize, encodeBrainState);
    const size_t pairAt = 14, pairLength = get16(good, 12);
    const size_t hasContextAt = pairAt + pairLength;
    const size_t contextAt = hasContextAt + 3;
    const size_t contextLength = get16(good, hasContextAt + 1);
    const size_t sequenceAt = contextAt + contextLength;
    const size_t pendingAt = sequenceAt + 8, requestAt = pendingAt + 3;
    const size_t requestLength = get16(good, pendingAt + 1);
    const size_t digestAt = requestAt + requestLength;
    CHECK(digestAt + 32 == good.size());
    auto fail = [&](Bytes bad, bool fixPair = false) {
        if (fixPair) repairCrc(bad, pairAt, pairLength);
        repairCrc(bad); reject(bad, original, decodeBrainState);
    };
    auto mutate = [&](size_t at, uint64_t value, size_t width = 1, bool pair = false) {
        auto bad = good; putLE(bad, at, value, width); fail(bad, pair);
    };
    for (size_t at = 0; at < 4; ++at) mutate(at, 0);
    mutate(4, 0, 2); mutate(4, 2, 2); mutate(6, 0, 2); mutate(6, 65535, 2);
    mutate(12, 0, 2); mutate(12, 65535, 2);
    for (size_t at = pairAt; at < pairAt + 4; ++at) mutate(at, 0, 1, true);
    mutate(pairAt + 4, 2, 2, true); mutate(pairAt + 6, 0, 2, true);
    mutate(pairAt + 12, 2, 1, true); mutate(pairAt + 13, 0, 1, true);
    // Recompute BOTH CRCs: rejection must come from identity semantics, not checksums.
    for (size_t at : {pairAt + 14, pairAt + 46, pairAt + 58, pairAt + 70})
        mutate(at, '!', 1, true);
    auto bad = good;
    std::fill(bad.begin() + pairAt + 14, bad.begin() + pairAt + 46, '0'); fail(bad, true);
    bad = good; std::copy_n(bad.begin() + pairAt + 46, 12, bad.begin() + pairAt + 58); fail(bad, true);
    mutate(pairAt + 70, 'x', 1, true); // Valid paired device, mismatched context/request.
    mutate(hasContextAt, 2); mutate(hasContextAt, 255);
    mutate(hasContextAt + 1, 0, 2); mutate(hasContextAt + 1, 65535, 2);
    mutate(contextAt, 2); mutate(contextAt + 1, 2);
    for (size_t at : {contextAt + 2, contextAt + 13, contextAt + 21, contextAt + 26})
        mutate(at, 65535, 2);
    for (size_t at : {contextAt + 4, contextAt + 15, contextAt + 23, contextAt + 28}) {
        mutate(at, 0); mutate(at, 255);
    }
    mutate(contextAt + 9, 0, 4); mutate(contextAt + 9, uint32_t(INT32_MAX) + 1, 4);
    mutate(contextAt + 33, 29, 2); mutate(contextAt + 35, 34);
    mutate(contextAt + 36, 0x7fc00000, 4);
    mutate(sequenceAt, 0, 8); mutate(sequenceAt, uint64_t(INT64_MAX) + 1, 8);
    mutate(sequenceAt, 41, 8); mutate(sequenceAt, 43, 8);
    mutate(pendingAt, 2); mutate(pendingAt, 255);
    mutate(pendingAt + 1, 0, 2); mutate(pendingAt + 1, 65535, 2);
    mutate(requestAt, 2); mutate(requestAt + 1, 1); mutate(requestAt + 2, 0);
    mutate(requestAt + 3, 0, 8); mutate(requestAt + 3, 43, 8);
    const size_t commandLengthAt = requestAt + 18;
    const size_t babyLengthAt = commandLengthAt + 2 + get16(good, commandLengthAt);
    const size_t versionAt = babyLengthAt + 2 + get16(good, babyLengthAt);
    for (size_t at : {requestAt + 11, commandLengthAt, babyLengthAt}) mutate(at, 65535, 2);
    for (size_t at : {requestAt + 13, commandLengthAt + 2, babyLengthAt + 2}) {
        mutate(at, 0); mutate(at, 255);
    }
    mutate(versionAt, 0, 4); mutate(versionAt + 4, 501, 2);
    mutate(versionAt + 6, 61); mutate(versionAt + 7, 0x7f800000, 4);
    for (size_t i = 0; i < 32; ++i) mutate(digestAt + i, good[digestAt + i] ^ 1);
    // A valid recipe edit without a fresh digest is rejected even with a valid CRC.
    mutate(versionAt + 4, 190, 2);
    bad = good; bad.push_back(0); putLE(bad, 6, bad.size() - 12, 2); fail(bad);
    // Current metadata may change independently of a frozen request. CRC is not authentication.
    bad = good; bad[contextAt + 23] = 'L'; repairCrc(bad);
    BrainState changed; CHECK(decodeBrainState(bad.data(), bad.size(), changed));
    CHECK(std::string(changed.context.babyName) == "Lia");
    CHECK(sameProductRequest(changed.pendingRequest, original.pendingRequest));

    for (unsigned field = 0; field < 6; ++field) {
        auto invalid = original;
        if (field == 0) { invalid.hasContext = false; invalid.context = ProductContext{}; }
        if (field == 1) invalid.context.profileVersion = 6;
        if (field == 2) std::strcpy(invalid.context.babyId, "other");
        if (field == 3) invalid.context.powderGPer100Ml = 14.f;
        if (field == 4) {
            invalid.context = ProductContext{}; std::strcpy(invalid.context.deviceId, "dev-1");
            invalid.context.profileVersion = 7; invalid.context.cleared = true;
        }
        if (field == 5) invalid.localSequence = 43;
        CHECK(!validBrainState(invalid)); rejectEncode(invalid, kBrainStateMaxSize, encodeBrainState);
    }
    for (unsigned field = 0; field < 3; ++field) {
        auto invalid = state(false);
        if (field == 0) invalid.pendingRequest = request();
        if (field == 1) invalid.pendingDigest[31] = 1;
        if (field == 2) invalid.hasContext = false;
        CHECK(!validBrainState(invalid)); rejectEncode(invalid, kBrainStateMaxSize, encodeBrainState);
    }
    auto customRecipe = original;
    customRecipe.pendingRequest.waterMl = 210; customRecipe.pendingRequest.temperatureC = 45;
    CHECK(requestDigest(customRecipe.pendingRequest, customRecipe.pendingDigest));
    CHECK(validBrainState(customRecipe));
    auto noContext = state(false); noContext.hasContext = false; noContext.context = ProductContext{};
    noContext.pending = true; noContext.pendingRequest.command = ProductCommand::Initialize;
    noContext.pendingRequest.sequence = noContext.localSequence;
    std::strcpy(noContext.pendingRequest.deviceId, noContext.pairing.deviceId);
    CHECK(makeLocalCommandId(noContext.pairing, noContext.localSequence, noContext.pendingRequest.commandId));
    CHECK(requestDigest(noContext.pendingRequest, noContext.pendingDigest));
    codec(noContext, kBrainStateMaxSize, encodeBrainState, decodeBrainState, sameBrainState);
}

void records() {
    const auto original = state();
    codec(original, kBrainStateMaxSize, encodeBrainState, decodeBrainState, sameBrainState);
    codec(state(false), kBrainStateMaxSize, encodeBrainState, decodeBrainState, sameBrainState);
    BrainState empty; empty.pairing = pairing();
    codec(empty, kBrainStateMaxSize, encodeBrainState, decodeBrainState, sameBrainState);
    auto old = original; ++old.context.profileVersion; std::strcpy(old.context.babyId, "new-baby");
    codec(old, kBrainStateMaxSize, encodeBrainState, decodeBrainState, sameBrainState);
    old.context = ProductContext{}; std::strcpy(old.context.deviceId, "dev-1");
    old.context.profileVersion = 8; old.context.cleared = true;
    codec(old, kBrainStateMaxSize, encodeBrainState, decodeBrainState, sameBrainState);
    auto maxSeq = original; maxSeq.localSequence = INT64_MAX;
    maxSeq.pendingRequest.sequence = INT64_MAX;
    CHECK(makeLocalCommandId(maxSeq.pairing, INT64_MAX, maxSeq.pendingRequest.commandId));
    CHECK(requestDigest(maxSeq.pendingRequest, maxSeq.pendingDigest));
    codec(maxSeq, kBrainStateMaxSize, encodeBrainState, decodeBrainState, sameBrainState);
    for (unsigned field = 0; field < 11; ++field) {
        auto bad = original;
        if (field == 0) bad.localSequence = uint64_t(INT64_MAX) + 1;
        if (field == 1) bad.localSequence = 0;
        if (field == 2) bad.localSequence = 41;
        if (field == 3) bad.pairing.role = v4::Role::Motion;
        if (field == 4) std::strcpy(bad.context.deviceId, "wrong");
        if (field == 5) bad.pendingRequest.source = v4::Source::CloudCommand;
        if (field == 6) std::strcpy(bad.pendingRequest.deviceId, "wrong");
        if (field == 7) std::strcpy(bad.pendingRequest.commandId, "wrong");
        if (field == 8) bad.pendingDigest[0] ^= 1;
        if (field == 9) bad.pendingRequest.waterMl = 190;
        if (field == 10) bad.pairing.epoch[0] = '9';
        CHECK(!validBrainState(bad)); rejectEncode(bad, kBrainStateMaxSize, encodeBrainState);
    }
    const Bytes good = encode(original, kBrainStateMaxSize, encodeBrainState);
    // Independently generated with Python struct, hashlib and zlib, not production codecs.
    CHECK(good == unhex(
        "424253310100f300bae9ab304b004254503101003f0074656d3c010530313233343536373839616263646566"
        "303132333435363738396162636465663132333435363738396162636162636465663132333435366465762d31"
        "012800010005006465762d31070000000600626162792d3103004d69610500467269736fb400289a995141"
        "2a000000000000000150000102022a0000000000000005006465762d3129006c6f63616c2d3031323334353637"
        "3839616263646566303132333435363738396162636465662d34320600626162792d3107000000b400289a995141"
        "bb853edf06192104f76cb0c8906fb86cea7d3898a00da3db9fc01a42b5407a7b"));
    for (size_t i = 0; i < good.size(); ++i) {
        auto corrupt = good; corrupt[i] ^= 1; reject(corrupt, original, decodeBrainState);
    }
    fake_product_crypto::fail = true;
    CHECK(!validBrainState(original)); rejectEncode(original, kBrainStateMaxSize, encodeBrainState);
    reject(good, original, decodeBrainState);
    fake_product_crypto::reset();
    auto maximum = original;
    std::memset(maximum.pairing.deviceId, 'd', 64); maximum.pairing.deviceId[64] = 0;
    std::strcpy(maximum.context.deviceId, maximum.pairing.deviceId);
    std::strcpy(maximum.pendingRequest.deviceId, maximum.pairing.deviceId);
    std::memset(maximum.context.babyId, 'b', 96); maximum.context.babyId[96] = 0;
    std::strcpy(maximum.pendingRequest.babyId, maximum.context.babyId);
    std::memset(maximum.context.babyName, 'n', 320); maximum.context.babyName[320] = 0;
    std::memset(maximum.context.formulaBrand, 'f', 480); maximum.context.formulaBrand[480] = 0;
    maximum.localSequence = INT64_MAX; maximum.pendingRequest.sequence = INT64_MAX;
    CHECK(makeLocalCommandId(maximum.pairing, INT64_MAX, maximum.pendingRequest.commandId));
    CHECK(requestDigest(maximum.pendingRequest, maximum.pendingDigest));
    const Bytes maxBytes = encode(maximum, kBrainStateMaxSize, encodeBrainState);
    CHECK(maxBytes.size() <= 1491);
    CHECK(maxBytes.size() == 1421);
    std::printf("Maximum local record: %zu bytes (bound %zu)\n", maxBytes.size(), kBrainStateMaxSize);
    codec(maximum, kBrainStateMaxSize, encodeBrainState, decodeBrainState, sameBrainState);
}

int main(int argc, char** argv) {
    struct Group { const char* name; void (*run)(); };
    const Group groups[] = {{"golden", golden}, {"context", contextIdentity},
        {"request", requests}, {"local-id", localIds}, {"crypto", cryptoFailures},
        {"record", records}, {"record-semantics", recordSemantics}};
    bool selected = false;
    for (const auto& group : groups) {
        if (argc > 1 && std::strcmp(argv[1], group.name)) continue;
        selected = true; fake_product_crypto::reset(); group.run();
        std::printf("PASS %s\n", group.name);
    }
    CHECK(selected); std::printf("PASS product state: %u checks\n", checks);
}
