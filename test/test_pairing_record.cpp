#include "BoardPairingRecord.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using babytech::boardlink::decodePairingRecord;
using babytech::boardlink::encodePairingRecord;
using babytech::boardlink::kPairingRecordMaxSize;
using babytech::v4::Pairing;
using babytech::v4::Role;
using Bytes = std::vector<uint8_t>;
static_assert(kPairingRecordMaxSize == 256, "Adapter buffer contract");

namespace {
Pairing sample() {
    Pairing pairing{};
    pairing.role = Role::Brain;
    std::strcpy(pairing.deviceId, "A");
    std::strcpy(pairing.epoch, "0123456789abcdef0123456789abcdef");
    std::strcpy(pairing.localPhysicalId, "012345abcdef");
    std::strcpy(pairing.peerPhysicalId, "fedcba987654");
    return pairing;
}

void equalFields(const Pairing& left, const Pairing& right) {
    assert(left.role == right.role);
    assert(std::strcmp(left.deviceId, right.deviceId) == 0);
    assert(std::strcmp(left.epoch, right.epoch) == 0);
    assert(std::strcmp(left.localPhysicalId, right.localPhysicalId) == 0);
    assert(std::strcmp(left.peerPhysicalId, right.peerPhysicalId) == 0);
}

void reject(const uint8_t* data, size_t length) {
    Pairing out = sample();
    out.role = Role::Motion;
    // Compare the actual object representation before/after, not two structs'
    // potentially different padding. Failures must not clear or partially copy.
    std::array<uint8_t, sizeof(Pairing)> before{};
    std::memcpy(before.data(), &out, sizeof(out));
    assert(!decodePairingRecord(data, length, out));
    assert(std::memcmp(before.data(), &out, sizeof(out)) == 0);
}

void reject(const Bytes& bytes) { reject(bytes.data(), bytes.size()); }

Bytes encode(const Pairing& pairing) {
    std::array<uint8_t, kPairingRecordMaxSize> buffer{};
    buffer.fill(0xa5);
    const size_t length = encodePairingRecord(pairing, buffer.data(), buffer.size());
    assert(length == 70 + std::strlen(pairing.deviceId));
    assert(length <= kPairingRecordMaxSize);
    for (size_t i = length; i < buffer.size(); ++i) assert(buffer[i] == 0xa5);
    return Bytes(buffer.begin(), buffer.begin() + length);
}

// Independent MSB-first implementation with explicit reflection, also checked
// against the zlib-generated golden fixture below. Used to bypass the CRC gate
// in semantic negative tests, so those tests actually exercise field validation.
uint32_t reflect(uint32_t value, unsigned bits) {
    uint32_t result = 0;
    for (unsigned i = 0; i < bits; ++i) {
        result = (result << 1) | (value & 1);
        value >>= 1;
    }
    return result;
}

void repairCrc(Bytes& bytes) {
    uint32_t crc = 0xffffffff;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i >= 8 && i < 12) continue;
        crc ^= reflect(bytes[i], 8) << 24;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc << 1) ^ ((crc & 0x80000000u) ? 0x04c11db7u : 0u);
    }
    crc = reflect(crc, 32) ^ 0xffffffff;
    for (size_t i = 0; i < 4; ++i) bytes[8 + i] = uint8_t(crc >> (8 * i));
}

void rejectWithCrc(Bytes bytes) {
    repairCrc(bytes);
    reject(bytes);
}

void rejectEncode(const Pairing& pairing) {
    std::array<uint8_t, kPairingRecordMaxSize> buffer{};
    buffer.fill(0x96);
    const auto before = buffer;
    assert(!encodePairingRecord(pairing, buffer.data(), buffer.size()));
    assert(buffer == before);
}

void golden() {
    const char* hex =
        "4254503101003b00ebdbe4ab0101"
        "3031323334353637383961626364656630313233343536373839616263646566"
        "30313233343561626364656666656463626139383736353441";
    Bytes expected;
    for (size_t i = 0; hex[i]; i += 2) {
        unsigned value = 0;
        assert(std::sscanf(hex + i, "%2x", &value) == 1);
        expected.push_back(uint8_t(value));
    }
    assert(expected.size() == 71);
    assert(encode(sample()) == expected);
    Bytes repaired = expected;
    repairCrc(repaired);
    assert(repaired == expected);
    Pairing decoded{};
    assert(decodePairingRecord(expected.data(), expected.size(), decoded));
    equalFields(sample(), decoded);
}

void roundtripsAndBounds() {
    for (Role role : {Role::Brain, Role::Motion}) {
        for (size_t size = 1; size <= 64; ++size) {
            Pairing pairing = sample();
            pairing.role = role;
            const char alphabet[] = "aZ09_-";
            for (size_t i = 0; i < size; ++i) pairing.deviceId[i] = alphabet[i % 6];
            pairing.deviceId[size] = 0;
            if (role == Role::Motion) {
                std::swap(pairing.localPhysicalId, pairing.peerPhysicalId);
                std::strcpy(pairing.epoch, "fedcba9876543210fedcba9876543210");
            }
            const Bytes bytes = encode(pairing);
            Pairing decoded{};
            assert(decodePairingRecord(bytes.data(), bytes.size(), decoded));
            equalFields(pairing, decoded);
            assert(encode(decoded) == bytes);
            // Exercise every insufficient capacity and an exact-size output.
            for (size_t capacity = 0; capacity < bytes.size(); ++capacity) {
                std::array<uint8_t, 256> out{};
                out.fill(0xc3);
                const auto before = out;
                assert(!encodePairingRecord(pairing, out.data(), capacity));
                assert(out == before);
            }
            Bytes exact(bytes.size());
            assert(encodePairingRecord(pairing, exact.data(), exact.size()) == bytes.size());
            assert(exact == bytes);
            // Unused in-memory bytes after a string terminator are not persisted.
            if (size < 64) {
                std::memset(pairing.deviceId + size + 1, 0xff, 64 - size);
                assert(encode(pairing) == bytes);
            }
        }
    }
    assert(!encodePairingRecord(sample(), nullptr, 256));
    reject(nullptr, 0);
    reject(nullptr, 71);
    reject(nullptr, SIZE_MAX);
}

void corruptionAndLengths() {
    for (size_t size : {size_t(1), size_t(64)}) {
        Pairing pairing = sample();
        std::memset(pairing.deviceId, 'Z', size);
        pairing.deviceId[size] = 0;
        const Bytes original = encode(pairing);
        for (size_t i = 0; i < original.size(); ++i) {
            for (unsigned delta = 1; delta <= 255; ++delta) {
                Bytes bytes = original;
                bytes[i] ^= uint8_t(delta);
                reject(bytes);
            }
        }
        // Exact allocations expose out-of-bounds reads under ASan.
        for (size_t length = 0; length < original.size(); ++length)
            reject(Bytes(original.begin(), original.begin() + length));
        for (size_t length = original.size() + 1; length <= 257; ++length) {
            Bytes bytes = original;
            bytes.resize(length);
            reject(bytes);
        }
        reject(original.data(), SIZE_MAX);
        for (unsigned schema : {0u, 2u, 256u, 65535u}) {
            Bytes bytes = original;
            bytes[4] = uint8_t(schema);
            bytes[5] = uint8_t(schema >> 8);
            rejectWithCrc(bytes);
        }
        for (unsigned payload : {0u, 58u, 123u, 256u, 65535u}) {
            Bytes bytes = original;
            bytes[6] = uint8_t(payload);
            bytes[7] = uint8_t(payload >> 8);
            rejectWithCrc(bytes);
        }
        for (size_t offset = 0; offset < 4; ++offset) {
            Bytes bytes = original;
            bytes[offset] ^= 1;
            rejectWithCrc(bytes);
        }
        for (unsigned length = 0; length <= 255; ++length) {
            if (length == size) continue;
            Bytes bytes = original;
            bytes[13] = uint8_t(length);
            rejectWithCrc(bytes);
        }
    }
}

void invalidIdentities() {
    Pairing pairing = sample();
    std::strcpy(pairing.deviceId, "device-A_9");
    const Bytes original = encode(pairing);
    for (unsigned role = 0; role <= 255; ++role) {
        if (role == 1 || role == 2) continue;
        Pairing bad = pairing;
        bad.role = static_cast<Role>(role);
        rejectEncode(bad);
        Bytes bytes = original;
        bytes[12] = uint8_t(role);
        rejectWithCrc(bytes);
    }
    for (size_t index : {size_t(0), size_t(1), size_t(8)}) {
        for (unsigned value = 0; value <= 255; ++value) {
            const bool alnum = (value >= 'a' && value <= 'z') ||
                (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9');
            if (alnum || (index && (value == '_' || value == '-'))) continue;
            Bytes bytes = original;
            bytes[70 + index] = uint8_t(value);
            rejectWithCrc(bytes);  // Includes embedded/trailing NUL and non-ASCII.
            if (value || !index) {
                Pairing bad = pairing;
                bad.deviceId[index] = char(value);
                rejectEncode(bad);
            }
        }
    }
    Pairing bad = pairing;
    std::memset(bad.deviceId, 'a', sizeof(bad.deviceId));
    rejectEncode(bad);  // 65 bytes without a terminator.

    for (size_t field = 0; field < 3; ++field) {
        const size_t offset = field == 0 ? 14 : (field == 1 ? 46 : 58);
        const size_t size = field == 0 ? 32 : 12;
        for (size_t index = 0; index < size; ++index) {
            for (unsigned value : {0u, unsigned('A'), unsigned('F'), unsigned('g'),
                                   unsigned('/'), unsigned('-'), 0x80u, 0xffu}) {
                Bytes bytes = original;
                bytes[offset + index] = uint8_t(value);
                rejectWithCrc(bytes);
                bad = pairing;
                char* target = field == 0 ? bad.epoch :
                    (field == 1 ? bad.localPhysicalId : bad.peerPhysicalId);
                target[index] = char(value);
                rejectEncode(bad);
            }
        }
        Bytes zero = original;
        std::fill(zero.begin() + offset, zero.begin() + offset + size, '0');
        rejectWithCrc(zero);
        bad = pairing;
        char* target = field == 0 ? bad.epoch :
            (field == 1 ? bad.localPhysicalId : bad.peerPhysicalId);
        std::memset(target, '0', size);
        rejectEncode(bad);
        // Smallest nonzero and maximum hex values remain valid.
        for (char digit : {'1', 'f'}) {
            std::memset(target, digit == '1' ? '0' : 'f', size);
            target[size - 1] = digit;
            Pairing decoded{};
            const Bytes bytes = encode(bad);
            assert(decodePairingRecord(bytes.data(), bytes.size(), decoded));
            equalFields(bad, decoded);
        }
        target[size] = 'f';
        rejectEncode(bad);  // Missing final NUL in the in-memory field.
    }
    bad = pairing;
    std::strcpy(bad.peerPhysicalId, bad.localPhysicalId);
    rejectEncode(bad);
    Bytes same = original;
    std::copy(same.begin() + 46, same.begin() + 58, same.begin() + 58);
    rejectWithCrc(same);
}
}  // namespace

int main() {
    golden();
    roundtripsAndBounds();
    corruptionAndLengths();
    invalidIdentities();
    std::puts("Pairing record: golden layout/CRC, 128 roundtrips, every-byte corruption, "
              "lengths, roles, identities and atomic failures passed");
}
