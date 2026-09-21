// ESP32-S3 motion board MVP.
//
// Responsibilities of this file:
//   * brain link (legacy BoardProtocol GetStatus/Status frames) over UART1,
//   * CAN bring-up + motor command HTTP API (delegated to motion::MotorControl),
//   * WiFi soft-AP + single embedded debug page (motion/data/index.html).
//
// It sends no enable or movement command on boot. Every motion command has to
// come from an explicit HTTP request; the driver may already be enabled.

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>

#include <BoardProtocol.h>
#include <MotorControl.h>
#include "WiFiSetup.h"

#include "board_config.h"

// Single self-contained page embedded by the build (board_build.embed_txtfiles).
// The blob is NUL terminated; subtract that byte when sending.
extern "C" {
extern const uint8_t indexHtmlStart[] asm("_binary_data_index_html_start");
extern const uint8_t indexHtmlEnd[] asm("_binary_data_index_html_end");
}

namespace {

HardwareSerial brain(1);
babytech::Parser parser;
motion::MotorControl motor;
WebServer server(kHttpPort);
bool motionBusy() { return motor.hasActiveMotion(); }
WiFiSetup wifiSetup(server, motionBusy);
uint32_t lastBrainByteAt = 0;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

void sendJson(int code, const String& body) {
    server.sendHeader("Cache-Control", "no-store");
    server.send(code, "application/json", body);
}

void sendError(int code, const __FlashStringHelper* message) {
    String body = F("{\"ok\":false,\"message\":\"");
    body += message;
    body += F("\"}");
    sendJson(code, body);
}

// Result messages come from trusted firmware code, but escaping them keeps the
// JSON valid no matter what text an ack string carries.
String jsonEscape(const char* text) {
    String out;
    if (!text) return out;
    for (const char* p = text; *p != '\0'; ++p) {
        const char c = *p;
        switch (c) {
            case '"': out += F("\\\""); break;
            case '\\': out += F("\\\\"); break;
            case '\n': out += F("\\n"); break;
            case '\r': out += F("\\r"); break;
            case '\t': out += F("\\t"); break;
            default:
                if (static_cast<uint8_t>(c) >= 0x20) out += c;
                break;
        }
    }
    return out;
}

// HTTP 202 from MotorControl only means "queued on this board", never that the
// drive acknowledged or finished the command. The page is told the same thing.
void sendResult(const char* action, int id, const motion::Result& result) {
    const bool ok = result.code < 300;
    String body = F("{\"ok\":");
    body += ok ? F("true") : F("false");
    body += F(",\"message\":\"");
    body += jsonEscape(result.message);
    body += F("\"}");

    int code = static_cast<int>(result.code);
    if (code < 100 || code > 599) code = 500;

    if (id >= 0) {
        Serial.printf("[http] %s id=%d -> %u %s\n", action, id,
                      static_cast<unsigned>(result.code),
                      result.message ? result.message : "");
    } else {
        Serial.printf("[http] %s -> %u %s\n", action, static_cast<unsigned>(result.code),
                      result.message ? result.message : "");
    }
    sendJson(code, body);
}

// ---------------------------------------------------------------------------
// Strict numeric parsing
//
// String::toInt() silently accepts "12abc" and returns 0 for garbage, which is
// dangerous for motion parameters. These helpers accept only a complete numeric
// literal and reject everything else (whitespace, exponents, hex, nan/inf,
// unsigned signs where not allowed).
// ---------------------------------------------------------------------------

bool parseDecimalStrict(const String& raw, double& out) {
    const unsigned int length = raw.length();
    if (length == 0 || length > 16) return false;

    unsigned int i = 0;
    bool negative = false;
    if (raw[0] == '+' || raw[0] == '-') {
        negative = (raw[0] == '-');
        i = 1;
    }
    if (i >= length) return false;

    double value = 0.0;
    double fraction = 0.1;
    bool inFraction = false;
    bool anyDigit = false;

    for (; i < length; ++i) {
        const char c = raw[i];
        if (c == '.') {
            if (inFraction) return false;
            inFraction = true;
            continue;
        }
        if (c < '0' || c > '9') return false;
        anyDigit = true;
        const double digit = static_cast<double>(c - '0');
        if (inFraction) {
            value += digit * fraction;
            fraction *= 0.1;
        } else {
            value = value * 10.0 + digit;
        }
        if (value > 1.0e7) return false;
    }

    if (!anyDigit) return false;
    out = negative ? -value : value;
    return true;  // finite by construction, no nan/inf possible
}

bool parseIntegerStrict(const String& raw, long& out) {
    const unsigned int length = raw.length();
    if (length == 0 || length > 10) return false;

    unsigned int i = 0;
    bool negative = false;
    if (raw[0] == '+' || raw[0] == '-') {
        negative = (raw[0] == '-');
        i = 1;
    }
    if (i >= length) return false;

    long value = 0;
    for (; i < length; ++i) {
        const char c = raw[i];
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
        if (value > 200000000L) return false;
    }
    out = negative ? -value : value;
    return true;
}

bool argInteger(const char* name, long& out) {
    if (!server.hasArg(name)) return false;
    return parseIntegerStrict(server.arg(name), out);
}

bool argDecimal(const char* name, double& out) {
    if (!server.hasArg(name)) return false;
    return parseDecimalStrict(server.arg(name), out);
}

// ---------------------------------------------------------------------------
// Brain link: GetStatus query -> Status frame
//
// Only a GetStatus frame with an empty payload is answered. Payload layout is
// six bytes as understood by BoardProtocol::readStatus: uptime little endian,
// one motion state byte, one flags byte (bit0 motors, bit1 sensors).
// ---------------------------------------------------------------------------

void serviceBrainLink() {
    if (millis() - lastBrainByteAt > babytech::kByteTimeoutMs) parser.reset();

    for (size_t count = 0; count < kLinkBytesPerPass && brain.available() > 0; ++count) {
        lastBrainByteAt = millis();
        babytech::Frame request;
        if (!parser.push(static_cast<uint8_t>(brain.read()), request)) continue;
        if (request.type != babytech::Type::GetStatus || request.length != 0) continue;

        const uint32_t uptimeMs = millis();
        uint8_t payload[6];
        payload[0] = static_cast<uint8_t>(uptimeMs & 0xFFu);
        payload[1] = static_cast<uint8_t>((uptimeMs >> 8) & 0xFFu);
        payload[2] = static_cast<uint8_t>((uptimeMs >> 16) & 0xFFu);
        payload[3] = static_cast<uint8_t>((uptimeMs >> 24) & 0xFFu);
        payload[4] = static_cast<uint8_t>(babytech::MotionState::NotConfigured);
        uint8_t flags = 0;
        if (motor.anyMotorOnline()) flags |= 0x01u;  // motors available
        // bit1 (sensors available) stays clear: no sensor drivers integrated.
        payload[5] = flags;

        uint8_t response[babytech::kMaxFrameSize];
        const size_t length = babytech::encode(babytech::Type::Status, request.sequence,
                                               payload, sizeof(payload), response,
                                               sizeof(response));
        if (length) brain.write(response, length);
    }
}

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------

void handleRoot() {
    const size_t blobLength = static_cast<size_t>(indexHtmlEnd - indexHtmlStart);
    if (blobLength < 2) {  // must contain at least one byte plus the terminator
        sendError(500, F("embedded debug page missing"));
        return;
    }
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html", reinterpret_cast<PGM_P>(indexHtmlStart), blobLength - 1);
}

void handleStatus() {
    long id = 0;
    if (!argInteger("id", id) || id < 1 || id > 255) {
        sendError(400, F("id must be an integer 1..255"));
        return;
    }
    const uint8_t target = static_cast<uint8_t>(id);
    motor.watch(target);
    // statusJson is already a complete JSON object; the body is forwarded
    // unmodified. It carries no uptime field.
    sendJson(200, motor.statusJson(target));
}

void handleEnable() {
    long id = 0;
    if (!argInteger("id", id) || id < 1 || id > 255) {
        sendError(400, F("id must be an integer 1..255"));
        return;
    }
    if (!server.hasArg("enabled")) {
        sendError(400, F("missing enabled"));
        return;
    }
    const String enabledRaw = server.arg("enabled");
    if (enabledRaw != "0" && enabledRaw != "1") {
        sendError(400, F("enabled must be 0 or 1"));
        return;
    }
    if (enabledRaw == "1" && wifiSetup.busy()) { sendError(409, F("wifi_busy")); return; }
    const uint8_t target = static_cast<uint8_t>(id);
    sendResult("enable", id, motor.enable(target, enabledRaw == "1"));
}

void handleMove() {
    if (wifiSetup.busy()) {
        sendError(409, F("wifi_busy"));
        return;
    }
    long id = 0;
    if (!argInteger("id", id) || id < 1 || id > 255) {
        sendError(400, F("id must be an integer 1..255"));
        return;
    }

    double angle = 0.0;
    if (!argDecimal("angle", angle)) {
        sendError(400, F("angle must be a number"));
        return;
    }
    if (angle == 0.0) {
        sendError(400, F("angle must not be 0"));
        return;
    }
    if (angle < -3600.0 || angle > 3600.0) {
        sendError(400, F("angle must be within -3600..3600"));
        return;
    }

    double speed = 0.0;
    if (!argDecimal("speed", speed)) {
        sendError(400, F("speed must be a number"));
        return;
    }
    if (speed < 0.1 || speed > 120.0) {
        sendError(400, F("speed must be within 0.1..120 rpm"));
        return;
    }

    long accel = 0;
    if (!argInteger("accel", accel) || accel < 1 || accel > 240) {
        sendError(400, F("accel must be an integer 1..240"));
        return;
    }
    long decel = 0;
    if (!argInteger("decel", decel) || decel < 1 || decel > 240) {
        sendError(400, F("decel must be an integer 1..240"));
        return;
    }
    long currentMa = 0;
    if (!argInteger("current", currentMa) || currentMa < 100 || currentMa > 5000) {
        sendError(400, F("current must be an integer 100..5000"));
        return;
    }

    // Every value is range checked above, so the narrowing casts are safe.
    motion::MoveRequest request;
    request.id = static_cast<uint8_t>(id);
    request.angleDeg = static_cast<float>(angle);
    request.speedRpm = static_cast<float>(speed);
    request.accelRpmS = static_cast<float>(accel);
    request.decelRpmS = static_cast<float>(decel);
    request.currentMa = static_cast<uint16_t>(currentMa);

    sendResult("move", id, motor.move(request));
}

void handleStop() {
    long id = 0;
    if (!argInteger("id", id) || id < 1 || id > 255) {
        sendError(400, F("id must be an integer 1..255"));
        return;
    }
    const uint8_t target = static_cast<uint8_t>(id);
    sendResult("stop", id, motor.stop(target));
}

void handleStopAll() {
    sendResult("stop-all", -1, motor.stopAll());
}

void handleCommand() {
    const String hex = server.arg("hex");
    if (hex.length() < 6 || hex.length() > 60 || hex.length() % 2) {
        sendError(400, F("hex must contain 3..30 bytes without spaces")); return;
    }
    uint8_t bytes[30];
    for (unsigned int i = 0; i < hex.length(); ++i) {
        const char c = hex[i];
        const int digit = c >= '0' && c <= '9' ? c - '0' :
            c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) { sendError(400, F("invalid hex")); return; }
        if (!(i % 2)) bytes[i/2] = static_cast<uint8_t>(digit << 4);
        else bytes[i/2] |= static_cast<uint8_t>(digit);
    }
    // Stops and disable remain accessible while the radio is occupied.
    if (wifiSetup.busy() && bytes[1] != 0xFE && bytes[1] != 0x9C &&
        !(bytes[1] == 0xF3 && hex.length() == 12 && bytes[3] == 0)) {
        sendError(409, F("wifi_busy")); return;
    }
    sendResult("command", bytes[0], motor.command(bytes, hex.length()/2));
}

void handleNotFound() {
    const String uri = server.uri();
    const bool knownPath = uri == "/" || uri == "/api/status" || uri == "/api/enable" ||
                           uri == "/api/command" || uri == "/api/trace" || uri == "/api/can-debug" || uri == "/api/move" || uri == "/api/stop" || uri == "/api/stop-all";
    if (knownPath) {
        server.sendHeader("Allow", "GET, POST");
        sendError(405, F("method not allowed"));
        return;
    }
    sendError(404, F("not found"));
}


}  // namespace

void setup() {
    Serial.begin(115200);
    Serial.println();
    Serial.println("[boot] Babytech Motion: AP + HTTP debug bridge. No motion on boot.");

    brain.begin(kLinkBaud, SERIAL_8N1, kLinkRxPin, kLinkTxPin);
    Serial.println("[uart] brain link ready on TX43/RX44 @115200");

    if (!motor.begin(kCanTxPin, kCanRxPin, kCanBitrate)) {
        Serial.println("[can] init failed; HTTP stays up, motor commands will be rejected");
    } else {
        Serial.printf("[can] ready on TX%d/RX%d @%ld\n", kCanTxPin, kCanRxPin, kCanBitrate);
    }

    wifiSetup.begin();

    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/can-debug", HTTP_GET, []() { sendJson(200, motor.canDebugJson()); });
    server.on("/api/trace", HTTP_GET, []() { sendJson(200, motor.traceJson()); });
    server.on("/api/command", HTTP_POST, handleCommand);
    server.on("/api/enable", HTTP_POST, handleEnable);
    server.on("/api/move", HTTP_POST, handleMove);
    server.on("/api/stop", HTTP_POST, handleStop);
    server.on("/api/stop-all", HTTP_POST, handleStopAll);
    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println("[http] listening on port 80; no enable or movement command sent on boot");
}

void loop() {
    motor.poll();
    server.handleClient();
    wifiSetup.poll();
    motor.poll();
    serviceBrainLink();
    delay(1);
}
