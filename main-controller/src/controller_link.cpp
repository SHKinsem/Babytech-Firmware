#include "controller_link.h"

#include "display_board_profile.h"

namespace babytech::display {

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

}  // namespace babytech::display
