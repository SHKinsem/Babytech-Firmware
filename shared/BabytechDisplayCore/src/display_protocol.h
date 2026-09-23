#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "display_model.h"

namespace babytech::display {

constexpr uint8_t kDisplayProtocolVersion = 3;
constexpr size_t kDisplayAckReasonCapacity = 32;
constexpr size_t kDisplayMaxPayloadSize = 96;
constexpr size_t kDisplayFrameHeaderSize = 10;
constexpr size_t kDisplayFrameCrcSize = 2;
constexpr size_t kDisplayMaxFrameSize =
    kDisplayFrameHeaderSize + kDisplayMaxPayloadSize + kDisplayFrameCrcSize;

enum class DisplayMessageType : uint8_t {
  State = 1,
  Intent = 2,
  Ack = 3,
};

struct DisplayAck {
  bool accepted = false;
  std::array<char, kDisplayAckReasonCapacity> reason{};
};

struct DisplayFrame {
  uint8_t protocolVersion = 0;
  DisplayMessageType type = DisplayMessageType::State;
  uint32_t sequence = 0;
  uint16_t payloadLength = 0;
  std::array<uint8_t, kDisplayMaxPayloadSize> payload{};
};

uint16_t displayCrc16(const uint8_t* data, size_t size);

size_t encodeDisplayFrame(DisplayMessageType type, uint32_t sequence,
                          const uint8_t* payload, size_t payloadLength,
                          uint8_t* output, size_t outputCapacity);

class DisplayFrameParser {
 public:
  bool push(uint8_t byte, DisplayFrame& frame);
  void reset();

 private:
  size_t expectedFrameSize() const;
  bool decode(DisplayFrame& frame) const;

  std::array<uint8_t, kDisplayMaxFrameSize> buffer_{};
  size_t size_ = 0;
  size_t expectedSize_ = 0;
};

size_t encodeDisplaySnapshotPayload(const DisplaySnapshot& snapshot,
                                    uint8_t* output,
                                    size_t outputCapacity);
bool decodeDisplaySnapshotPayload(const DisplayFrame& frame,
                                  DisplaySnapshot& snapshot);

size_t encodeDisplayIntentPayload(DisplayIntent intent, uint8_t* output,
                                  size_t outputCapacity);
bool decodeDisplayIntentPayload(const DisplayFrame& frame,
                                DisplayIntent& intent);

size_t encodeDisplayAckPayload(const DisplayAck& ack, uint8_t* output,
                               size_t outputCapacity);
bool decodeDisplayAckPayload(const DisplayFrame& frame, DisplayAck& ack);

void setDisplayAckReason(DisplayAck& ack, const char* reason);

}  // namespace babytech::display
