#pragma once

#include <Arduino.h>

#include "brain_mode.h"

#include "display_model.h"
#if BABYTECH_BOARD_LINK_V4
#include "BoardLinkArduino.h"
#else
#include "display_protocol.h"
#endif

namespace babytech::display {

class ControllerLink {
 public:
  bool begin();
  void poll(uint32_t nowMs);

  bool connected(uint32_t nowMs) const;
  bool protocolIncompatible(uint32_t nowMs) const;
  bool hasSnapshot() const { return hasSnapshot_; }
  const DisplaySnapshot& snapshot() const { return snapshot_; }
#if BABYTECH_BOARD_LINK_V4
  const char* deviceId() const { return ready_ ? boardLink_.deviceId() : nullptr; }
  // Pairing was checked against the actual MAC during begin. A subsequent
  // UART failure must not hide it from the local read-only recovery owner.
  const babytech::v4::Pairing* verifiedPairing() const { return boardLink_.verifiedPairing(); }
  bool requestDiscovery(const char* deviceId, uint32_t nowMs) {
    return ready_ && boardLink_.requestDiscovery(deviceId, nowMs);
  }
  const babytech::boardlink::DiscoveryResult& discoveryResult() const {
    return boardLink_.discoveryResult();
  }
  bool requestRecords(const char* device, uint32_t nowMs) {
    return ready_ && boardLink_.requestRecords(device, nowMs);
  }
  babytech::boardlink::ExportTransferState recordsState() const { return boardLink_.recordsState(); }
  bool requestInstalledRecords(const char* device, const babytech::v4::Pairing& expected, uint32_t nowMs) {
    return ready_ && boardLink_.requestInstalledRecords(device, expected, nowMs);
  }
  bool requestRecoveryRecords(const char* device, const babytech::v4::Pairing& expected, uint32_t nowMs) {
    return ready_ && boardLink_.requestRecoveryRecords(device, expected, nowMs);
  }
  const babytech::boardlink::MotionExportSnapshot* recordsSnapshot() const { return boardLink_.recordsSnapshot(); }
  bool requestMaintenance(const char* device, uint32_t nowMs) {
    return ready_ && boardLink_.requestMaintenance(device, nowMs);
  }
  bool releaseMaintenance(uint32_t nowMs) {
    return ready_ && boardLink_.releaseMaintenance(nowMs);
  }
  babytech::boardlink::BoardMaintenanceState maintenanceState() const { return boardLink_.maintenanceState(); }
  // Installation is called only by the explicit local commissioning owner.
  const char* installationPhysicalId() const { return boardLink_.install().physicalId(); }
  const char* installationNonce() const { return boardLink_.maintenance().nonce(); }
  bool installationLeaseValid(uint32_t nowMs) {
    boardLink_.maintenance().poll(nowMs);
    return boardLink_.maintenanceState() == babytech::boardlink::BoardMaintenanceState::Active;
  }
  bool requestInstallation(const babytech::boardlink::CommissioningImport& request, uint32_t nowMs) {
    return ready_ && installationLeaseValid(nowMs) && boardLink_.install().request(
        request, installationNonce(), boardLink_.maintenance().peerBoot(), nowMs);
  }
  babytech::boardlink::BoardInstallState installationState() const { return boardLink_.install().state(); }
  babytech::boardlink::CommissioningResult installationResult() const { return boardLink_.install().result(); }
  // Historical telemetry survives link expiry/restart; connected() reports
  // liveness separately. Only a newly accepted sample may replace its values.
  const babytech::boardlink::Status* lastTelemetry() const {
    return telemetrySeen_ ? &lastTelemetry_ : nullptr;
  }
  // Brain-local receipt time, valid when lastTelemetry() != nullptr (0 is valid).
  // Consumers must use uint32_t(nowMs - receivedAtMs) for outbound expiry.
  uint32_t lastTelemetryReceivedAtMs() const { return lastTelemetryReceivedAtMs_; }
#endif

  bool sendIntent(DisplayIntent intent, uint32_t nowMs);
  bool intentPending() const {
#if BABYTECH_BOARD_LINK_V4
    return false;
#else
    return pendingIntent_;
#endif
  }

 private:
#if BABYTECH_BOARD_LINK_V4
  // ControllerLink is global; keep the large adapter off the task stack.
  babytech::boardlink::ArduinoBoardLink boardLink_;
  bool ready_ = false;
  bool hasSnapshot_ = false;
  bool telemetrySeen_ = false;
  babytech::boardlink::Status lastTelemetry_{};
  uint32_t lastTelemetryReceivedAtMs_ = 0;
  DisplaySnapshot snapshot_{};
#else
  void handleFrame(const DisplayFrame& frame, uint32_t nowMs);
  bool sendPendingIntent(uint32_t nowMs);
  bool sendFrame(DisplayMessageType type, uint32_t sequence,
                 const uint8_t* payload, size_t payloadSize);

  HardwareSerial serial_{1};
  DisplayFrameParser parser_;
  bool ready_ = false;
  bool hasSnapshot_ = false;
  bool protocolIncompatible_ = false;
  uint32_t lastProtocolMismatchAtMs_ = 0;
  DisplaySnapshot snapshot_{};
  uint32_t lastStateAtMs_ = 0;

  bool pendingIntent_ = false;
  DisplayIntent pendingIntentValue_ = DisplayIntent::None;
  uint32_t pendingSequence_ = 0;
  uint32_t nextSequence_ = 1;
  uint32_t lastIntentSentAtMs_ = 0;
  uint8_t intentRetries_ = 0;
#endif
};

}  // namespace babytech::display
