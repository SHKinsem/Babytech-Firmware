#pragma once

#include "ProductRequest.h"

namespace babytech { namespace boardlink {

struct MotionContextBarrier {
    bool present = false;
    uint32_t profileVersion = 0;
    bool cleared = false;
    uint8_t digest[kProductDigestSize] = {};
    char babyId[97] = {};
    float powderGPer100Ml = 0;
};

enum class MotionResultKind : uint8_t { None = 0, Ordinary = 1, CloudStop = 2 };
enum class MotionOutcome : uint8_t { None = 0, Succeeded = 1, Interrupted = 2, Failed = 3 };

struct MotionResult {
    MotionResultKind kind = MotionResultKind::None;
    ProductRequest request;
    uint8_t digest[kProductDigestSize] = {};
    // Cloud Stop is not an ordinary request. Its sequence is still Cloud-issued.
    uint64_t stopSequence = 0;
    char stopCommandId[129] = {};
    // Empty targets the observed idle state, never an arbitrary active run.
    char stopExecutionId[33] = {};
    bool accepted = false;
    char reason[65] = {};
    // Execution outcome never changes the original accepted/reason response.
    MotionOutcome outcome = MotionOutcome::None;
};

enum class MotionSlotKind : uint8_t { Empty = 0, Intent = 1, Terminal = 2 };
struct MotionExecutionSlot {
    MotionSlotKind kind = MotionSlotKind::Empty;
    ProductRequest request;
    uint8_t digest[kProductDigestSize] = {};
    char executionId[33] = {};
    char eventId[59] = {};
    float targetPowderG = 0;
    bool completed = false;
    uint32_t uptimeMs = 0;
    char reason[65] = {};
    char errorCode[65] = {};
};

// Capacity includes a reserved result for an in-flight prepare.
constexpr size_t kMotionResultQueueCapacity = 4;

struct MotionState {
    v4::Pairing pairing;
    MotionContextBarrier context;
    uint64_t cloudSequence = 0;
    uint64_t localSequence = 0;
    MotionResult cloudResult;
    MotionResult localResult;
    MotionExecutionSlot slot;
    uint8_t pendingResultCount = 0;
    MotionExecutionSlot pendingResults[kMotionResultQueueCapacity];
};

// Conservative ordinary-result bounds also cover the shorter Cloud Stop form.
constexpr size_t kMotionResultMaxSize = 1 + 2 + kRequestIdentityMaxSize + 32 + 1 + 2 + 64 + 1;
constexpr size_t kMotionSlotMaxSize = 1 + 2 + kRequestIdentityMaxSize + 32 + 2 + 32 +
    2 + 58 + 4 + 1 + 4 + 2 + 64 + 2 + 64;
constexpr size_t kMotionStateV1MaxSize = 12 + 2 + 134 + 1 + 4 + 1 + 32 + 2 + 96 + 4 +
    8 + 8 + 2 * kMotionResultMaxSize + kMotionSlotMaxSize;
constexpr size_t kMotionStateMaxSize = kMotionStateV1MaxSize + 1 +
    kMotionResultQueueCapacity * kMotionSlotMaxSize;
static_assert(kMotionStateMaxSize <= 4096, "Motion NVS record budget");

bool makeMotionContextBarrier(const ProductContext& context, MotionContextBarrier& output);
bool sameMotionContextBarrier(const MotionContextBarrier& left, const MotionContextBarrier& right);
bool makeProductEventId(const v4::Pairing& pairing, v4::Source source, uint64_t sequence,
                        char (&output)[59]);
bool validExecutionId(const char (&value)[33]);
// Preserve ProductSession's existing one-decimal-gram rounding.
float productTargetPowderG(const ProductRequest& request);
bool validMotionState(const MotionState& state);
bool sameMotionState(const MotionState& left, const MotionState& right);
size_t encodeMotionState(const MotionState& state, uint8_t* output, size_t capacity);
bool decodeMotionState(const uint8_t* bytes, size_t length, MotionState& output);

} }
