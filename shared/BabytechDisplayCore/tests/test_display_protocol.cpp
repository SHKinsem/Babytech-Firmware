#include <array>
#include <cassert>
#include <cstring>
#include <iostream>

#include "display_protocol.h"

using namespace babytech::display;

namespace {

DisplaySnapshot sampleSnapshot() {
  DisplaySnapshotInputs inputs;
  inputs.machineStage = DisplayStage::DispensingWater;
  inputs.cloudConnected = true;
  inputs.feedingContextConfigured = true;
  inputs.formulaActive = true;
  inputs.bottleStateValid = true;
  inputs.bottleState = DisplayBottleState::Empty;
  inputs.lowWaterValid = true;
  inputs.powderGrams = 300;
  inputs.requiredPowderGrams = 9;
  inputs.waterMl = 180;
  inputs.temperatureC = 45;
  inputs.thermalSimulated = true;
  inputs.babyName = "Mia";
  inputs.formulaBrand = "Friso";
  return buildDisplaySnapshot(inputs);
}

DisplayFrame parseFrame(const uint8_t* bytes, size_t size) {
  DisplayFrameParser parser;
  DisplayFrame frame;
  bool complete = false;
  for (size_t index = 0; index < size; ++index) {
    if (parser.push(bytes[index], frame)) complete = true;
  }
  assert(complete);
  return frame;
}

void refreshFrameCrc(std::array<uint8_t, kDisplayMaxFrameSize>& bytes,
                     size_t frameSize) {
  const uint16_t crc = displayCrc16(bytes.data() + 2, frameSize - 4);
  bytes[frameSize - 2] = static_cast<uint8_t>(crc & 0xFF);
  bytes[frameSize - 1] = static_cast<uint8_t>(crc >> 8);
}

void testSnapshotRoundTrip() {
  const auto expected = sampleSnapshot();
  std::array<uint8_t, kDisplayMaxPayloadSize> payload{};
  const size_t payloadSize = encodeDisplaySnapshotPayload(
      expected, payload.data(), payload.size());
  assert(payloadSize > 0);

  std::array<uint8_t, kDisplayMaxFrameSize> bytes{};
  const size_t frameSize = encodeDisplayFrame(
      DisplayMessageType::State, 42, payload.data(), payloadSize,
      bytes.data(), bytes.size());
  const DisplayFrame frame = parseFrame(bytes.data(), frameSize);
  assert(frame.sequence == 42);
  DisplaySnapshot actual;
  assert(decodeDisplaySnapshotPayload(frame, actual));
  assert(actual.schemaVersion == 3);
  assert(actual.thermalSimulated);
  assert(displaySnapshotsEqual(expected, actual));
}

void testHeatingConditionRoundTrip() {
  auto expected = sampleSnapshot();
  expected.stage = DisplayStage::Ready;
  expected.footerCondition = DisplayCondition::Heating;

  std::array<uint8_t, kDisplayMaxPayloadSize> payload{};
  const size_t size = encodeDisplaySnapshotPayload(
      expected, payload.data(), payload.size());
  assert(size > 0);

  std::array<uint8_t, kDisplayMaxFrameSize> bytes{};
  const size_t frameSize = encodeDisplayFrame(
      DisplayMessageType::State, 43, payload.data(), size,
      bytes.data(), bytes.size());
  const DisplayFrame frame = parseFrame(bytes.data(), frameSize);
  DisplaySnapshot actual;
  assert(decodeDisplaySnapshotPayload(frame, actual));
  assert(actual.schemaVersion == kDisplaySchemaVersion);
  assert(actual.stage == DisplayStage::Ready);
  assert(actual.footerCondition == DisplayCondition::Heating);
}

void testPowderMotorFaultRoundTrip() {
  auto expected = sampleSnapshot();
  expected.stage = DisplayStage::Error;
  expected.error = DisplayError::PowderMotorFault;
  std::array<uint8_t, kDisplayMaxPayloadSize> payload{};
  const size_t size = encodeDisplaySnapshotPayload(
      expected, payload.data(), payload.size());
  assert(size > 0);
  std::array<uint8_t, kDisplayMaxFrameSize> bytes{};
  const size_t frameSize = encodeDisplayFrame(
      DisplayMessageType::State, 44, payload.data(), size,
      bytes.data(), bytes.size());
  DisplaySnapshot actual;
  assert(decodeDisplaySnapshotPayload(parseFrame(bytes.data(), frameSize),
                                      actual));
  assert(actual.error == DisplayError::PowderMotorFault);
  assert(actual.schemaVersion == kDisplaySchemaVersion);
}

void testIntentAndAckRoundTrip() {
  std::array<uint8_t, kDisplayMaxPayloadSize> payload{};
  std::array<uint8_t, kDisplayMaxFrameSize> bytes{};

  size_t payloadSize = encodeDisplayIntentPayload(
      DisplayIntent::StartFeeding, payload.data(), payload.size());
  size_t frameSize = encodeDisplayFrame(
      DisplayMessageType::Intent, 7, payload.data(), payloadSize,
      bytes.data(), bytes.size());
  auto frame = parseFrame(bytes.data(), frameSize);
  DisplayIntent intent = DisplayIntent::None;
  assert(decodeDisplayIntentPayload(frame, intent));
  assert(intent == DisplayIntent::StartFeeding);

  DisplayAck expected;
  expected.accepted = false;
  setDisplayAckReason(expected, "low_water");
  payloadSize = encodeDisplayAckPayload(expected, payload.data(), payload.size());
  frameSize = encodeDisplayFrame(DisplayMessageType::Ack, 7, payload.data(),
                                 payloadSize, bytes.data(), bytes.size());
  frame = parseFrame(bytes.data(), frameSize);
  DisplayAck actual;
  assert(decodeDisplayAckPayload(frame, actual));
  assert(!actual.accepted);
  assert(std::strcmp(actual.reason.data(), "low_water") == 0);
}

void testParserSkipsNoiseAndRecovers() {
  const uint8_t payload[] = {static_cast<uint8_t>(DisplayIntent::StartFeeding)};
  std::array<uint8_t, kDisplayMaxFrameSize> bytes{};
  const size_t frameSize = encodeDisplayFrame(
      DisplayMessageType::Intent, 9, payload, sizeof(payload), bytes.data(),
      bytes.size());

  DisplayFrameParser parser;
  DisplayFrame frame;
  const uint8_t noise[] = {0x00, 0x42, 0x00, 0xFF};
  for (uint8_t byte : noise) assert(!parser.push(byte, frame));
  bool complete = false;
  for (size_t index = 0; index < frameSize; ++index) {
    complete = parser.push(bytes[index], frame) || complete;
  }
  assert(complete);
  assert(frame.sequence == 9);
}

void testBadCrcAndTruncatedFramesAreRejected() {
  const uint8_t payload[] = {static_cast<uint8_t>(DisplayIntent::StartFeeding)};
  std::array<uint8_t, kDisplayMaxFrameSize> bytes{};
  const size_t frameSize = encodeDisplayFrame(
      DisplayMessageType::Intent, 10, payload, sizeof(payload), bytes.data(),
      bytes.size());

  DisplayFrameParser parser;
  DisplayFrame frame;
  bytes[kDisplayFrameHeaderSize] ^= 0x01;
  for (size_t index = 0; index < frameSize; ++index) {
    assert(!parser.push(bytes[index], frame));
  }

  bytes[kDisplayFrameHeaderSize] ^= 0x01;
  bool recovered = false;
  for (size_t index = 0; index < frameSize; ++index) {
    recovered = parser.push(bytes[index], frame) || recovered;
  }
  assert(recovered);
  assert(frame.sequence == 10);

  parser.reset();
  for (size_t index = 0; index + 1 < frameSize; ++index) {
    assert(!parser.push(bytes[index], frame));
  }
}

void testUnknownMessageTypeIsRejectedAndParserRecovers() {
  const uint8_t payload[] = {static_cast<uint8_t>(DisplayIntent::StartFeeding)};
  std::array<uint8_t, kDisplayMaxFrameSize> bytes{};
  const size_t frameSize = encodeDisplayFrame(
      DisplayMessageType::Intent, 13, payload, sizeof(payload), bytes.data(),
      bytes.size());

  bytes[3] = 0x7F;
  refreshFrameCrc(bytes, frameSize);
  DisplayFrameParser parser;
  DisplayFrame frame;
  for (size_t index = 0; index < frameSize; ++index) {
    assert(!parser.push(bytes[index], frame));
  }

  bytes[3] = static_cast<uint8_t>(DisplayMessageType::Intent);
  refreshFrameCrc(bytes, frameSize);
  bool recovered = false;
  for (size_t index = 0; index < frameSize; ++index) {
    recovered = parser.push(bytes[index], frame) || recovered;
  }
  assert(recovered);
  assert(frame.sequence == 13);
}

void testOversizeHeaderDoesNotPoisonNextFrame() {
  DisplayFrameParser parser;
  DisplayFrame frame;
  const uint8_t oversized[] = {0x42, 0x54, 0x01, 0x01, 0xFF, 0x7F};
  for (uint8_t byte : oversized) assert(!parser.push(byte, frame));

  const uint8_t payload[] = {static_cast<uint8_t>(DisplayIntent::StartFeeding)};
  std::array<uint8_t, kDisplayMaxFrameSize> bytes{};
  const size_t frameSize = encodeDisplayFrame(
      DisplayMessageType::Intent, 11, payload, sizeof(payload), bytes.data(),
      bytes.size());
  bool complete = false;
  for (size_t index = 0; index < frameSize; ++index) {
    complete = parser.push(bytes[index], frame) || complete;
  }
  assert(complete);
  assert(frame.sequence == 11);
}

void testUnsupportedVersionsAndInvalidPayloadsFailClosed() {
  const uint8_t payload[] = {static_cast<uint8_t>(DisplayIntent::StartFeeding)};
  std::array<uint8_t, kDisplayMaxFrameSize> bytes{};
  const size_t frameSize = encodeDisplayFrame(
      DisplayMessageType::Intent, 12, payload, sizeof(payload), bytes.data(),
      bytes.size());
  bytes[2] = 1;
  refreshFrameCrc(bytes, frameSize);
  const DisplayFrame frame = parseFrame(bytes.data(), frameSize);
  DisplayIntent intent = DisplayIntent::None;
  assert(!decodeDisplayIntentPayload(frame, intent));

  DisplayFrame invalid = frame;
  invalid.protocolVersion = kDisplayProtocolVersion;
  invalid.payload[0] = 0xFF;
  assert(!decodeDisplayIntentPayload(invalid, intent));
}

void testInvalidSnapshotFieldsFailClosed() {
  const auto snapshot = sampleSnapshot();
  std::array<uint8_t, kDisplayMaxPayloadSize> payload{};
  const size_t payloadSize = encodeDisplaySnapshotPayload(
      snapshot, payload.data(), payload.size());
  std::array<uint8_t, kDisplayMaxFrameSize> bytes{};
  const size_t frameSize = encodeDisplayFrame(
      DisplayMessageType::State, 14, payload.data(), payloadSize,
      bytes.data(), bytes.size());
  DisplayFrame frame = parseFrame(bytes.data(), frameSize);
  DisplaySnapshot decoded;

  frame.payload[2] = 0xFF;
  assert(!decodeDisplaySnapshotPayload(frame, decoded));
  frame.payload[2] = static_cast<uint8_t>(snapshot.stage);

  frame.payload[6] = 0x80;
  assert(!decodeDisplaySnapshotPayload(frame, decoded));
  frame.payload[6] = 0;

  frame.payload[0] = 1;
  frame.payload[1] = 0;
  assert(!decodeDisplaySnapshotPayload(frame, decoded));
}

void testOutputCapacityAndNullInputsAreChecked() {
  std::array<uint8_t, 4> small{};
  assert(encodeDisplayFrame(DisplayMessageType::Intent, 1, nullptr, 1,
                            small.data(), small.size()) == 0);
  assert(encodeDisplayIntentPayload(DisplayIntent::None, small.data(),
                                    small.size()) == 0);
  assert(encodeDisplaySnapshotPayload(sampleSnapshot(), small.data(),
                                      small.size()) == 0);
}

}  // namespace

int main() {
  testSnapshotRoundTrip();
  testHeatingConditionRoundTrip();
  testPowderMotorFaultRoundTrip();
  testIntentAndAckRoundTrip();
  testParserSkipsNoiseAndRecovers();
  testBadCrcAndTruncatedFramesAreRejected();
  testUnknownMessageTypeIsRejectedAndParserRecovers();
  testOversizeHeaderDoesNotPoisonNextFrame();
  testUnsupportedVersionsAndInvalidPayloadsFailClosed();
  testInvalidSnapshotFieldsFailClosed();
  testOutputCapacityAndNullInputsAreChecked();
  std::cout << "display protocol tests passed\n";
  return 0;
}
