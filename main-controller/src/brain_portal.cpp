#include "brain_portal.h"
#include "brain_portal_page.h"
#include "brain_station.h"
#include "CloudLink.h"
#include <ArduinoJson.h>
#include <WiFi.h>

namespace babytech { namespace brain {
namespace {
template<size_t N>
bool field(const String& value, char (&output)[N]) {
    if (value.length() >= N || std::strlen(value.c_str()) != value.length()) return false;
    for (size_t n = 0; n < value.length(); ++n)
        if (static_cast<unsigned char>(value[n]) < 32 || value[n] == 127) return false;
    std::memcpy(output, value.c_str(), value.length() + 1);
    return true;
}
bool portNumber(const String& value, uint16_t& output) {
    if (!value.length() || value.length() > 5) return false;
    uint32_t number = 0;
    for (size_t n = 0; n < value.length(); ++n) {
        if (value[n] < '0' || value[n] > '9') return false;
        number = number * 10 + unsigned(value[n] - '0');
    }
    if (!number || number > 65535) return false;
    output = uint16_t(number);
    return true;
}
}

void BrainPortal::begin(const char* deviceId) {
    if (deviceId && std::strlen(deviceId) < sizeof(info_.deviceId)) {
        std::strcpy(info_.deviceId, deviceId);
        developmentPairingCode(deviceId, info_.developmentCode);
        if (std::strlen(deviceId) < sizeof(info_.user)) std::strcpy(info_.user, deviceId);
    }
    state_.publish(info_);
    enabled_.store(true);
}

void BrainPortal::poll(BrainStation& station, CloudLink& cloud, bool feeding, bool bootDown, uint32_t nowMs) {
    state_.button(bootDown, nowMs);
    info_.feeding = feeding;
    if (finishResult_ && state_.finish(finishResult_, finishSaved_)) finishResult_ = nullptr;
    if (refresh_) {
        CloudLink::ConfigurationSummary summary;
        const bool copiedSsid = station.copySsid(info_.ssid);
        if (cloud.configurationSummary(summary)) {
            std::memcpy(info_.host, summary.host, sizeof(info_.host));
            std::memcpy(info_.user, summary.user, sizeof(info_.user));
            info_.port = summary.port;
        }
        if (copiedSsid) refresh_ = false;
    }
    state_.publish(info_);
    PortalRequest request;
    if (finishResult_ || !state_.take(request)) return;
    bool saved = false;
    if (feeding) finishResult_ = "feeding_active";
    else {
        saved = request.kind == PortalSave::Wifi ? station.configure(request.hostOrSsid, request.password) :
            cloud.configure(request.hostOrSsid, request.port, request.user, request.password);
        finishResult_ = saved ? (request.kind == PortalSave::Wifi ? "wifi_saved" : "mqtt_saved") : "save_failed";
        if (saved) { refresh_ = true; if (request.kind == PortalSave::Wifi) cloud.requestReconnect(); }
    }
    wipePortalRequest(request);
    finishSaved_ = saved;
    if (state_.finish(finishResult_, saved)) finishResult_ = nullptr;
}

bool BrainPortal::localRequest() {
    if (!state_.active() || server_.client().localIP() != WiFi.softAPIP()) {
        reply(403, "ap_only");
        return false;
    }
    server_.sendHeader("Cache-Control", "no-store");
    return true;
}

void BrainPortal::reply(int code, const char* result) {
    String body = "{\"result\":\"";
    body += result;
    body += "\"}";
    server_.send(code, "application/json", body);
}

void BrainPortal::status() {
    if (!localRequest()) return;
    PortalInfo info;
    if (!state_.snapshot(info)) { reply(503, "busy"); return; }
    StaticJsonDocument<1024> document;
    document["device_id"] = info.deviceId;
    document["development_code"] = info.developmentCode;
    document["ssid"] = info.ssid;
    document["host"] = info.host;
    document["port"] = info.port;
    document["user"] = info.user;
    document["result"] = info.result;
    document["feeding"] = info.feeding;
    document["wifi_connected"] = wifiConnected_;
    document["mqtt_connected"] = mqttConnected_;
    char body[1536];
    serializeJson(document, body, sizeof(body));
    server_.send(200, "application/json", body);
}

void BrainPortal::save(PortalSave kind) {
    if (!localRequest()) return;
    // A custom header prevents cross-origin browser form POSTs (no CORS grant).
    if (!(server_.header("X-Babytech-Portal") == "1")) { reply(403, "local_page_required"); return; }
    PortalRequest request;
    request.kind = kind;
    const bool valid = field(server_.arg(kind == PortalSave::Wifi ? "ssid" : "host"), request.hostOrSsid) &&
        field(server_.arg("password"), request.password) &&
        (kind == PortalSave::Wifi || (field(server_.arg("user"), request.user) && portNumber(server_.arg("port"), request.port)));
    const char* error = valid ? state_.submit(request) : "invalid_parameters";
    wipePortalRequest(request);
    reply(error ? 409 : 202, error ? error : "pending");
}

void BrainPortal::routes() {
    const char* headers[] = {"X-Babytech-Portal"};
    server_.collectHeaders(headers, 1);
    server_.on("/", HTTP_GET, [this] { if (localRequest()) server_.send_P(200, "text/html", kBrainPortalPage); });
    server_.on("/api/status", HTTP_GET, [this] { status(); });
    server_.on("/api/wifi", HTTP_POST, [this] { save(PortalSave::Wifi); });
    server_.on("/api/mqtt", HTTP_POST, [this] { save(PortalSave::Mqtt); });
    server_.onNotFound([this] {
        if (!localRequest()) return;
        server_.sendHeader("Location", "http://192.168.4.1/");
        server_.send(302, "text/plain", "Open http://192.168.4.1/");
    });
    routesReady_ = true;
}

void BrainPortal::service(BrainStation& station, bool mqttConnected) {
    if (!enabled_.load() || !station.credentialsKnown()) return;
    wifiConnected_ = station.connected();
    mqttConnected_ = mqttConnected;
    const auto action = state_.action(station.hasCredentials(), wifiConnected_, millis());
    if (action == BrainPortalState::Action::Start) {
        station.provisioningAp(true);
        const IPAddress address(192, 168, 4, 1);
        if (WiFi.softAPConfig(address, address, IPAddress(255, 255, 255, 0)) && WiFi.softAP("Babytech-Brain-Setup")) {
            if (!routesReady_) routes();
            server_.begin();
            dns_.begin(address, 53); // Bound to AP IP, not the STA DNS interface.
            state_.started();
        } else {
            WiFi.softAPdisconnect(false);
            station.provisioningAp(false);
        }
    } else if (action == BrainPortalState::Action::Stop) {
        server_.stop();
        dns_.stop();
        WiFi.softAPdisconnect(false);
        station.provisioningAp(false);
        state_.stopped();
    }
    if (state_.active()) { dns(); server_.handleClient(); }
}

void BrainPortal::dns() {
    const int size = dns_.parsePacket();
    if (!size) return;
    uint8_t packet[288];
    if (size < 17 || size > 256) { dns_.clear(); return; }
    const int read = dns_.read(packet, size);
    if (read != size || (packet[2] & 0xf8) || packet[4] || packet[5] != 1 ||
        packet[6] || packet[7] || packet[8] || packet[9] || packet[10] || packet[11]) return;
    size_t end = 12;
    while (end < size_t(size) && packet[end]) {
        const uint8_t length = packet[end++];
        if (length > 63 || end + length >= size_t(size)) return;
        end += length;
    }
    if (end + 5 != size_t(size)) return;
    ++end;
    const uint16_t type = uint16_t(packet[end]) << 8 | packet[end + 1];
    if (packet[end + 2] || packet[end + 3] != 1) return;
    packet[2] = uint8_t(0x80 | (packet[2] & 1));
    packet[3] = 0x80;
    packet[6] = 0; packet[7] = (type == 1 || type == 255) ? 1 : 0;
    packet[8] = packet[9] = packet[10] = packet[11] = 0;
    size_t output = size_t(size);
    if (packet[7]) {
        constexpr uint8_t answer[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1};
        std::memcpy(packet + output, answer, sizeof(answer));
        output += sizeof(answer);
    }
    dns_.beginPacket(dns_.remoteIP(), dns_.remotePort());
    dns_.write(packet, output);
    dns_.endPacket();
}
} }
