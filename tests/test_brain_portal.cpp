#include "brain_portal.h"
#include "brain_portal_page.h"
#include "brain_station.h"
#include "CloudLink.h"
#include "FakeCloudIo.h"
#include "FakeBrainNvs.h"
#include <WiFi.h>
#include <ArduinoJson.h>
#include <iostream>
#include <thread>

using namespace babytech::brain;
namespace {
void check(bool condition, const char* message) { fake::check(condition, message); }
void joined(const char* ssid) {
    WiFi.state = WL_CONNECTED;
    WiFi.ssid = ssid;
    WiFi.address = IPAddress(192, 168, 1, 20);
}
struct Fixture {
    BrainStation station;
    CloudLink cloud;
    BrainPortal portal;
    WebServer& http = *WebServer::instances.back();
    WiFiUDP& udp = *WiFiUDP::instances.back();
    Fixture() {
        fake::nvs.allowWrites = true;
        WiFi.state = 0;
        portal.begin("bt-000000000001");
    }
    void ui(bool feeding = false, bool button = false) {
        fake::io.inWorker = false;
        portal.poll(station, cloud, feeding, button, fake::io.now);
    }
    void worker() {
        fake::io.inWorker = true;
        station.poll();
        portal.service(station, false);
        fake::io.inWorker = false;
    }
    void manual() {
        ui(false, true);
        fake::io.now += 5000;
        ui(false, true);
        worker();
        check(portal.active(), "manual AP did not open");
    }
    void request(const char* path, HTTPMethod method) {
        http.queued = [this, path, method] { http.request(path, method); };
        worker();
    }
    void wifi(const char* ssid = "new-wifi", const char* password = "wifi-secret") {
        http.requestHeaders["X-Babytech-Portal"] = "1";
        http.arguments = {{"ssid", ssid}, {"password", password}};
        request("/api/wifi", HTTP_POST);
    }
    void mqtt() {
        http.requestHeaders["X-Babytech-Portal"] = "1";
        http.arguments = {{"host", "custom.example"}, {"port", "2883"},
                          {"user", "custom-user"}, {"password", "mqtt-secret"}};
        request("/api/mqtt", HTTP_POST);
    }
    std::string status() { request("/api/status", HTTP_GET); return http.response; }
    void noLeaks() {
        const auto body = status();
        check(body.find("secret") == std::string::npos && body.find("password") == std::string::npos,
              "GET exposes credentials");
        for (const auto& line : fake::io.serial)
            check(line.find("secret") == std::string::npos, "serial exposes credentials");
        for (const auto& call : WiFi.calls) check(call.worker, "radio call outside network worker");
        for (const auto& call : fake::nvs.calls)
            if (call.operation == "set" || call.operation == "commit") check(!call.worker, "NVS save on worker");
    }
};
void stateCase() {
    using Action = BrainPortalState::Action;
    BrainPortalState state;
    char code[7];
    check(developmentPairingCode("bt-000000000001", code) && !std::strcmp(code, "000001"), "code zero padding");
    check(developmentPairingCode("bt-FFFFFFFFFFFF", code) && !std::strcmp(code, "710655"), "code full hex range");
    for (const char* id : {"", "bt-xyz", "bt-00000000000G", "other-000000000001"})
        check(!developmentPairingCode(id, code) && !code[0], "invalid ID got code");
    check(state.action(true, false, 100) == Action::None, "disconnect opened AP");
    state.button(true, 0xfffffff0u);
    state.button(true, uint32_t(0xfffffff0u + 4999u));
    check(state.action(true, true, 100) == Action::None, "BOOT fired early");
    state.button(true, uint32_t(0xfffffff0u + 5000u));
    check(state.action(true, true, 100) == Action::Start, "BOOT rollover failed");
    state.started();
    check(state.action(true, true, 10000) == Action::None, "manual AP closed without save");
    PortalRequest request, taken;
    check(!state.submit(request) && state.take(taken), "mailbox lost request");
    check(!std::strcmp(state.submit(request), "busy"), "second save admitted");
    check(state.finish("save_failed", false), "finish failed");
    check(state.action(true, true, 20000) == Action::None, "failed save closed AP");
    check(state.finish("mqtt_saved", true), "finish save failed");
    check(state.action(true, true, 20001) == Action::None, "AP closed before grace");
    check(state.action(true, true, 22000) == Action::None, "AP grace short");
    check(state.action(true, true, 22001) == Action::Stop, "MQTT save did not close connected manual AP");
    state.stopped();
    state.button(true, 30000);
    check(state.action(true, true, 30000) == Action::None, "held BOOT reopened AP");
    state.button(false, 30000); state.button(true, 30001); state.button(true, 35001);
    check(state.action(true, true, 35001) == Action::Start, "BOOT release did not rearm");
    BrainPortalState failed;
    check(failed.action(false, false, 0) == Action::Start, "blank did not open AP");
    check(failed.action(false, false, 4999) == Action::None, "start retry unbounded");
    check(failed.action(false, false, 5000) == Action::Start, "start retry missing");
}
void mailboxCase() {
    BrainPortalState state;
    std::atomic<bool> done{false};
    std::thread snapshots([&] {
        PortalInfo info;
        while (!done.load()) { state.publish(info); state.snapshot(info); }
    });
    for (int n = 0; n < 20000; ++n) {
        PortalRequest request, taken;
        std::strcpy(request.password, "mailbox-secret");
        while (state.submit(request)) std::this_thread::yield();
        while (!state.take(taken)) std::this_thread::yield();
        check(!std::strcmp(taken.password, request.password), "credential lost during take");
        wipePortalRequest(taken);
        while (!state.finish("wifi_saved", true)) std::this_thread::yield();
    }
    done.store(true); snapshots.join();
    PortalRequest request;
    check(!state.submit(request), "saving permanently stuck after lock contention");
}
void autoCase() {
    Fixture f; f.ui(); f.worker();
    check(f.portal.active() && WiFi.apName == "Babytech-Brain-Setup", "blank/unpaired AP missing");
    check(f.udp.bound == IPAddress(192, 168, 4, 1) && f.udp.boundPort == 53, "DNS not AP-bound");
    check(f.http.listening, "HTTP not listening");
    check(f.cloud.configured() == false && fake::io.tasks.empty(), "portal invented MQTT identity/configuration");
    f.noLeaks();
}
void manualCase() {
    Fixture f;
    check(f.station.configure("old-wifi", "old-secret"), "seed failed");
    f.ui(); f.worker(); joined("old-wifi"); f.worker();
    check(!f.portal.active(), "configured WiFi opened auto AP");
    WiFi.state = 0; fake::io.now += 40000; f.worker();
    check(!f.portal.active(), "transient disconnect opened AP");
    joined("old-wifi"); f.manual(); fake::io.now += 10000; f.worker();
    check(f.portal.active(), "existing WiFi immediately closed manual AP");
    f.noLeaks();
}
void wifiCase(bool failure) {
    Fixture f;
    check(f.station.configure("old-wifi", "old-secret"), "old credentials seed");
    f.ui(); f.worker(); joined("old-wifi"); f.worker(); f.manual();
    if (failure) fake::nvs.readErrors["pass"] = ESP_FAIL; // Writes happen, readback fails: partial save.
    f.wifi(); check(f.http.status == 202, "save not queued"); f.ui();
    check(f.station.connected() == failure, "old connection confirmed new save");
    fake::io.now += 10000; f.worker();
    check(f.portal.active(), "AP closed on old connection/partial write");
    char ssid[33]; f.station.copySsid(ssid);
    check(!std::strcmp(ssid, failure ? "old-wifi" : "new-wifi"), "verified configuration incorrect");
    if (!failure) {
        check(WiFi.attemptedSsid == "new-wifi", "new credentials not joined");
        joined("new-wifi"); f.worker(); fake::io.now += 1999; f.worker();
        check(f.portal.active(), "AP grace boundary");
        fake::io.now += 1; f.worker();
        check(!f.portal.active() && !f.http.listening && !f.udp.boundPort, "joined WiFi did not close AP without MQTT");
    } else {
        check(f.status().find("save_failed") != std::string::npos, "failure reported saved");
        fake::nvs.readErrors.clear(); f.wifi(); f.ui();
        check(f.status().find("wifi_saved") != std::string::npos, "failed save left mailbox stuck");
    }
}
void feedingCase() {
    Fixture f; f.ui(); f.worker();
    f.ui(true); unsigned before = fake::nvs.writes; f.wifi();
    check(f.http.status == 409 && f.http.response.find("feeding_active") != std::string::npos, "feeding save allowed");
    f.ui(false); f.wifi(); check(f.http.status == 202, "save admission failed");
    f.ui(true); check(fake::nvs.writes == before, "feeding race applied credential save");
    check(f.status().find("feeding_active") != std::string::npos, "apply denial missing");
    f.ui(false); f.wifi(); f.ui(); check(fake::nvs.writes > before, "idle did not release save gate");
    f.noLeaks();
}
void mqttCase() {
    Fixture f; check(f.station.configure("old-wifi", "old-secret"), "seed WiFi");
    f.ui(); f.worker(); joined("old-wifi"); f.worker(); f.manual();
    const unsigned writes = fake::nvs.writes;
    f.mqtt(); check(f.http.status == 202, "MQTT admission failed"); f.ui();
    check(fake::nvs.writes == writes && fake::io.preferenceWriteCalls == 1, "MQTT save changed WiFi or failed");
    CloudLink::ConfigurationSummary summary;
    check(f.cloud.configurationSummary(summary) && !std::strcmp(summary.host, "custom.example") &&
          !std::strcmp(summary.user, "custom-user") && summary.port == 2883, "MQTT settings not saved exactly");
    f.ui(); check(f.status().find("mqtt_saved") != std::string::npos, "MQTT result not shown");
    f.noLeaks(); fake::io.now += 2000; f.worker();
    check(!f.portal.active(), "manual AP failed to close after MQTT save with existing WiFi");
}
void prefillCase() {
    Fixture f;
    check(f.cloud.configure("kept.example", 8883, "kept-user", "kept-secret"), "seed MQTT");
    check(f.station.configure("kept-wifi", "kept-secret"), "seed WiFi");
    f.ui(); f.worker(); f.manual();
    const auto before = fake::io.preferences;
    const unsigned writes = fake::io.preferenceWriteCalls;
    const auto body = f.status();
    StaticJsonDocument<1024> json; check(!deserializeJson(json, body), "status JSON invalid");
    check(json["host"] == "kept.example" && json["port"] == 8883 && json["user"] == "kept-user" &&
          json["ssid"] == "kept-wifi" && json["development_code"] == "000001", "custom prefill lost");
    check(fake::io.preferences == before && fake::io.preferenceWriteCalls == writes, "GET rewrote NVS");
    f.noLeaks();
}
void httpCase() {
    Fixture f; f.ui(); f.worker();
    f.http.client().local = IPAddress(192, 168, 1, 20);
    f.request("/api/status", HTTP_GET); check(f.http.status == 403, "STA GET accepted");
    f.wifi(); check(f.http.status == 403, "STA POST accepted");
    f.http.client().local = IPAddress(192, 168, 4, 1);
    f.http.requestHeaders.clear(); f.request("/api/wifi", HTTP_POST);
    check(f.http.status == 403, "cross-origin form accepted");
    f.wifi(); check(f.http.status == 202, "AP POST denied");
    f.mqtt(); check(f.http.status == 409, "concurrent save replaced mailbox");
    f.request("/generate_204", HTTP_GET); check(f.http.status == 302, "captive fallback missing");
    f.ui(); check(f.status().find("wifi_saved") != std::string::npos, "queued password missing completion");
    check(f.http.headers["Cache-Control"] == "no-store", "sensitive metadata response cacheable");
    f.noLeaks();
}
void invalidCase() {
    Fixture f; f.ui(); f.worker();
    f.http.requestHeaders["X-Babytech-Portal"] = "1";
    for (const auto& port : {"", "0", "65536", "-1", "1.5", "999999"}) {
        f.http.arguments = {{"host", "custom.example"}, {"port", port}, {"user", "user"}, {"password", "secret"}};
        f.request("/api/mqtt", HTTP_POST); check(f.http.status == 409, "invalid port accepted");
    }
    for (const auto& password : {std::string(128, 'p'), std::string("abc\0def", 7), std::string("abc\ndef")}) {
        f.http.arguments = {{"ssid", "wifi"}, {"password", password}};
        f.request("/api/wifi", HTTP_POST); check(f.http.status == 409, "invalid/overlong credential accepted");
    }
    check(!fake::nvs.writes && !fake::io.preferenceWriteCalls, "invalid input mutated config");
    f.wifi("wifi", "short"); f.ui(); check(f.status().find("save_failed") != std::string::npos, "WiFi validation bypassed");
    check(!fake::nvs.writes, "invalid WiFi wrote NVS");
    f.wifi("open-wifi", ""); f.ui(); f.worker();
    check(WiFi.attemptedPassword.empty(), "open WiFi password changed");
}
void dnsCase() {
    Fixture f; f.ui(); f.worker();
    const std::vector<uint8_t> query = {0x12,0x34,1,0,0,1,0,0,0,0,0,0,3,'a','b','c',0,0,1,0,1};
    f.udp.incoming = query; f.worker();
    check(f.udp.outgoing.size() == query.size() + 16 && f.udp.outgoing[0] == 0x12 &&
          f.udp.outgoing[7] == 1 && f.udp.outgoing.back() == 1, "DNS A response invalid");
    f.udp.incoming = query; f.udp.incoming[18] = 28; f.worker();
    check(f.udp.outgoing.size() == query.size() && !f.udp.outgoing[7], "AAAA answered with malformed A");
    auto reject = [&](std::vector<uint8_t> packet) {
        f.udp.incoming = std::move(packet); f.udp.outgoing.clear(); f.worker();
        check(f.udp.outgoing.empty(), "malformed DNS answered");
    };
    reject({}); reject(std::vector<uint8_t>(10)); reject(std::vector<uint8_t>(257));
    for (const auto index : {2,4,5,6,7,8,9,10,11,12,20}) {
        auto bad = query; bad[index] = 0xff; reject(bad);
    }
    auto truncated = query; truncated.pop_back(); reject(truncated);
    auto trailing = query; trailing.push_back(0); reject(trailing);
    auto compressed = query; compressed[12] = 0xc0; reject(compressed);
}
void pageCase() {
    const std::string page = kBrainPortalPage;
    check(page.find("<details>") != std::string::npos && page.find("<details open") == std::string::npos,
          "advanced MQTT expanded by default");
    check(page.find("101.33.219.108") != std::string::npos && page.find("1883") != std::string::npos, "page defaults missing");
    check(page.find("execCommand('copy')") != std::string::npos && page.find("navigator.clipboard") != std::string::npos,
          "HTTP copy fallback missing");
    check(page.find("Cloud") != std::string::npos && page.find("development") != std::string::npos,
          "development code caveat missing");
}
}
int main(int argc, char** argv) {
    try {
        check(argc == 2, "case required");
        const std::string name = argv[1];
        if (name == "state") stateCase();
        else if (name == "mailbox") mailboxCase();
        else if (name == "auto") autoCase();
        else if (name == "manual") manualCase();
        else if (name == "wifi" || name == "failed-wifi") wifiCase(name == "failed-wifi");
        else if (name == "feeding") feedingCase();
        else if (name == "mqtt") mqttCase();
        else if (name == "prefill") prefillCase();
        else if (name == "http") httpCase();
        else if (name == "invalid") invalidCase();
        else if (name == "dns") dnsCase();
        else if (name == "page") pageCase();
        else check(false, "unknown case");
        fake::cleanupLifetimeResources();
        std::cout << "PASS brain-portal " << name << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL brain-portal: " << error.what() << '\n'; return 1;
    }
}
