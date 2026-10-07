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
#include "brain_local_dispatcher.h"
#include <esp_system.h>
#endif

namespace {

babytech::display::BabytechSt7796Panel panel;
babytech::display::BabytechDisplayView view;
babytech::display::ControllerLink controllerLink;
#if BABYTECH_BOARD_LINK_V4
babytech::brain::BrainNetwork network;
babytech::boardlink::MaintenanceUsbConsole commissioningSession;
// Installation, local requests and result recovery share this single store.
babytech::boardlink::BrainStateStore productState;
babytech::brain::BrainPendingRecovery<babytech::display::ControllerLink> pendingRecovery(
  controllerLink, productState);
bool locallyInstalling() { return commissioningSession.active() && !controllerLink.intentPending(); }
uint32_t installNowMs() { return uint32_t(millis()); }
babytech::brain::BrainLocalDispatcher<babytech::display::ControllerLink> localDispatcher(
  controllerLink, productState, installNowMs);
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
  displayReady = panel.begin() && view.begin(true);
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
  cloudDispatcher.poll(nowMs, commissioningSession.active() || productState.state().pending);
  network.poll(controllerLink.lastTelemetry(), controllerLink.connected(nowMs), nowMs,
               controllerLink.lastTelemetryReceivedAtMs(), controllerLink.connected(nowMs), false);
  // Drain a queued urgent Stop before installer/recovery may perform Flash I/O.
  controllerLink.poll(uint32_t(millis()));
  localDispatcher.poll(uint32_t(millis()));
  pollCommissioningConsole();
  installer.poll(uint32_t(millis()));
  pendingRecovery.poll(uint32_t(millis()), commissioningSession.active() || cloudDispatcher.busy() || localDispatcher.busy());
  if (!commissioningSession.active()) controllerLink.releaseMaintenance(uint32_t(millis()));
#endif
  if (!displayReady) {
    delay(10);
    return;
  }

  const uint32_t displayNowMs = millis();
  view.poll(displayNowMs);
  babytech::display::DisplaySnapshot snapshot;
  bool controllerConnected = controllerLink.connected(displayNowMs);
  bool intentPending = controllerLink.intentPending();
  if (controllerLink.hasSnapshot()) snapshot = controllerLink.snapshot();
#if BABYTECH_BOARD_LINK_V4
  snapshot.cloudConnected = network.connected();
  snapshot.startEnabled = localDispatcher.canStart(displayNowMs,
    commissioningSession.active() || cloudDispatcher.ordinaryBusy());
  intentPending = intentPending || cloudDispatcher.ordinaryBusy() || commissioningSession.active() ||
    !productState.ready() || productState.state().pending ||
    productState.state().localSequence >= babytech::v4::kMaxSequence;
  if (commissioningSession.active()) snapshot.startEnabled = false;
#endif
  if (controllerLink.protocolIncompatible(displayNowMs)) {
    snapshot.primaryCondition =
        babytech::display::DisplayCondition::ProtocolIncompatible;
    snapshot.startEnabled = false;
    controllerConnected = true;
  } else if (intentPending) {
    snapshot.startEnabled = false;
  }
  view.update(snapshot, controllerConnected, intentPending);

  babytech::display::DisplayIntent intent;
  const bool hasIntent = view.takeIntent(intent);
#if BABYTECH_BOARD_LINK_V4
  if (hasIntent && commissioningSession.active()) {
    delay(5);
    return;
  }
  if (hasIntent) {
    if (!cloudDispatcher.yieldToLocal(uint32_t(millis()))) {
      Serial.println("[Brain] Local intent unavailable: busy");
    } else if (!localDispatcher.dispatch(intent, uint32_t(millis()), commissioningSession.active())) {
      Serial.printf("[Brain] Local intent unavailable: %s\n", localDispatcher.reason());
    }
  }
#else
  if (hasIntent && !controllerLink.sendIntent(intent, displayNowMs)) {
    Serial.println("[Display] Intent ignored while controller is unavailable");
  }
#endif
  delay(5);
}
