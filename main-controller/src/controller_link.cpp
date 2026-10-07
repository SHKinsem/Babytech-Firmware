#include "controller_link.h"

#if !BABYTECH_BOARD_LINK_V4
#include <esp_system.h>
#endif

#include "display_board_profile.h"

namespace babytech::display {

#if BABYTECH_BOARD_LINK_V4

bool ControllerLink::begin() {
  hasSnapshot_ = false;
  telemetrySeen_ = false;
  lastTelemetry_ = babytech::boardlink::Status{};
  lastTelemetryReceivedAtMs_ = 0;
  snapshot_ = DisplaySnapshot{};
  snapshot_.stage = DisplayStage::NotReady;
  ready_ = boardLink_.begin(babytech::v4::Role::Brain,
                           kControllerUartRxPin, kControllerUartTxPin,
                           kControllerUartBaud, true);
  return ready_;
}

void ControllerLink::poll(uint32_t nowMs) {
  if (!ready_) return;
  boardLink_.poll(nowMs);
  const auto& link = boardLink_.link();
  hasSnapshot_ = link.healthy() && link.connected(nowMs) &&
                 link.freshStatus(nowMs);
  if (hasSnapshot_) {
    lastTelemetry_ = link.peerStatus();
    lastTelemetryReceivedAtMs_ = link.peerStatusReceivedAtMs();
    telemetrySeen_ = true;
    snapshot_ = link.peerStatus().snapshot;
  } else {
    snapshot_ = DisplaySnapshot{};
    snapshot_.stage = DisplayStage::NotReady;
    snapshot_.primaryCondition = DisplayCondition::ControllerOffline;
  }
  snapshot_.startEnabled = false;
}

bool ControllerLink::connected(uint32_t nowMs) const {
  const auto& link = boardLink_.link();
  return ready_ && hasSnapshot_ && link.healthy() && link.connected(nowMs) &&
         link.freshStatus(nowMs);
}

bool ControllerLink::protocolIncompatible(uint32_t) const { return false; }

bool ControllerLink::sendIntent(DisplayIntent, uint32_t) { return false; }

#else

bool ControllerLink::begin() {
  if (kControllerUartTxPin < 0 || kControllerUartRxPin < 0 ||
      kControllerUartTxPin == kControllerUartRxPin) {
    Serial.println(
        "[ControllerLink] UART pins are not configured; display stays offline");
    return false;
  }
  serial_.begin(kControllerUartBaud, SERIAL_8N1, kControllerUartRxPin,
                kControllerUartTxPin);
  nextSequence_ = esp_random();
  if (nextSequence_ == 0) nextSequence_ = 1;
  ready_ = true;
  Serial.printf("[ControllerLink] UART ready, TX=%d RX=%d\n",
                kControllerUartTxPin, kControllerUartRxPin);
  return true;
}

void ControllerLink::poll(uint32_t nowMs) {
  if (!ready_) return;
  while (serial_.available() > 0) {
    DisplayFrame frame;
    if (parser_.push(static_cast<uint8_t>(serial_.read()), frame)) {
      handleFrame(frame, nowMs);
    }
  }

  if (!pendingIntent_) return;
  if (nowMs - lastIntentSentAtMs_ < kIntentRetryIntervalMs) return;
  if (intentRetries_ < kIntentRetryLimit) {
    ++intentRetries_;
    sendPendingIntent(nowMs);
  } else if (nowMs - lastIntentSentAtMs_ >= kControllerOfflineTimeoutMs) {
    Serial.println("[ControllerLink] Intent ACK timeout");
    pendingIntent_ = false;
    pendingIntentValue_ = DisplayIntent::None;
  }
}

bool ControllerLink::connected(uint32_t nowMs) const {
  return ready_ && hasSnapshot_ && !protocolIncompatible_ &&
         nowMs - lastStateAtMs_ <= kControllerOfflineTimeoutMs;
}

bool ControllerLink::protocolIncompatible(uint32_t nowMs) const {
  return ready_ && protocolIncompatible_ &&
         nowMs - lastProtocolMismatchAtMs_ <= kControllerOfflineTimeoutMs;
}

bool ControllerLink::sendIntent(DisplayIntent intent, uint32_t nowMs) {
  if (!connected(nowMs) || pendingIntent_ || intent == DisplayIntent::None) {
    return false;
  }
  pendingIntent_ = true;
  pendingIntentValue_ = intent;
  pendingSequence_ = nextSequence_++;
  intentRetries_ = 0;
  if (!sendPendingIntent(nowMs)) {
    pendingIntent_ = false;
    pendingIntentValue_ = DisplayIntent::None;
    return false;
  }
  return true;
}

void ControllerLink::handleFrame(const DisplayFrame& frame, uint32_t nowMs) {
  if (frame.protocolVersion != kDisplayProtocolVersion) {
    protocolIncompatible_ = true;
    lastProtocolMismatchAtMs_ = nowMs;
    return;
  }

  if (frame.type == DisplayMessageType::State) {
    DisplaySnapshot received;
    if (!decodeDisplaySnapshotPayload(frame, received)) return;
    snapshot_ = received;
    hasSnapshot_ = true;
    protocolIncompatible_ = false;
    lastStateAtMs_ = nowMs;
    return;
  }

  if (frame.type == DisplayMessageType::Ack && pendingIntent_ &&
      frame.sequence == pendingSequence_) {
    DisplayAck ack;
    if (!decodeDisplayAckPayload(frame, ack)) return;
    Serial.printf("[ControllerLink] Intent %s: %s%s%s\n",
                  displayIntentKey(pendingIntentValue_),
                  ack.accepted ? "accepted" : "rejected",
                  ack.reason[0] == '\0' ? "" : ", ", ack.reason.data());
    pendingIntent_ = false;
    pendingIntentValue_ = DisplayIntent::None;
  }
}

bool ControllerLink::sendPendingIntent(uint32_t nowMs) {
  uint8_t payload[kDisplayMaxPayloadSize] = {};
  const size_t payloadSize = encodeDisplayIntentPayload(
      pendingIntentValue_, payload, sizeof(payload));
  if (payloadSize == 0 ||
      !sendFrame(DisplayMessageType::Intent, pendingSequence_, payload,
                 payloadSize)) {
    return false;
  }
  lastIntentSentAtMs_ = nowMs;
  return true;
}

bool ControllerLink::sendFrame(DisplayMessageType type, uint32_t sequence,
                               const uint8_t* payload, size_t payloadSize) {
  uint8_t frame[kDisplayMaxFrameSize] = {};
  const size_t frameSize = encodeDisplayFrame(
      type, sequence, payload, payloadSize, frame, sizeof(frame));
  return frameSize > 0 && serial_.write(frame, frameSize) == frameSize;
}

#endif

}  // namespace babytech::display
