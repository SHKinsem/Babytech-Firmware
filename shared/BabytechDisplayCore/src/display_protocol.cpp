#include "display_protocol.h"

#include <algorithm>
#include <cstring>

namespace babytech::display {
namespace {

constexpr uint8_t kMagic0 = 0x42;
constexpr uint8_t kMagic1 = 0x54;
constexpr size_t kDisplaySnapshotPayloadSize =
    2 + 4 + 1 + 2 + 2 + kDisplayBabyNameCapacity +
    kDisplayFormulaBrandCapacity;
static_assert(kDisplaySnapshotPayloadSize <= kDisplayMaxPayloadSize);

void writeU16(uint8_t* output, uint16_t value) {
  output[0] = static_cast<uint8_t>(value & 0xFF);
  output[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void writeU32(uint8_t* output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value & 0xFF);
  output[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  output[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  output[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

uint16_t readU16(const uint8_t* input) {
  return static_cast<uint16_t>(input[0]) |
         (static_cast<uint16_t>(input[1]) << 8);
}

uint32_t readU32(const uint8_t* input) {
  return static_cast<uint32_t>(input[0]) |
         (static_cast<uint32_t>(input[1]) << 8) |
         (static_cast<uint32_t>(input[2]) << 16) |
         (static_cast<uint32_t>(input[3]) << 24);
}

bool validMessageType(uint8_t raw) {
  return raw >= static_cast<uint8_t>(DisplayMessageType::State) &&
         raw <= static_cast<uint8_t>(DisplayMessageType::Ack);
}

bool validStage(uint8_t raw) {
  return raw <= static_cast<uint8_t>(DisplayStage::Unknown);
}

bool validCondition(uint8_t raw) {
  return raw <= static_cast<uint8_t>(DisplayCondition::Ready);
}

bool validError(uint8_t raw) {
  return raw <= static_cast<uint8_t>(DisplayError::PowderMotorFault);
}

bool validIntent(uint8_t raw) {
  return raw <= static_cast<uint8_t>(DisplayIntent::Initialize);
}

template <size_t Size>
void copyFixedText(std::array<char, Size>& output, const uint8_t* input) {
  std::memcpy(output.data(), input, Size);
  output.back() = '\0';
}

}  // namespace

uint16_t displayCrc16(const uint8_t* data, size_t size) {
  uint16_t crc = 0xFFFF;
  for (size_t index = 0; index < size; ++index) {
    crc ^= static_cast<uint16_t>(data[index]) << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) != 0
          ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
          : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

size_t encodeDisplayFrame(DisplayMessageType type, uint32_t sequence,
                          const uint8_t* payload, size_t payloadLength,
                          uint8_t* output, size_t outputCapacity) {
  if (output == nullptr || payloadLength > kDisplayMaxPayloadSize ||
      (payloadLength > 0 && payload == nullptr)) {
    return 0;
  }
  const size_t frameSize =
      kDisplayFrameHeaderSize + payloadLength + kDisplayFrameCrcSize;
  if (outputCapacity < frameSize) return 0;

  output[0] = kMagic0;
  output[1] = kMagic1;
  output[2] = kDisplayProtocolVersion;
  output[3] = static_cast<uint8_t>(type);
  writeU16(output + 4, static_cast<uint16_t>(payloadLength));
  writeU32(output + 6, sequence);
  if (payloadLength > 0) {
    std::memcpy(output + kDisplayFrameHeaderSize, payload, payloadLength);
  }
  const uint16_t crc = displayCrc16(
      output + 2, kDisplayFrameHeaderSize - 2 + payloadLength);
  writeU16(output + kDisplayFrameHeaderSize + payloadLength, crc);
  return frameSize;
}

bool DisplayFrameParser::push(uint8_t byte, DisplayFrame& frame) {
  if (size_ == 0) {
    if (byte == kMagic0) buffer_[size_++] = byte;
    return false;
  }
  if (size_ == 1) {
    if (byte == kMagic1) {
      buffer_[size_++] = byte;
    } else if (byte != kMagic0) {
      reset();
    }
    return false;
  }

  if (size_ >= buffer_.size()) {
    reset();
    if (byte == kMagic0) buffer_[size_++] = byte;
    return false;
  }
  buffer_[size_++] = byte;

  if (size_ == 6) {
    const uint16_t payloadLength = readU16(buffer_.data() + 4);
    if (payloadLength > kDisplayMaxPayloadSize) {
      reset();
      return false;
    }
    expectedSize_ =
        kDisplayFrameHeaderSize + payloadLength + kDisplayFrameCrcSize;
  }

  if (expectedSize_ == 0 || size_ < expectedSize_) return false;
  const bool valid = decode(frame);
  reset();
  return valid;
}

void DisplayFrameParser::reset() {
  size_ = 0;
  expectedSize_ = 0;
}

size_t DisplayFrameParser::expectedFrameSize() const {
  return expectedSize_;
}

bool DisplayFrameParser::decode(DisplayFrame& frame) const {
  if (expectedFrameSize() == 0 || size_ != expectedFrameSize()) return false;
  const uint8_t rawType = buffer_[3];
  if (!validMessageType(rawType)) return false;
  const uint16_t payloadLength = readU16(buffer_.data() + 4);
  const uint16_t expectedCrc =
      readU16(buffer_.data() + kDisplayFrameHeaderSize + payloadLength);
  const uint16_t actualCrc = displayCrc16(
      buffer_.data() + 2, kDisplayFrameHeaderSize - 2 + payloadLength);
  if (actualCrc != expectedCrc) return false;

  frame.protocolVersion = buffer_[2];
  frame.type = static_cast<DisplayMessageType>(rawType);
  frame.sequence = readU32(buffer_.data() + 6);
  frame.payloadLength = payloadLength;
  frame.payload.fill(0);
  if (payloadLength > 0) {
    std::memcpy(frame.payload.data(),
                buffer_.data() + kDisplayFrameHeaderSize, payloadLength);
  }
  return true;
}

size_t encodeDisplaySnapshotPayload(const DisplaySnapshot& snapshot,
                                    uint8_t* output,
                                    size_t outputCapacity) {
  if (output == nullptr || outputCapacity < kDisplaySnapshotPayloadSize) {
    return 0;
  }
  size_t offset = 0;
  writeU16(output + offset, snapshot.schemaVersion);
  offset += 2;
  output[offset++] = static_cast<uint8_t>(snapshot.stage);
  output[offset++] = static_cast<uint8_t>(snapshot.primaryCondition);
  output[offset++] = static_cast<uint8_t>(snapshot.footerCondition);
  output[offset++] = static_cast<uint8_t>(snapshot.error);
  output[offset++] = static_cast<uint8_t>(
      (snapshot.cloudConnected ? 0x01 : 0x00) |
      (snapshot.startEnabled ? 0x02 : 0x00) |
      (snapshot.thermalSimulated ? 0x04 : 0x00));
  writeU16(output + offset, snapshot.waterMl);
  offset += 2;
  writeU16(output + offset, static_cast<uint16_t>(snapshot.temperatureC));
  offset += 2;
  std::memcpy(output + offset, snapshot.babyName.data(),
              snapshot.babyName.size());
  offset += snapshot.babyName.size();
  std::memcpy(output + offset, snapshot.formulaBrand.data(),
              snapshot.formulaBrand.size());
  offset += snapshot.formulaBrand.size();
  return offset;
}

bool decodeDisplaySnapshotPayload(const DisplayFrame& frame,
                                  DisplaySnapshot& snapshot) {
  if (frame.protocolVersion != kDisplayProtocolVersion ||
      frame.type != DisplayMessageType::State ||
      frame.payloadLength != kDisplaySnapshotPayloadSize) {
    return false;
  }
  const uint8_t* input = frame.payload.data();
  const uint8_t rawStage = input[2];
  const uint8_t rawPrimary = input[3];
  const uint8_t rawFooter = input[4];
  const uint8_t rawError = input[5];
  const uint8_t flags = input[6];
  if (!validStage(rawStage) || !validCondition(rawPrimary) ||
      !validCondition(rawFooter) || !validError(rawError) ||
      (flags & 0xF8) != 0) {
    return false;
  }

  snapshot = DisplaySnapshot{};
  snapshot.schemaVersion = readU16(input);
  if (snapshot.schemaVersion != kDisplaySchemaVersion) return false;
  snapshot.stage = static_cast<DisplayStage>(rawStage);
  snapshot.primaryCondition = static_cast<DisplayCondition>(rawPrimary);
  snapshot.footerCondition = static_cast<DisplayCondition>(rawFooter);
  snapshot.error = static_cast<DisplayError>(rawError);
  snapshot.cloudConnected = (flags & 0x01) != 0;
  snapshot.startEnabled = (flags & 0x02) != 0;
  snapshot.thermalSimulated = (flags & 0x04) != 0;
  snapshot.waterMl = readU16(input + 7);
  snapshot.temperatureC = static_cast<int16_t>(readU16(input + 9));
  copyFixedText(snapshot.babyName, input + 11);
  copyFixedText(snapshot.formulaBrand,
                input + 11 + kDisplayBabyNameCapacity);
  return true;
}

size_t encodeDisplayIntentPayload(DisplayIntent intent, uint8_t* output,
                                  size_t outputCapacity) {
  if (output == nullptr || outputCapacity < 1 || intent == DisplayIntent::None ||
      !validIntent(static_cast<uint8_t>(intent))) {
    return 0;
  }
  output[0] = static_cast<uint8_t>(intent);
  return 1;
}

bool decodeDisplayIntentPayload(const DisplayFrame& frame,
                                DisplayIntent& intent) {
  if (frame.protocolVersion != kDisplayProtocolVersion ||
      frame.type != DisplayMessageType::Intent || frame.payloadLength != 1 ||
      !validIntent(frame.payload[0]) ||
      frame.payload[0] == static_cast<uint8_t>(DisplayIntent::None)) {
    return false;
  }
  intent = static_cast<DisplayIntent>(frame.payload[0]);
  return true;
}

size_t encodeDisplayAckPayload(const DisplayAck& ack, uint8_t* output,
                               size_t outputCapacity) {
  constexpr size_t kPayloadSize = 1 + kDisplayAckReasonCapacity;
  if (output == nullptr || outputCapacity < kPayloadSize) return 0;
  output[0] = ack.accepted ? 1 : 0;
  std::memcpy(output + 1, ack.reason.data(), ack.reason.size());
  return kPayloadSize;
}

bool decodeDisplayAckPayload(const DisplayFrame& frame, DisplayAck& ack) {
  constexpr size_t kPayloadSize = 1 + kDisplayAckReasonCapacity;
  if (frame.protocolVersion != kDisplayProtocolVersion ||
      frame.type != DisplayMessageType::Ack ||
      frame.payloadLength != kPayloadSize || frame.payload[0] > 1) {
    return false;
  }
  ack = DisplayAck{};
  ack.accepted = frame.payload[0] == 1;
  copyFixedText(ack.reason, frame.payload.data() + 1);
  return true;
}

void setDisplayAckReason(DisplayAck& ack, const char* reason) {
  ack.reason.fill('\0');
  if (reason == nullptr) return;
  std::strncpy(ack.reason.data(), reason, ack.reason.size() - 1);
}

}  // namespace babytech::display
