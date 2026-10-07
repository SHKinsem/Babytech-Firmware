#include <Arduino.h>

#include "babytech_display_view.h"
#include "babytech_st7796_panel.h"
#include "controller_link.h"
#include "brain_network.h"
#if BABYTECH_BOARD_LINK_V4
#include <MaintenanceUsbConsole.h>
#include "brain_network_console.h"
#include "brain_pairing_console.h"
#include "brain_installer.h"
#include "brain_install_console.h"
#include "brain_pending_recovery.h"
#include "brain_cloud_dispatcher.h"
#include <esp_system.h>
#endif

namespace {

babytech::display::BabytechSt7796Panel panel;
babytech::display::BabytechDisplayView view;
babytech::display::ControllerLink controllerLink;
#if BABYTECH_BOARD_LINK_V4
babytech::brain::BrainNetwork network;
babytech::boardlink::MaintenanceUsbConsole commissioningSession;
// This same store will serve the subsequent local-command runtime owner.
babytech::boardlink::BrainStateStore productState;
babytech::brain::BrainPendingRecovery<babytech::display::ControllerLink> pendingRecovery(
  controllerLink, productState);
bool locallyInstalling() { return commissioningSession.active() && !controllerLink.intentPending(); }
uint32_t installNowMs() { return uint32_t(millis()); }
const char* cloudAdmission() {
  if (commissioningSession.active()) return "maintenance_active";
  if (!productState.ready()) return "storage_fault";
  if (productState.state().pending) return "busy";
  return nullptr; // Motion remains the final mechanical/resource authority.
}
babytech::brain::BrainCloudDispatcher<babytech::display::ControllerLink,
  babytech::brain::BrainNetwork> cloudDispatcher(controllerLink, network, installNowMs, cloudAdmission);
void dispatchCloudCommand(void*, const babytech::boardlink::CloudCommand& command,
                          uint32_t generation, uint32_t nowMs) {
  cloudDispatcher.command(command, generation, nowMs);
}
void dispatchCloudStop(void*, const babytech::boardlink::CloudStop& stop,
                       uint32_t generation, uint32_t nowMs) {
  cloudDispatcher.stop(stop, generation, nowMs);
}
bool newPairingEpoch(char (&epoch)[33]) {
  constexpr char hex[] = "0123456789abcdef";
  bool nonzero = false;
  for (size_t i = 0; i < 32; i += 8) {
    const uint32_t random = esp_random();
    nonzero |= random != 0;
    for (size_t n = 0; n < 8; ++n) epoch[i + n] = hex[(random >> (4 * n)) & 15];
  }
  epoch[32] = 0;
  return nonzero;
}
babytech::brain::BrainInstaller<babytech::display::ControllerLink> installer(
  controllerLink, productState, locallyInstalling, installNowMs, newPairingEpoch);

void pollCommissioningConsole() {
  // Only the explicit installation command can advance from diagnostics to writes.
  commissioningSession.poll(Serial, millis(), !controllerLink.intentPending(),
    [](const char* line, char* output, size_t capacity) {
      if (babytech::brain::BrainInstallConsole::handle(line, installer, millis(), output, capacity)) return true;
      if (installer.busy() && (!std::strncmp(line, "PAIR ", 5) ||
          (!std::strncmp(line, "NET ", 4) && std::strcmp(line, "NET STATUS")))) {
        std::snprintf(output, capacity, "[install] busy\n");
        return true;
      }
      return babytech::brain::BrainPairingConsole::handle(
        line, commissioningSession.active(), controllerLink, millis(), output, capacity) ||
        babytech::brain::BrainNetworkConsole::handle(
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
  if (const auto* pairing = controllerLink.verifiedPairing()) {
    // Reuse the installer store; recovery queries original acceptance evidence,
    // never replaying a command during boot.
    const auto loaded = productState.load(*pairing);
    Serial.printf("[Brain] Business state load=%u pending=%s; no boot replay\n",
                  unsigned(loaded), productState.ready()
                      ? (productState.state().pending ? "yes" : "no") : "unknown");
  }
  if (!network.begin(controllerLink.deviceId())) {
    Serial.println("[Brain] Network unavailable; check pairing and resources");
  }
  network.setProductHandlers(dispatchCloudCommand, dispatchCloudStop, nullptr);
#endif
}

void loop() {
  const uint32_t nowMs = millis();
  controllerLink.poll(nowMs);
#if BABYTECH_BOARD_LINK_V4
  cloudDispatcher.poll(nowMs);
  network.poll(controllerLink.lastTelemetry(), controllerLink.connected(nowMs), nowMs,
               controllerLink.lastTelemetryReceivedAtMs(), controllerLink.connected(nowMs), false);
  // Drain a queued urgent Stop before installer/recovery may perform Flash I/O.
  controllerLink.poll(uint32_t(millis()));
  pollCommissioningConsole();
  installer.poll(uint32_t(millis()));
  pendingRecovery.poll(uint32_t(millis()), commissioningSession.active() || cloudDispatcher.busy());
  if (!commissioningSession.active()) controllerLink.releaseMaintenance(uint32_t(millis()));
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
