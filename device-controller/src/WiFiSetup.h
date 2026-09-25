#pragma once

#include <Arduino.h>
#include <WebServer.h>

// Runtime WiFi commissioning for the existing CAN motor debug page.
//
// Design constraints:
//   * The soft AP (kApSsid / kApPassword from board_config.h) is permanent.
//     It is started here and is never disabled or reconfigured, so the debug
//     AP remains enabled when STA fails; clients may temporarily lose the link.
//   * Station credentials are optional. They live in a dedicated Preferences
//     namespace owned by this class only; they are never logged and never
//     echoed back over HTTP (only the requested/saved SSID may be shown).
//   * No polling wait blocks the loop: no delay(), no synchronous scan,
//     no wait loops. Radio work is sequenced by poll().
//   * busy() is true while a connect attempt, a scan or a deferred radio
//     mutation is in flight. The parent should refuse NEW motion commands
//     while busy() is true but must always keep its stop path available.
//
// Single-radio note: the ESP32-S3 has one 2.4 GHz radio shared by AP and STA.
// When the STA joins a router on a different channel the AP has to follow that
// channel, so already-connected AP clients can momentarily drop and reconnect.
// The AP itself stays up; only short-lived link hiccups are expected.
//
// HTTP surface (all JSON, no credentials ever returned):
//   GET  /api/wifi          -> {state, ssid, saved, ip, apSsid, apIp, rssi, busy, error}
//   POST /api/wifi/connect  -> urlencoded ssid,password; 202, radio change deferred
//   POST /api/wifi/forget   -> 202, clears only our Preferences record, keeps AP
//   POST /api/wifi/scan     -> 202, asynchronous scan started from poll()
//   GET  /api/wifi/scan     -> {state, networks:[{ssid, rssi, secure}]}
//
// mDNS is started by the parent after HTTP is listening; this class owns only
// the AP and station lifecycle. Captive portal, OTA, and AP password editing
// are outside this class.
class WiFiSetup {
 public:
  // motionBusy() is supplied by the parent (motor/CAN layer). It must be a
  // cheap, non-blocking query. May be nullptr, in which case motion is never
  // considered busy.
  WiFiSetup(WebServer& server, bool (*motionBusy)());

  // Starts the permanent soft AP, registers the /api/wifi routes, loads any
  // saved station credentials and, when present, kicks off a non-blocking
  // join. The parent no longer needs its own softAP/startAP step.
  void begin();

  // Drives the connect / scan / reconnect state machines. Call every loop()
  // pass, unconditionally (it never blocks and yields to motion by itself).
  void poll();

  // True while connecting, scanning or waiting to apply a radio mutation.
  bool busy() const;
  bool apReady() const { return apStarted_; }

 private:
  // Bounded cache of the strongest distinct scan results.
  static constexpr size_t kMaxNetworks = 20;

  enum StaState : uint8_t { kStaIdle, kStaConnecting, kStaConnected, kStaFailed };
  enum ScanState : uint8_t { kScanIdle, kScanScanning, kScanDone, kScanFailed };
  enum PendingAction : uint8_t { kPendingNone, kPendingConnect, kPendingForget, kPendingScan };

  struct ScanEntry {
    char ssid[33];  // 32 bytes + NUL
    int16_t rssi;
    bool secure;
  };

  // HTTP handlers.
  void handleWifiStatus();
  void handleConnect();
  void handleForget();
  void handleScanStart();
  void handleScanStatus();

  // State machine steps.
  void applyPending();
  void pollStation();
  void pollScan();

  // Radio helpers (none of these block).
  void startStationConnect();
  void stopStation();
  void startScan();
  void processScanResults(int16_t count);

  // Persistence (private namespace, verified writes).
  void loadCredentials();
  bool saveCredentials(const String& ssid, const String& pass);
  bool clearCredentials();

  // Scan cache helpers.
  void addEntry(size_t index, const String& ssid, int32_t rssi, bool secure);
  void sortByRssiDesc();

  // Request/response helpers.
  bool readFormParam(const char* name, String& out) const;
  void sendError(int code, const char* message) const;
  const char* staStateName() const;
  const char* scanStateName() const;
  bool isMotionBusy() const;
  bool timeReached(uint32_t deadline) const;

  WebServer& server_;
  bool (*motionBusy_)();

  StaState staState_ = kStaIdle;
  ScanState scanState_ = kScanIdle;
  PendingAction pending_ = kPendingNone;

  uint32_t connectStartedMs_ = 0;   // start of the current join attempt
  uint32_t reconnectAtMs_ = 0;      // next background retry deadline
  uint32_t scanStartedMs_ = 0;      // start of the current scan
  uint32_t pendingAtMs_ = 0;        // when a deferred radio action may run

  bool hasSaved_ = false;
  bool apStarted_ = false;
  String savedSsid_;   // shown to the UI; never the password
  String savedPass_;   // RAM only, never logged or returned
  String lastError_;   // human-readable last station error, "" when fine

  ScanEntry networks_[kMaxNetworks];
  size_t networkCount_ = 0;
};
