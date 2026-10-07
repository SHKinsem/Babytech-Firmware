#pragma once

#include <stddef.h>
#include <stdint.h>

namespace babytech { namespace v4 {

constexpr size_t kHeaderSize = 34;
constexpr size_t kMaxFragment = 160;
constexpr size_t kMaxMessage = 2047;
constexpr size_t kMaxFrame = kHeaderSize + kMaxFragment + 2;
constexpr uint32_t kByteTimeoutMs = 100;
constexpr uint32_t kMessageTimeoutMs = 1000;
constexpr uint32_t kHeartbeatIntervalMs = 250;
constexpr uint32_t kLinkTimeoutMs = 1500;
constexpr uint32_t kStatusIntervalMs = 500;
constexpr uint64_t kMaxSequence = UINT64_C(9223372036854775807);

enum class Kind : uint8_t {
    Hello = 1, HelloAck, Heartbeat, StatusQuery, Status, Context, ContextResult,
    Command, CommandResult, ResultQuery, Result, Terminal, CloudReceipt,
    LinkAck, LinkReject, Stop, Discovery, MigrationRead, MigrationMaintenance
};

struct Frame {
    Kind kind = Kind::Heartbeat;
    uint64_t senderBoot = 0;
    uint64_t receiverBoot = 0;
    uint32_t messageId = 0;
    uint16_t total = 0;
    uint16_t offset = 0;
    uint16_t length = 0;
    uint8_t payload[kMaxFragment]{};
};

struct Message {
    Kind kind = Kind::Heartbeat;
    uint64_t senderBoot = 0;
    uint64_t receiverBoot = 0;
    uint32_t messageId = 0;
    uint16_t length = 0;
    uint8_t payload[kMaxMessage]{};
};

bool isControl(Kind kind);
bool validFrame(const Frame& frame);
size_t encode(const Frame& frame, uint8_t* output, size_t capacity);
bool fragment(const Message& message, size_t offset, Frame& result);

class Parser {
public:
    bool push(uint8_t byte, uint32_t nowMs, Frame& result);
    void reset();
private:
    void discard(size_t count);
    uint8_t bytes_[kMaxFrame]{};
    size_t size_ = 0;
    uint32_t lastByteAt_ = 0;
};

enum class AssemblyResult { Incomplete, Complete, DuplicateFragment, Busy, Invalid };

// Session authorization and whole-message deduplication belong to the endpoint.
class Assembler {
public:
    AssemblyResult accept(const Frame& frame, uint32_t nowMs, Message& result);
    bool expire(uint32_t nowMs);
    void reset();
    bool active() const { return active_; }
private:
    Message pending_{};
    uint16_t received_ = 0;
    uint32_t startedAt_ = 0;
    bool active_ = false;
};

enum class Source : uint8_t { CloudCommand = 1, LocalTouch = 2 };
enum class StopScope : uint8_t { Product = 1, Workbench = 2, Idle = 3 };
struct StopRequest {
    Source source = Source::LocalTouch;
    uint64_t sequence = 0;
    StopScope scope = StopScope::Idle;
    uint8_t executionId[16]{};
    uint8_t commandIdLength = 0;
    char commandId[129]{};
};

size_t encodeStop(const StopRequest& request, uint8_t* output, size_t capacity);
bool decodeStop(const uint8_t* payload, size_t size, StopRequest& result);
bool validUtf8(const uint8_t* bytes, size_t size);

} }
