#pragma once
#include <stddef.h>
#include <stdint.h>

namespace babytech {

constexpr uint8_t kVersion = 1;
constexpr size_t kMaxPayload = 32;
constexpr size_t kHeaderSize = 10;
constexpr size_t kMaxFrameSize = kHeaderSize + kMaxPayload + 2;
constexpr uint32_t kByteTimeoutMs = 100;

enum class Type : uint8_t { GetStatus = 1, Status = 2 };
enum class MotionState : uint8_t { NotConfigured = 0 };

struct Frame {
    Type type = Type::GetStatus;
    uint32_t sequence = 0;
    uint16_t length = 0;
    uint8_t payload[kMaxPayload]{};
};

struct Status {
    uint32_t uptimeMs = 0;
    MotionState state = MotionState::NotConfigured;
    bool motorsAvailable = false;
    bool sensorsAvailable = false;
};

uint16_t crc16(const uint8_t* data, size_t length);
size_t encode(Type type, uint32_t sequence, const uint8_t* payload,
              size_t length, uint8_t* output, size_t capacity);
bool readStatus(const Frame& frame, Status& status);

// First slice endpoint. No motor or sensor drivers are integrated yet.
size_t respondToStatusQuery(const Frame& request, uint32_t uptimeMs,
                           uint8_t* output, size_t capacity);

class Parser {
public:
    bool push(uint8_t byte, Frame& frame);
    void reset() { size_ = 0; }
private:
    uint8_t buffer_[kMaxFrameSize]{};
    size_t size_ = 0;
};

}  // namespace babytech
