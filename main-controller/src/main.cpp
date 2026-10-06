#include <Arduino.h>

#include "babytech_display_view.h"
#include "babytech_st7796_panel.h"
#include "controller_link.h"
#include "brain_network.h"
#if BABYTECH_BOARD_LINK_V4
#include <MaintenanceUsbConsole.h>
#include "brain_network_console.h"
#include <esp_system.h>
#endif

namespace {

babytech::display::BabytechSt7796Panel panel;
babytech::display::BabytechDisplayView view;
babytech::display::ControllerLink controllerLink;
#if BABYTECH_BOARD_LINK_V4
babytech::brain::BrainNetwork network;
babytech::boardlink::MaintenanceUsbConsole commissioningSession;

void pollCommissioningConsole() {
  // Local UI lock only; neither this lock nor an export authorizes an import.
  commissioningSession.poll(Serial, millis(), !controllerLink.intentPending(),
    [](const char* line, char* output, size_t capacity) {
      return babytech::brain::BrainNetworkConsole::handle(
        line, commissioningSession.active(), network, output, capacity);
    });
}
#endif
bool displayReady = false;

}  // namespace

void setup() {
  Serial.begin(115200);
#if BABYTECH_BOARD_LINK_V4
  // Arduino 3.3 HWCDC decrements an unsigned retry counter: zero can wrap.
  Serial.setTxTimeoutMs(1);
#endif
  Serial.setTimeout(20);
  displayReady = panel.begin() && view.begin(BABYTECH_BOARD_LINK_V4 == 0);
  if (displayReady) {
    Serial.println("[Display] LVGL UI ready");
  } else {
    Serial.println("[Display] Panel initialization failed");
  }
  controllerLink.begin();
#if BABYTECH_BOARD_LINK_V4
  uint64_t maintenanceBoot = 0;
  while (!maintenanceBoot) maintenanceBoot = (uint64_t(esp_random()) << 32) | esp_random();
  commissioningSession.begin(babytech::v4::Role::Brain, maintenanceBoot);
  if (!network.begin(controllerLink.deviceId())) {
    Serial.println("[Brain] Network unavailable; check pairing and resources");
  }
#endif
}

void loop() {
  const uint32_t nowMs = millis();
  controllerLink.poll(nowMs);
#if BABYTECH_BOARD_LINK_V4
  pollCommissioningConsole();
  network.poll(controllerLink.lastTelemetry(), controllerLink.connected(nowMs), nowMs,
               controllerLink.lastTelemetryReceivedAtMs());
#endif
  if (!displayReady) {
    delay(10);
    return;
  }

  view.poll(nowMs);
  babytech::display::DisplaySnapshot snapshot;
  bool controllerConnected = controllerLink.connected(nowMs);
  if (controllerLink.hasSnapshot()) snapshot = controllerLink.snapshot();
#if BABYTECH_BOARD_LINK_V4
  snapshot.cloudConnected = network.connected();
  if (commissioningSession.active()) snapshot.startEnabled = false;
#endif
  if (controllerLink.protocolIncompatible(nowMs)) {
    snapshot.primaryCondition =
        babytech::display::DisplayCondition::ProtocolIncompatible;
    snapshot.startEnabled = false;
    controllerConnected = true;
  } else if (controllerLink.intentPending()) {
    snapshot.startEnabled = false;
  }
  view.update(snapshot, controllerConnected, controllerLink.intentPending());

  babytech::display::DisplayIntent intent;
  const bool hasIntent = view.takeIntent(intent);
#if BABYTECH_BOARD_LINK_V4
  if (hasIntent && commissioningSession.active()) {
    delay(5);
    return;
  }
#endif
  if (hasIntent && !controllerLink.sendIntent(intent, nowMs)) {
    Serial.println("[Display] Intent ignored while controller is unavailable");
  }
  delay(5);
}
