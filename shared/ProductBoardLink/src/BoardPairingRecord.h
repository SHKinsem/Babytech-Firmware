#pragma once

#include "BoardSessionV4.h"

namespace babytech { namespace boardlink {

constexpr size_t kPairingRecordMaxSize = 256;

// B1.1 productpair/record schema 1, independent of C++ layout and endianness.
// Offsets (bytes), all integer fields unsigned and little-endian:
//   0..3    magic = 0x31505442 (ASCII "BTP1")
//   4..5    schema = 1
//   6..7    payload_length = 58 + D (excludes the 12-byte header)
//   8..11   CRC32/ISO-HDLC: poly 0x04c11db7, reflected (0xedb88320),
//           init/xorout 0xffffffff; covers [0,8) then [12,record_length).
//   12      local role: Brain=1, Motion=2
//   13      device ID byte length D, 1..64
//   14..45  pairing epoch, exactly 32 lowercase ASCII hex bytes, nonzero
//   46..57  local physical ID, 12 lowercase ASCII hex bytes, nonzero
//   58..69  peer physical ID, 12 lowercase ASCII hex bytes, nonzero
//   70..    device ID, D ASCII bytes: [A-Za-z0-9][A-Za-z0-9_-]{0,63}
// Physical IDs must differ. Strings have NO stored NULs or padding. Total
// length is exactly 70+D (71..134), with no trailing bytes/extensions allowed.
// CRC detects corruption, not malicious rewriting/authenticity. Identity
// validation uses v4::validPairing; matching role/local ID to actual hardware
// remains the adapter's responsibility. Missing/I/O errors are not codec states.
// No storage, provisioning, migration, fallback identity or Flash writes here.
//
// Encode returns bytes written, or 0 with the entire output buffer unchanged.
// Decode returns false with out unchanged, including on null data. Success
// yields NUL-terminated fields. Inputs must remain stable during each call.
size_t encodePairingRecord(const v4::Pairing& pairing, uint8_t* out, size_t capacity);
bool decodePairingRecord(const uint8_t* data, size_t length, v4::Pairing& out);

} }  // namespace babytech::boardlink
