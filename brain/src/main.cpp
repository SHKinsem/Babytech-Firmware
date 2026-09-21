#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <BoardProtocol.h>
#include "board_config.h"

extern const uint8_t indexStart[] asm("_binary_data_index_html_start");
extern const uint8_t indexEnd[] asm("_binary_data_index_html_end");

namespace {
HardwareSerial motion(1);
WebServer server(80);
babytech::Parser parser;
babytech::Status status;
uint32_t sequence = 0;
uint32_t lastQueryAt = 0;
uint32_t lastStatusAt = 0;
uint32_t lastByteAt = 0;
uint32_t responseCount = 0;
bool haveStatus = false;
bool pending = false;

void queryMotion() {
    // Coalesce the browser button and automatic refresh. Only one query in flight.
    if (pending && millis() - lastQueryAt < 500) return;
    uint8_t bytes[babytech::kMaxFrameSize];
    if (++sequence == 0) ++sequence;
    const size_t length = babytech::encode(
        babytech::Type::GetStatus, sequence, nullptr, 0, bytes, sizeof(bytes));
    pending = motion.write(bytes, length) == length;
    lastQueryAt = millis();
}

void pollMotion() {
    if (millis() - lastByteAt > babytech::kByteTimeoutMs) parser.reset();
    for (size_t count = 0; count < 128 && motion.available() > 0; ++count) {
        lastByteAt = millis();
        babytech::Frame frame;
        if (!parser.push(static_cast<uint8_t>(motion.read()), frame)) continue;
        babytech::Status next;
        if (!pending || frame.sequence != sequence || !babytech::readStatus(frame, next)) continue;
        status = next;
        haveStatus = true;
        pending = false;
        lastStatusAt = millis();
        ++responseCount;
    }
}

void sendStatus() {
    const uint32_t age = millis() - lastStatusAt;
    const bool connected = haveStatus && age < 1500;
    String body = "{\"connected\":";
    body += connected ? "true" : "false";
    body += ",\"motionState\":\"";
    body += connected ? "not_configured" : "offline";
    body += "\",\"motionUptimeMs\":";
    body += connected ? String(status.uptimeMs) : String("null");
    body += ",\"ageMs\":";
    body += haveStatus ? String(age) : String("null");
    body += ",\"responses\":" + String(responseCount);
    body += ",\"motorsAvailable\":";
    body += connected && status.motorsAvailable ? "true" : "false";
    body += ",\"sensorsAvailable\":";
    body += connected && status.sensorsAvailable ? "true" : "false";
    body += "}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", body);
}
}

void setup() {
    Serial.begin(115200);
    motion.begin(kLinkBaud, SERIAL_8N1, kLinkRxPin, kLinkTxPin);
    WiFi.mode(WIFI_AP);
    if (!WiFi.softAP("Babytech-Debug", "babytech-demo")) {
        Serial.println("Failed to start debug Wi-Fi.");
        return;
    }
    server.on("/", HTTP_GET, [] {
        server.send_P(200, "text/html; charset=utf-8",
                      reinterpret_cast<const char*>(indexStart), indexEnd - indexStart);
    });
    server.on("/api/status", HTTP_GET, sendStatus);
    server.on("/api/query", HTTP_POST, [] {
        queryMotion();
        server.send(202, "application/json", "{\"queued\":true}");
    });
    server.begin();
    Serial.print("Babytech Brain: connect to Babytech-Debug, then http://");
    Serial.println(WiFi.softAPIP());
    queryMotion();
}

void loop() {
    pollMotion();
    if (millis() - lastQueryAt >= 500) queryMotion();
    server.handleClient();
    delay(1);
}
