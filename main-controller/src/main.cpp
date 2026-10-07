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
#include "brain_context_sync.h"
#include "brain_simulation_console.h"
#include "brain_simulation_dispatcher.h"
#include "brain_simulation_mode_guard.h"
#include "brain_result_delivery.h"
#include <esp_system.h>
#endif

namespace {

babytech::display::BabytechSt7796Panel panel;
babytech::display::BabytechDisplayView view;
babytech::display::ControllerLink controllerLink;
#if BABYTECH_BOARD_LINK_V4
babytech::brain::BrainNetwork network;
babytech::brain::BrainResultDelivery<babytech::display::ControllerLink,
  babytech::brain::BrainNetwork> resultDelivery(controllerLink, network);
bool receiveMotionTerminal(const babytech::v4::Message& message,
                           const babytech::boardlink::TerminalEvent&, uint32_t) {
  return resultDelivery.terminal(message);
}
using SimulationOwner = babytech::brain::BrainSimulationDispatcher<babytech::brain::BrainNetwork>;
std::unique_ptr<SimulationOwner> simulation;
babytech::brain::BrainSimulationModeGuard simulationModeGuard;
bool simulating() { return simulation && simulation->enabled(); }
void observeRealAcceptance(const babytech::boardlink::ProductRequest& request, uint32_t nowMs) {
  simulationModeGuard.observeAccepted(request, nowMs);
}
babytech::boardlink::MaintenanceUsbConsole commissioningSession;
// Installation, local requests and result recovery share this single store.
babytech::boardlink::BrainStateStore productState;
babytech::brain::BrainContextSync<babytech::display::ControllerLink> contextSync(controllerLink, productState);
bool contextReady() { return contextSync.canPrepare(); }
void yieldConfiguration(uint32_t nowMs) { contextSync.yield(nowMs); }
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
void refreshSimulationContext() {
  if (simulation) simulation->setContext(
    productState.ready() && productState.state().hasContext ? &productState.state().context : nullptr,
    contextSync.hasUsableCache(), !commissioningSession.active() && productState.ready() && !productState.state().pending);
}
void dispatchCloudCommand(void*, const babytech::boardlink::CloudCommand& command,
                          uint32_t generation, uint32_t nowMs) {
  if (simulating()) { refreshSimulationContext(); simulation->command(command, generation, nowMs); }
  else cloudDispatcher.command(command, generation, nowMs);
}
void receiveCloudContext(void*, const babytech::boardlink::ProductContext& context, uint32_t, uint32_t) {
  contextSync.receive(context);
  refreshSimulationContext();
}
void dispatchCloudStop(void*, const babytech::boardlink::CloudStop& stop,
                       uint32_t generation, uint32_t nowMs) {
  if (simulating()) simulation->stop(stop, generation, nowMs);
  else cloudDispatcher.stop(stop, generation, nowMs);
}
void receiveCloudReceipt(void*, const babytech::boardlink::CloudReceipt& receipt) {
  if (simulation && simulation->receipt(receipt)) return;
  resultDelivery.receipt(receipt);
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
      if (!std::strncmp(line, "SIM ", 4)) {
        if (!simulation) { std::snprintf(output, capacity, "[simulation] verified_pairing_required\n"); return true; }
        const auto* last = controllerLink.lastTelemetry();
        const bool realUnresolved = installer.busy() || commissioningSession.active() ||
          cloudDispatcher.ordinaryBusy() || cloudDispatcher.stopInFlight() || localDispatcher.busy() || controllerLink.intentPending() ||
          !productState.ready() || productState.state().pending ||
          simulationModeGuard.unresolved(last, controllerLink.connected(uint32_t(millis())),
            controllerLink.lastTelemetryReceivedAtMs(), uint32_t(millis())) ||
          (last && (last->motionBusy || last->isPreparing || last->activeExecutionId[0] || !last->stationary));
        const bool before = simulating();
        const bool handled = babytech::brain::BrainSimulationConsole::handle(
          line, *simulation, realUnresolved, output, capacity);
        if (before != simulating()) contextSync.yield(uint32_t(millis()));
        return handled;
      }
      if (simulating() && (!std::strncmp(line, "INSTALL ", 8) || !std::strncmp(line, "PAIR ", 5))) {
        std::snprintf(output, capacity, "[simulation] disable_before_install_or_pairing\n");
        return true;
      }
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
    simulation.reset(new (std::nothrow) SimulationOwner(*pairing, network, installNowMs));
    Serial.printf("[Brain] Business state load=%u pending=%s; no boot replay\n",
                  unsigned(loaded), productState.ready()
                      ? (productState.state().pending ? "yes" : "no") : "unknown");
  }
  if (!network.begin(controllerLink.deviceId())) {
    Serial.println("[Brain] Network unavailable; check pairing and resources");
  }
  network.setProductHandlers(dispatchCloudCommand, dispatchCloudStop, nullptr);
  network.setContextHandler(receiveCloudContext, nullptr);
  network.setReceiptHandler(receiveCloudReceipt, nullptr);
  controllerLink.setTerminalHandler(receiveMotionTerminal);
  cloudDispatcher.setPrepareReadyHandler(contextReady);
  cloudDispatcher.setConfigurationYieldHandler(yieldConfiguration);
  cloudDispatcher.setAcceptanceHandler(observeRealAcceptance);
  localDispatcher.setPrepareReadyHandler(contextReady);
  localDispatcher.setAcceptanceHandler(observeRealAcceptance);
  pendingRecovery.setAcceptanceHandler(observeRealAcceptance);
#endif
}

void loop() {
  const uint32_t nowMs = millis();
  controllerLink.poll(nowMs);
#if BABYTECH_BOARD_LINK_V4
  simulationModeGuard.unresolved(controllerLink.lastTelemetry(), controllerLink.connected(nowMs),
    controllerLink.lastTelemetryReceivedAtMs(), nowMs);
  cloudDispatcher.poll(nowMs, commissioningSession.active() || productState.state().pending);
  if (simulation) {
    refreshSimulationContext();
    // Process explicit Stop before the timer on this iteration, so Stop wins
    // at its deadline rather than first recording a simulated success.
  }
  network.poll(controllerLink.lastTelemetry(), controllerLink.connected(nowMs), nowMs,
               controllerLink.lastTelemetryReceivedAtMs(), controllerLink.connected(nowMs), false,
               simulating() ? &simulation->status() : nullptr);
  if (simulation) simulation->poll(uint32_t(millis()));
  // Drain a queued urgent Stop before installer/recovery may perform Flash I/O.
  controllerLink.poll(uint32_t(millis()));
  resultDelivery.poll(uint32_t(millis()), commissioningSession.active());
  localDispatcher.poll(uint32_t(millis()));
  pollCommissioningConsole();
  installer.poll(uint32_t(millis()));
  pendingRecovery.poll(uint32_t(millis()), commissioningSession.active() || cloudDispatcher.busy() || localDispatcher.busy());
  contextSync.poll(uint32_t(millis()), commissioningSession.active(),
    cloudDispatcher.busy() || localDispatcher.busy(), !simulating());
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
#if BABYTECH_BOARD_LINK_V4
  if (simulating()) {
    const auto& state = simulation->status();
    snapshot = {};
    snapshot.cloudConnected = network.connected();
    snapshot.thermalSimulated = true;
    snapshot.stage = state.running ? babytech::display::DisplayStage::Mixing :
      state.complete ? babytech::display::DisplayStage::Complete :
      state.canStart ? babytech::display::DisplayStage::Ready : babytech::display::DisplayStage::NotReady;
    snapshot.primaryCondition = state.context && !state.context->cleared ?
      babytech::display::DisplayCondition::None : babytech::display::DisplayCondition::BabyMissing;
    if (state.context && !state.context->cleared) {
      snapshot.waterMl = state.request ? state.request->waterMl : state.context->waterMl;
      snapshot.temperatureC = state.request ? state.request->temperatureC : state.context->temperatureC;
      std::snprintf(snapshot.babyName.data(), snapshot.babyName.size(), "%s", state.context->babyName);
      std::snprintf(snapshot.formulaBrand.data(), snapshot.formulaBrand.size(), "%s", state.context->formulaBrand);
    }
    // This minimal tool simulates App commands, not local persistent sequences.
    snapshot.startEnabled = false;
    controllerConnected = true;
    intentPending = state.running;
  } else
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
  if (hasIntent && (commissioningSession.active() || simulating())) {
    delay(5);
    return;
  }
  if (hasIntent) {
    if (intent == babytech::display::DisplayIntent::Initialize) contextSync.yield(uint32_t(millis()));
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
