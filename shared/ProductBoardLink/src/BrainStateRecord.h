#pragma once

#include "ProductRequest.h"

namespace babytech { namespace boardlink {

struct BrainState {
    v4::Pairing pairing;
    bool hasContext = false;
    ProductContext context;
    uint64_t localSequence = 0;
    bool pending = false;
    ProductRequest pendingRequest;
    uint8_t pendingDigest[kProductDigestSize] = {};
};

// BBS1 envelope + paired identity + full context + one local request/digest.
// Upper bound includes the full request ID capacity, even though local IDs are shorter.
constexpr size_t kBrainStateMaxSize = 12 + 2 + 134 + 1 + 2 + kContextIdentityMaxSize +
                                      8 + 1 + 2 + kRequestIdentityMaxSize + kProductDigestSize;
static_assert(kBrainStateMaxSize <= 4096, "Brain NVS record budget");
bool validBrainState(const BrainState& state);
bool sameBrainState(const BrainState& left, const BrainState& right);
size_t encodeBrainState(const BrainState& state, uint8_t* output, size_t capacity);
bool decodeBrainState(const uint8_t* bytes, size_t length, BrainState& output);

} }
