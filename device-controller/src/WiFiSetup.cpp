#include "WiFiSetup.h"

#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "board_config.h"

namespace {

// Preferences namespace/keys owned exclusively by this class, so a "forget"
// can only ever touch our own record.
constexpr char kPrefsNamespace[] = "wifi-cfg";
constexpr char kKeySsid[] = "ssid";
constexpr char kKeyPass[] = "pass";

// Station join budget. After this we declare failure but keep the saved config
// and the AP, and simply retry later.
constexpr uint32_t kConnectTimeoutMs = 15000;
// Background retry cadence once a join failed or was lost. Retries are skipped
// while the motors are active (see pollStation()).
constexpr uint32_t kReconnectIntervalMs = 30000;
// Upper bound for an asynchronous scan; a stuck scan is abandoned.
constexpr uint32_t kScanTimeoutMs = 15000;
// Quiet window between answering a mutation and touching the radio, so the 202
// response is fully flushed before the radio state changes.
constexpr uint32_t kPostResponseDelayMs = 100;

bool isHex64(const String& value) {
  if (value.length() != 64) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!hex) return false;
  }
  return true;
}

// Append `in` to `out` as a JSON string body (no surrounding quotes), escaping
// quotes, backslashes and control characters so odd SSIDs cannot break the
// response. Bytes >= 0x80 are passed through untouched (SSIDs are UTF-8).
void appendJsonEscaped(String& out, const String& in) {
  for (size_t i = 0; i < in.length(); ++i) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    switch (c) {
      case '"': out += F("\\\""); break;
      case '\\': out += F("\\\\"); break;
      case '\b': out += F("\\b"); break;
      case '\f': out += F("\\f"); break;
      case '\n': out += F("\\n"); break;
      case '\r': out += F("\\r"); break;
      case '\t': out += F("\\t"); break;
      default:
        if (c < 0x20) {
          char buf[7];
          snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
}

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

String urlDecode(const String& in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && (i + 2) < in.length()) {
      const int hi = hexValue(in[i + 1]);
      const int lo = hexValue(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += static_cast<char>((hi << 4) | lo);
        i += 2;
      } else {
        out += c;
      }
    } else {
      out += c;
    }
  }
  return out;
}

// Minimal x-www-form-urlencoded value lookup, used only as a fallback when the
// WebServer did not parse the body into named arguments.
bool formValue(const String& body, const char* key, String& out) {
  const String prefix = String(key) + '=';
  const int length = body.length();
  int pos = 0;
  while (pos <= length) {
    int end = body.indexOf('&', pos);
    if (end < 0) end = length;
    if (body.startsWith(prefix, pos)) {
      out = urlDecode(body.substring(pos + prefix.length(), end));
      return true;
    }
    if (end >= length) break;
    pos = end + 1;
  }
  return false;
}

}  // namespace

WiFiSetup::WiFiSetup(WebServer& server, bool (*motionBusy)())
    : server_(server), motionBusy_(motionBusy) {}

void WiFiSetup::begin() {
  // We persist credentials ourselves (saveCredentials), so the WiFi stack must
  // not write its own copy to NVS.
  WiFi.persistent(false);
  // Reconnect timing is owned by poll() so it can stand down while the motors
  // are active; the driver's own auto-retry would not respect that.
  WiFi.setAutoReconnect(false);

  // AP+STA is permanent: the soft AP is never torn down, even if the station
  // link fails, is forgotten, or the board reboots unconfigured.
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  if (!WiFi.softAP(kApSsid, kApPassword, kApChannel, 0, kApMaxClients)) {
    Serial.println("[wifi] AP start failed");
  }

  server_.on("/api/wifi", HTTP_GET, [this]() { handleWifiStatus(); });
  server_.on("/api/wifi/connect", HTTP_POST, [this]() { handleConnect(); });
  server_.on("/api/wifi/forget", HTTP_POST, [this]() { handleForget(); });
  server_.on("/api/wifi/scan", HTTP_GET, [this]() { handleScanStatus(); });
  server_.on("/api/wifi/scan", HTTP_POST, [this]() { handleScanStart(); });

  loadCredentials();
  if (hasSaved_) {
    // WiFi.begin() only queues the join; it returns immediately.
    startStationConnect();
  }
}

void WiFiSetup::poll() {
  // Deferred radio mutations land first so the rest of this pass sees a
  // consistent state.
  if (pending_ != kPendingNone && timeReached(pendingAtMs_)) {
    applyPending();
  }
  pollScan();
  pollStation();
}

bool WiFiSetup::busy() const {
  return pending_ != kPendingNone || staState_ == kStaConnecting || scanState_ == kScanScanning;
}

void WiFiSetup::applyPending() {
  const PendingAction action = pending_;
  pending_ = kPendingNone;
  switch (action) {
    case kPendingConnect:
      startStationConnect();
      break;
    case kPendingForget:
      stopStation();
      staState_ = kStaIdle;
      break;
    case kPendingScan:
      startScan();
      break;
    default:
      break;
  }
}

void WiFiSetup::startStationConnect() {
  if (!hasSaved_) return;
  lastError_ = "";
  staState_ = kStaConnecting;
  connectStartedMs_ = millis();
  // Non-blocking. An empty passphrase means an open network.
  WiFi.setMinSecurity(savedPass_.length() == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK);
  WiFi.begin(savedSsid_.c_str(), savedPass_.c_str());
}

void WiFiSetup::stopStation() {
  // Drop only the station link: keep the radio on and the soft AP running.
  WiFi.disconnect(false, false);
  lastError_ = "";
}

void WiFiSetup::pollStation() {
  switch (staState_) {
    case kStaConnecting: {
      if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == savedSsid_ &&
          WiFi.localIP() != IPAddress(0, 0, 0, 0) &&
          timeReached(connectStartedMs_ + 250)) {
        staState_ = kStaConnected;
        lastError_ = "";
        reconnectAtMs_ = millis() + kReconnectIntervalMs;
      } else if (timeReached(connectStartedMs_ + kConnectTimeoutMs)) {
        // Give up on this attempt but keep the AP and the saved config; the
        // background retry below will try again later.
        WiFi.disconnect(false, false);
        staState_ = kStaFailed;
        lastError_ = "connection timed out";
        reconnectAtMs_ = millis() + kReconnectIntervalMs;
      }
      break;
    }
    case kStaConnected: {
      if (WiFi.status() != WL_CONNECTED) {
        staState_ = kStaFailed;
        lastError_ = "connection lost";
        reconnectAtMs_ = millis() + kReconnectIntervalMs;
      }
      break;
    }
    case kStaFailed:
    case kStaIdle: {
      // Background retry, deferred while the motors are active so we never
      // perturb the radio during motion.
      if (hasSaved_ && pending_ == kPendingNone && scanState_ != kScanScanning &&
          !isMotionBusy() && timeReached(reconnectAtMs_)) {
        startStationConnect();
      }
      break;
    }
  }
}

void WiFiSetup::startScan() {
  networkCount_ = 0;
  scanStartedMs_ = millis();
  scanState_ = kScanScanning;
  // Asynchronous scan: returns straight away, results are collected from
  // pollScan() via scanComplete().
  WiFi.scanNetworks(true, false);
}

void WiFiSetup::pollScan() {
  if (scanState_ != kScanScanning) return;

  const int16_t found = WiFi.scanComplete();
  if (found == WIFI_SCAN_RUNNING) {
    if (timeReached(scanStartedMs_ + kScanTimeoutMs)) {
      esp_wifi_scan_stop();
      WiFi.scanDelete();
      scanState_ = kScanFailed;
    }
    return;
  }
  if (found >= 0) {
    processScanResults(found);
    scanState_ = kScanDone;
    return;
  }
  // End any underlying scan before permitting another radio action.
  esp_wifi_scan_stop();
  WiFi.scanDelete();
  scanState_ = kScanFailed;
}

void WiFiSetup::processScanResults(int16_t count) {
  networkCount_ = 0;
  for (int16_t i = 0; i < count && i < 255; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;  // skip hidden/blank beacons
    const int32_t rssi = WiFi.RSSI(i);
    const bool secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;

    int existing = -1;
    for (size_t j = 0; j < networkCount_; ++j) {
      if (ssid.equals(networks_[j].ssid)) {
        existing = static_cast<int>(j);
        break;
      }
    }
    if (existing >= 0) {
      // Same SSID seen on several BSSIDs: keep the strongest signal.
      if (rssi > networks_[existing].rssi) networks_[existing].rssi = static_cast<int16_t>(rssi);
      continue;
    }
    if (networkCount_ < kMaxNetworks) {
      addEntry(networkCount_, ssid, rssi, secure);
      ++networkCount_;
      continue;
    }
    // Cache full: evict the weakest entry if this one is stronger.
    size_t weakest = 0;
    for (size_t j = 1; j < kMaxNetworks; ++j) {
      if (networks_[j].rssi < networks_[weakest].rssi) weakest = j;
    }
    if (rssi > networks_[weakest].rssi) {
      addEntry(weakest, ssid, rssi, secure);
    }
  }
  sortByRssiDesc();
  // Release the driver's result buffer now that we hold our bounded copy.
  WiFi.scanDelete();
}

void WiFiSetup::addEntry(size_t index, const String& ssid, int32_t rssi, bool secure) {
  size_t length = ssid.length();
  if (length > sizeof(networks_[index].ssid) - 1) length = sizeof(networks_[index].ssid) - 1;
  memcpy(networks_[index].ssid, ssid.c_str(), length);
  networks_[index].ssid[length] = '\0';
  networks_[index].rssi = static_cast<int16_t>(rssi);
  networks_[index].secure = secure;
}

void WiFiSetup::sortByRssiDesc() {
  // Insertion sort over at most kMaxNetworks entries: strongest first.
  for (size_t i = 1; i < networkCount_; ++i) {
    const ScanEntry key = networks_[i];
    size_t j = i;
    while (j > 0 && networks_[j - 1].rssi < key.rssi) {
      networks_[j] = networks_[j - 1];
      --j;
    }
    networks_[j] = key;
  }
}

void WiFiSetup::handleWifiStatus() {
  const bool connected = (staState_ == kStaConnected);

  String json;
  json.reserve(320);
  json += F("{\"state\":\"");
  json += staStateName();
  json += F("\",\"ssid\":\"");
  appendJsonEscaped(json, connected ? WiFi.SSID() : savedSsid_);
  json += F("\",\"saved\":");
  json += hasSaved_ ? F("true") : F("false");
  json += F(",\"ip\":\"");
  if (connected) json += WiFi.localIP().toString();
  json += F("\",\"apSsid\":\"");
  appendJsonEscaped(json, kApSsid);
  json += F("\",\"apIp\":\"");
  json += WiFi.softAPIP().toString();
  json += F("\",\"rssi\":");
  if (connected) {
    json += String(WiFi.RSSI());
  } else {
    json += F("null");
  }
  json += F(",\"busy\":");
  json += busy() ? F("true") : F("false");
  json += F(",\"error\":\"");
  appendJsonEscaped(json, lastError_);
  json += F("\"}");
  // The stored password is deliberately never part of any response.
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", json);
}

void WiFiSetup::handleConnect() {
  if (isMotionBusy() || busy()) {
    sendError(409, "busy");
    return;
  }

  String ssid;
  String pass;
  readFormParam("ssid", ssid);
  // A missing password is treated as empty, i.e. an open network.
  readFormParam("password", pass);

  if (ssid.length() < 1 || ssid.length() > 32) {
    sendError(400, "ssid must be 1..32 bytes");
    return;
  }
  const size_t passLength = pass.length();
  const bool passOk = (passLength == 0) || (passLength >= 8 && passLength <= 63) || isHex64(pass);
  if (!passOk) {
    sendError(400, "password must be empty, 8..63 bytes or 64 hex chars");
    return;
  }

  if (!saveCredentials(ssid, pass)) {
    sendError(500, "failed to save credentials");
    return;
  }
  savedSsid_ = ssid;
  savedPass_ = pass;
  hasSaved_ = true;
  lastError_ = "";

  // Answer first: the radio change is deferred to poll() so the response is
  // flushed before the AP channel may shift under the client.
  server_.send(202, "application/json", "{\"state\":\"connecting\"}");
  pending_ = kPendingConnect;
  pendingAtMs_ = millis() + kPostResponseDelayMs;
}

void WiFiSetup::handleForget() {
  if (isMotionBusy() || busy()) {
    sendError(409, "busy");
    return;
  }

  // Clear our own record only; the radio is left alone until after the reply.
  if (!clearCredentials()) {
    sendError(500, "failed to forget network");
    return;
  }
  savedSsid_ = "";
  savedPass_ = "";
  hasSaved_ = false;
  lastError_ = "";

  server_.send(202, "application/json", "{\"state\":\"idle\"}");
  pending_ = kPendingForget;
  pendingAtMs_ = millis() + kPostResponseDelayMs;
}

void WiFiSetup::handleScanStart() {
  if (isMotionBusy() || busy()) {
    sendError(409, "busy");
    return;
  }

  server_.send(202, "application/json", "{\"state\":\"scanning\"}");
  pending_ = kPendingScan;
  pendingAtMs_ = millis() + kPostResponseDelayMs;
}

void WiFiSetup::handleScanStatus() {
  String json;
  json.reserve(1400);
  json += F("{\"state\":\"");
  json += scanStateName();
  json += F("\",\"networks\":[");
  if (scanState_ == kScanDone) {
    for (size_t i = 0; i < networkCount_; ++i) {
      if (i > 0) json += ',';
      json += F("{\"ssid\":\"");
      appendJsonEscaped(json, String(networks_[i].ssid));
      json += F("\",\"rssi\":");
      json += String(static_cast<int>(networks_[i].rssi));
      json += F(",\"secure\":");
      json += networks_[i].secure ? F("true") : F("false");
      json += '}';
    }
  }
  json += F("]}");
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", json);
}

void WiFiSetup::loadCredentials() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {  // read-only, namespace may not exist yet
    hasSaved_ = false;
    return;
  }
  savedSsid_ = prefs.getString(kKeySsid, String());
  savedPass_ = prefs.getString(kKeyPass, String());
  prefs.end();
  hasSaved_ = savedSsid_.length() > 0;
}

bool WiFiSetup::saveCredentials(const String& ssid, const String& pass) {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) return false;
  prefs.putString(kKeySsid, ssid);
  prefs.putString(kKeyPass, pass);
  // Read back to confirm both values actually landed in NVS.
  const bool ok = prefs.isKey(kKeySsid) && prefs.isKey(kKeyPass) &&
                  (prefs.getString(kKeySsid, String()) == ssid) &&
                  (prefs.getString(kKeyPass, String()) == pass);
  prefs.end();
  return ok;
}

bool WiFiSetup::clearCredentials() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) return false;
  const bool ok = prefs.clear();
  prefs.end();
  return ok;
}

bool WiFiSetup::readFormParam(const char* name, String& out) const {
  if (server_.hasArg(name)) {
    out = server_.arg(name);
    return true;
  }
  // Fallback for clients that post the body without the form content type.
  const String body = server_.arg("plain");
  if (body.length() == 0) return false;
  return formValue(body, name, out);
}

void WiFiSetup::sendError(int code, const char* message) const {
  String json;
  json.reserve(64);
  json += F("{\"error\":\"");
  appendJsonEscaped(json, String(message));
  json += F("\"}");
  server_.send(code, "application/json", json);
}

const char* WiFiSetup::staStateName() const {
  // A queued connect already counts as "connecting" for the UI.
  if (pending_ == kPendingConnect || staState_ == kStaConnecting) return "connecting";
  switch (staState_) {
    case kStaConnected: return "connected";
    case kStaFailed: return "failed";
    default: return "idle";
  }
}

const char* WiFiSetup::scanStateName() const {
  if (pending_ == kPendingScan || scanState_ == kScanScanning) return "scanning";
  switch (scanState_) {
    case kScanDone: return "done";
    case kScanFailed: return "failed";
    default: return "idle";
  }
}

bool WiFiSetup::isMotionBusy() const {
  return motionBusy_ != nullptr && motionBusy_();
}

bool WiFiSetup::timeReached(uint32_t deadline) const {
  // Wraparound-safe millis() comparison.
  return static_cast<int32_t>(millis() - deadline) >= 0;
}
