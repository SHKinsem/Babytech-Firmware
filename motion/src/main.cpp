// ESP32-S3 motion board MVP.
//
// Responsibilities of this file:
//   * brain link (BoardProtocol v2 READ/WRITE/EXEC/STOP frames) over UART1,
//   * CAN bring-up + motor command HTTP API (delegated to motion::MotorControl),
//   * WiFi soft-AP + single embedded debug page (motion/data/index.html).
//
// It sends no enable or movement command on boot. Every motion command has to
// come from an explicit HTTP or UART request; the driver may already be enabled.

#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include <BoardProtocolV2.h>
#include <Hx711Scale.h>
#include "QueueBoardMotion.h"
#include "CommandQueue.h"
#include "DebugLog.h"
#include "ProtocolGate.h"
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_system.h>
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
babytech::v2::Parser parser;
motion::MotorControl motor;
motion::Hx711Scale powderScale;
motion::CommandQueue queue(motor);
QueueBoardMotion boardMotion(motor, queue);
babytech::v2::Endpoint endpoint(boardMotion);
WebServer server(kHttpPort);
// The queue is a board operation like any other: while it runs, Wi-Fi scanning
// and the scale/config endpoints stay blocked.
bool motionBusy() { return endpoint.busy() || motor.hasActiveMotion() || queue.active(); }
WiFiSetup wifiSetup(server, motionBusy);
uint32_t lastBrainByteAt = 0;
int scaleDoutPin = kScaleDoutPin;
int scaleSckPin = kScaleSckPin;

// ---------------------------------------------------------------------------
// Diagnostic log (RAM only, see DebugLog.h)
//
// The log only observes: it cannot move, enable or stop anything, and it adds no
// serial output. State is sampled as cheap scalars once per loop and only
// TRANSITIONS are recorded, so a steady board writes nothing and no status JSON
// string is ever built for logging. This is a board-side convenience history,
// not a complete audit: raw/UART actions performed between two polls are only
// visible through the state they leave behind, and queue steps are summarised by
// state changes rather than replayed one by one.
// ---------------------------------------------------------------------------
motion::DebugLog debugLog;

String bootIdHex(uint64_t value) {
    char buffer[17];
    snprintf(buffer, sizeof(buffer), "%016llX", static_cast<unsigned long long>(value));
    return String(buffer);
}

// POST routes worth an entry. Nothing else is logged, so an unknown path or a
// query string can never end up in the log or in an export, and the Wi-Fi routes
// (which carry credentials) are deliberately absent.
const char* const kLoggedPostRoutes[] = {
    "/api/enable-all", "/api/command", "/api/move", "/api/enable", "/api/stop", "/api/stop-all",
    "/api/control/reset",
    "/api/queue/start", "/api/queue/cancel", "/api/limits", "/api/motor-distance",
    "/api/scale/tare", "/api/scale/calibrate", "/api/scale/config",
};

bool loggedPostRoute(const String& uri) {
    for (const char* route : kLoggedPostRoutes) {
        if (uri == route) return true;
    }
    return false;
}

// Request parameters are taken from a fixed allowlist - never enumerated - so an
// arbitrary argument and any credential field cannot reach the log. Queue
// programs are excluded on purpose: the browser already owns that text.
const char* const kLoggedArgs[] = {
    "id", "hex", "angle", "speed", "accel", "decel", "current", "enabled",
    "repeat", "knownWeightG", "doutPin", "sckPin", "rotationDistance",
    "maxSpeedRpm", "maxAccelRpmS", "maxCurrentMa", "maxAngleDeg",
    "maxMoveSeconds", "experimentSeconds",
};

void appendLoggedArgs(String& out) {
    for (const char* name : kLoggedArgs) {
        if (!server.hasArg(name)) continue;
        const String value = server.arg(name);
        if (value.length() == 0) continue;
        out += ' ';
        out += name;
        out += '=';
        for (unsigned int i = 0; i < value.length() && i < 24; ++i) out += value[i];
    }
}

// The response itself is never stored: only the status code, the body length and
// the documented message/error text (bounded), so no arbitrary body can leak.
void appendResponseSummary(String& out, int code, const String& body) {
    out += " code=";
    out += code;
    out += " bytes=";
    out += static_cast<unsigned int>(body.length());
    String needle = F("\"message\":\"");
    int at = body.indexOf(needle);
    const char* label = " message=";
    if (at < 0) {
        needle = F("\"error\":\"");
        at = body.indexOf(needle);
        label = " error=";
    }
    if (at < 0) return;
    out += label;
    const unsigned int start = static_cast<unsigned int>(at) + needle.length();
    for (unsigned int i = start; i < body.length(); ++i) {
        const char c = body[i];
        if (c == '"' || c == '\\') break;
        out += c;
    }
}

// One loop-time sample of the board's cheap scalar state. Strings are copied
// into fixed buffers because their source may be rewritten in place.
struct DebugSnapshot {
    bool primed = false;
    bool motorReady = false;
    uint8_t moveOutcome = 0, homeOutcome = 0, homeId = 0;
    uint8_t queueState = 0;
    uint32_t queueRunId = 0;
    uint16_t queueErrorLine = 0;
    int wifiStatus = 0;
    char fault[24] = {};
    char config[24] = {};
    char queueMessage[32] = {};
};

DebugSnapshot debugSnapshot;

void copyBounded(char* out, size_t capacity, const char* text) {
    size_t i = 0;
    if (text) {
        for (; text[i] != '\0' && i + 1 < capacity; ++i) out[i] = text[i];
    }
    out[i] = '\0';
}

void debugLogPoll(uint32_t now) {
    DebugSnapshot next;
    next.motorReady = motor.ready();
    next.moveOutcome = static_cast<uint8_t>(motor.moveOutcome());
    next.homeOutcome = static_cast<uint8_t>(motor.homeOutcome());
    next.homeId = motor.homeId();
    next.queueState = static_cast<uint8_t>(queue.state());
    next.queueRunId = queue.runId();
    next.queueErrorLine = queue.lastErrorLine();
    next.wifiStatus = static_cast<int>(WiFi.status());
    copyBounded(next.fault, sizeof(next.fault), motor.faultTag());
    copyBounded(next.config, sizeof(next.config), motor.configMessage());
    copyBounded(next.queueMessage, sizeof(next.queueMessage), queue.message());

    if (!debugSnapshot.primed) {
        // The first pass only establishes a baseline: the startup events are
        // already in the log, and nothing is reported that was never observed.
        debugSnapshot = next;
        debugSnapshot.primed = true;
        return;
    }

    if (next.motorReady != debugSnapshot.motorReady) {
        debugLog.addf(now, next.motorReady ? "info" : "error", "can.state",
                      "ready=%d", next.motorReady ? 1 : 0);
    }
    if (strcmp(next.fault, debugSnapshot.fault) != 0) {
        debugLog.addf(now, "error", "motor.fault", "fault=%s", next.fault);
    }
    if (next.moveOutcome != debugSnapshot.moveOutcome) {
        debugLog.addf(now, "info", "move.outcome", "outcome=%u",
                      static_cast<unsigned>(next.moveOutcome));
    }
    if (next.homeOutcome != debugSnapshot.homeOutcome || next.homeId != debugSnapshot.homeId) {
        debugLog.addf(now, "info", "home.outcome", "id=%u outcome=%u",
                      static_cast<unsigned>(next.homeId),
                      static_cast<unsigned>(next.homeOutcome));
    }
    if (strcmp(next.config, debugSnapshot.config) != 0) {
        debugLog.addf(now, "info", "config.state", "state=%s", next.config);
    }
    if (next.queueState != debugSnapshot.queueState || next.queueRunId != debugSnapshot.queueRunId) {
        debugLog.addf(now, "info", "queue.state", "run=%lu state=%u",
                      static_cast<unsigned long>(next.queueRunId),
                      static_cast<unsigned>(next.queueState));
    }
    if (strcmp(next.queueMessage, debugSnapshot.queueMessage) != 0) {
        const bool failed = next.queueState == static_cast<uint8_t>(motion::QueueState::Failed);
        debugLog.addf(now, failed ? "error" : "info", "queue.message",
                      "run=%lu message=%s", static_cast<unsigned long>(next.queueRunId),
                      next.queueMessage);
    }
    if (next.queueErrorLine != debugSnapshot.queueErrorLine) {
        debugLog.addf(now, "info", "queue.error_line", "line=%u",
                      static_cast<unsigned>(next.queueErrorLine));
    }
    if (next.wifiStatus != debugSnapshot.wifiStatus) {
        debugLog.addf(now, "info", "wifi.status", "status=%d", next.wifiStatus);
    }
    debugSnapshot = next;
    debugSnapshot.primed = true;  // the baseline is kept, only the values change
}

// ---------------------------------------------------------------------------
// Per-motor rotation distance (mm per revolution), stored in NVS.
//
// The queue resolves every mm step from this table at validation time and never
// falls back to a default: an id without a stored distance rejects the program.
// ---------------------------------------------------------------------------
constexpr char kMotorDistanceNamespace[] = "motor-distance";
double rotationMmValue[256] = {};
bool rotationMmValid[256] = {};

bool rotationMmAllowed(double value) {
    return value >= motion::kQueueMinRotationMm && value <= motion::kQueueMaxRotationMm;
}

void rotationKey(uint8_t id, char* out, size_t capacity) {
    snprintf(out, capacity, "d%u", static_cast<unsigned int>(id));
}

bool saveRotationMm(uint8_t id, double value) {
    Preferences prefs;
    if (!prefs.begin(kMotorDistanceNamespace, false)) return false;
    char key[8];
    rotationKey(id, key, sizeof(key));
    const bool saved = prefs.putBytes(key, &value, sizeof(value)) == sizeof(value);
    prefs.end();
    return saved;
}

bool clearRotationMm(uint8_t id) {
    Preferences prefs;
    if (!prefs.begin(kMotorDistanceNamespace, false)) return false;
    char key[8];
    rotationKey(id, key, sizeof(key));
    // A key that was never stored is already "cleared"; only an existing key
    // that refuses to go away is a failure.
    if (prefs.isKey(key) && !prefs.remove(key)) {
        prefs.end();
        return false;
    }
    prefs.end();
    return true;
}

void loadRotationDistances() {
    Preferences prefs;
    if (!prefs.begin(kMotorDistanceNamespace, true)) return;
    for (uint16_t id = 1; id < 256; ++id) {
        char key[8];
        rotationKey(static_cast<uint8_t>(id), key, sizeof(key));
        // Check the key first: reading 255 missing keys would only fill the log
        // with not-found errors.
        if (!prefs.isKey(key)) continue;
        if (prefs.getBytesLength(key) != sizeof(double)) continue;
        double value = 0.0;
        if (prefs.getBytes(key, &value, sizeof(value)) == sizeof(value) &&
            rotationMmAllowed(value)) {
            rotationMmValue[id] = value;
            rotationMmValid[id] = true;
        }
    }
    prefs.end();
}

// Adapts the stored table to the queue's rotation source.
class BoardRotationSource : public motion::QueueRotationSource {
public:
    bool rotationMm(uint8_t id, double& out) const override {
        if (!rotationMmValid[id]) return false;
        out = rotationMmValue[id];
        return true;
    }
};
BoardRotationSource boardRotation;

constexpr char kScaleIoNamespace[] = "scale-io";
constexpr char kScaleIoPinsKey[] = "pins";

bool scaleGpio45Allowed() {
    // GPIO45's strap selects VDD_SPI unless both eFuses force it to 3.3 V.
    // Reading these bits is harmless; this firmware never writes eFuses.
    static const bool allowed = esp_efuse_read_field_bit(ESP_EFUSE_VDD_SPI_FORCE) &&
                                esp_efuse_read_field_bit(ESP_EFUSE_VDD_SPI_TIEH);
    return allowed;
}

bool scalePinAllowed(int pin) {
    // GPIO 22..37 are not exposed on this N16R8 board. CAN, UART, native USB
    // and strapping pins stay reserved so a web setting cannot break recovery.
    if (!((pin >= 1 && pin <= 21) || (pin >= 38 && pin <= 48))) return false;
    if (pin == 45) return scaleGpio45Allowed();
    return pin != kCanTxPin && pin != kCanRxPin &&
           pin != kLinkTxPin && pin != kLinkRxPin &&
           pin != 3 && pin != 19 && pin != 20 && pin != 46;
}

bool scalePinsAllowed(int doutPin, int sckPin) {
    return doutPin != sckPin && scalePinAllowed(doutPin) && scalePinAllowed(sckPin);
}

uint32_t encodeScalePins(int doutPin, int sckPin) {
    return static_cast<uint32_t>(doutPin) |
           (static_cast<uint32_t>(sckPin) << 8) |
           (static_cast<uint32_t>(doutPin ^ 0xFF) << 16) |
           (static_cast<uint32_t>(sckPin ^ 0xFF) << 24);
}

void loadScalePins() {
    Preferences preferences;
    if (!preferences.begin(kScaleIoNamespace, true)) return;
    const uint32_t packed = preferences.getUInt(kScaleIoPinsKey, 0);
    preferences.end();
    const int doutPin = packed & 0xFF;
    const int sckPin = (packed >> 8) & 0xFF;
    const bool checksumValid = ((packed >> 16) & 0xFF) == (doutPin ^ 0xFF) &&
                               ((packed >> 24) & 0xFF) == (sckPin ^ 0xFF);
    if (checksumValid && scalePinsAllowed(doutPin, sckPin)) {
        scaleDoutPin = doutPin;
        scaleSckPin = sckPin;
    }
}

bool saveScalePins(int doutPin, int sckPin) {
    Preferences preferences;
    if (!preferences.begin(kScaleIoNamespace, false)) return false;
    const bool saved = preferences.putUInt(
        kScaleIoPinsKey, encodeScalePins(doutPin, sckPin)) == sizeof(uint32_t);
    preferences.end();
    return saved;
}

motion::Hx711ScaleConfig makeScaleConfig() {
    motion::Hx711ScaleConfig config;
    config.doutPin = scaleDoutPin;
    config.sckPin = scaleSckPin;
    config.startupDiscardSamples = 4;
    config.processing.tareOffsetRaw = 0;
    config.processing.countsPerGram = 0.0f;
    config.processing.filterDivisor = 4;
    config.processing.stableSampleCount = 8;
    config.processing.stableToleranceG = 1.0f;
    config.processing.sampleTimeoutMs = 1500;
    config.processing.rawMin = -8300000;
    config.processing.rawMax = 8300000;
    return config;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

void sendJson(int code, const String& body) {
    server.sendHeader("Cache-Control", "no-store");
    server.send(code, "application/json", body);
    // Diagnostic log, written after the response is already on the wire so the
    // response itself cannot be affected. Only allowlisted POST routes are
    // recorded: the frequent GET polls cost one method comparison and allocate
    // nothing, GET /api/logs never logs itself, and the query string is never
    // read. A POST that this board refused is recorded with its status and the
    // documented reason, which is exactly what is hard to see while debugging.
    if (server.method() != HTTP_POST) return;
    const String uri = server.uri();
    if (!loggedPostRoute(uri)) return;
    String detail = uri;
    appendLoggedArgs(detail);
    appendResponseSummary(detail, code, body);
    debugLog.add(millis(), code < 400 ? "info" : "warn", "http.result", detail.c_str());
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
// Brain link: v2 shared endpoint, bounded receive and event servicing.
// ---------------------------------------------------------------------------
void sendFrame(const babytech::v2::Frame& frame) {
    uint8_t bytes[babytech::v2::kMaxFrameSize];
    const size_t n=babytech::v2::encode(frame,bytes,sizeof(bytes));
    if (n) brain.write(bytes,n);
}
void serviceBrainLink() {
    using namespace babytech::v2;
    if (millis()-lastBrainByteAt>kByteTimeoutMs) parser.reset();
    boardMotion.setRadioBusy(wifiSetup.busy() || queue.active());
    for (size_t count=0;count<kLinkBytesPerPass && brain.available()>0;++count) {
        lastBrainByteAt=millis(); Frame request,response;
        if (parser.push(uint8_t(brain.read()),request)) {
            // Endpoint validates and deduplicates before QueueBoardMotion::stop
            // cancels any queue. Reads cannot acquire mechanical ownership.
            if (endpoint.handle(request,millis(),response)) sendFrame(response);
        }
    }
    Frame event;
    for (uint8_t i=0;i<2 && endpoint.tick(millis(),event);++i) sendFrame(event);
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
    // Reading a status must not steal the polling target from a running queue.
    if (!endpoint.busy() && !queue.active()) motor.watch(target);
    // statusJson is already a complete JSON object; the body is forwarded
    // unmodified. It carries no uptime field.
    sendJson(200, motor.statusJson(target));
}

String scaleStatusJson() {
    const uint32_t nowMs = millis();
    const motion::LoadCellSnapshot& snapshot = powderScale.snapshot();
    const bool fresh = snapshot.hasSample &&
        snapshot.status != motion::LoadCellStatus::Stale &&
        snapshot.status != motion::LoadCellStatus::Fault;
    String body;
    body.reserve(460);
    body += F("{\"ok\":true,\"initialized\":");
    body += powderScale.initialized() ? F("true") : F("false");
    body += F(",\"doutPin\":"); body += scaleDoutPin;
    body += F(",\"sckPin\":"); body += scaleSckPin;
    body += F(",\"gpio45Allowed\":"); body += scaleGpio45Allowed() ? F("true") : F("false");
    body += F(",\"available\":"); body += fresh ? F("true") : F("false");
    body += F(",\"status\":\""); body += motion::loadCellStatusName(snapshot.status);
    body += F("\",\"calibrated\":"); body += snapshot.calibrated ? F("true") : F("false");
    body += F(",\"stable\":"); body += snapshot.stable ? F("true") : F("false");
    body += F(",\"rawCounts\":");
    body += snapshot.hasSample ? String(snapshot.raw) : String(F("null"));
    body += F(",\"netCounts\":");
    body += snapshot.hasSample ? String(snapshot.netRaw) : String(F("null"));
    body += F(",\"rawWeightG\":");
    body += snapshot.calibrated && fresh
        ? String(snapshot.rawWeightG, 3) : String(F("null"));
    body += F(",\"weightG\":");
    body += snapshot.calibrated && fresh
        ? String(snapshot.filteredWeightG, 3) : String(F("null"));
    body += F(",\"sampleAgeMs\":");
    body += snapshot.hasSample ? String(nowMs - snapshot.sampledAtMs) : String(F("null"));
    body += F(",\"sampleCount\":"); body += snapshot.sampleCount;
    body += F(",\"tareInProgress\":");
    body += powderScale.tareInProgress() ? F("true") : F("false");
    body += F(",\"tareCompleted\":");
    body += powderScale.tareCompleted() ? F("true") : F("false");
    body += F(",\"calibrationPersisted\":");
    body += powderScale.calibrationPersisted() ? F("true") : F("false");
    body += F(",\"tareRaw\":"); body += powderScale.tareOffsetRaw();
    body += F(",\"countsPerGram\":");
    body += snapshot.calibrated ? String(powderScale.countsPerGram(), 6) : String(F("null"));
    body += '}';
    return body;
}

void handleScaleTare() {
    if (motionBusy()) { sendError(409, F("motion_active")); return; }
    if (!powderScale.initialized()) { sendError(503, F("scale_unavailable")); return; }
    if (!powderScale.startTare()) { sendError(409, F("tare_already_active")); return; }
    sendJson(202, F("{\"ok\":true,\"message\":\"tare_started\"}"));
}

void handleScaleCalibrate() {
    if (motionBusy()) { sendError(409, F("motion_active")); return; }
    double knownWeightG = 0.0;
    if (!argDecimal("knownWeightG", knownWeightG) ||
        knownWeightG < 1.0 || knownWeightG > 5000.0) {
        sendError(400, F("knownWeightG must be within 1..5000"));
        return;
    }
    const motion::LoadCellSnapshot& snapshot = powderScale.snapshot();
    if (!powderScale.tareCompleted()) { sendError(409, F("tare_required")); return; }
    if (!snapshot.hasSample || snapshot.status == motion::LoadCellStatus::Stale ||
        snapshot.status == motion::LoadCellStatus::Fault ||
        static_cast<uint32_t>(millis() - snapshot.sampledAtMs) > powderScale.sampleTimeoutMs()) {
        sendError(409, F("fresh_scale_sample_required"));
        return;
    }
    if (!powderScale.calibrate(static_cast<float>(knownWeightG), millis())) {
        sendError(422, F("calibration_failed"));
        return;
    }
    sendJson(200, scaleStatusJson());
}

void handleScaleConfig() {
    if (motionBusy()) { sendError(409, F("motion_active")); return; }
    long doutPin = -1;
    long sckPin = -1;
    if (!argInteger("doutPin", doutPin) || !argInteger("sckPin", sckPin)) {
        sendError(400, F("doutPin and sckPin must be integers"));
        return;
    }
    if (!scalePinsAllowed(static_cast<int>(doutPin), static_cast<int>(sckPin))) {
        sendError(400, F("scale_pin_invalid_or_reserved"));
        return;
    }
    if (doutPin == scaleDoutPin && sckPin == scaleSckPin) {
        sendJson(200, scaleStatusJson());
        return;
    }
    if (!saveScalePins(static_cast<int>(doutPin), static_cast<int>(sckPin))) {
        sendError(500, F("scale_pin_save_failed"));
        return;
    }
    powderScale.end();
    scaleDoutPin = static_cast<int>(doutPin);
    scaleSckPin = static_cast<int>(sckPin);
    if (!powderScale.begin(makeScaleConfig(), millis())) {
        sendError(500, F("scale_reconfigure_failed"));
        return;
    }
    Serial.printf("[scale] IO changed to DOUT%d/SCK%d from web\n", scaleDoutPin, scaleSckPin);
    sendJson(200, scaleStatusJson());
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
    const bool enabling = enabledRaw == "1";
    if (enabling) {
        // Enabling while the queue runs would hand the same node to two owners.
        if (queue.active()) { sendError(409, F("queue_busy")); return; }
        if (endpoint.busy()) { sendError(409, F("uart_operation_active")); return; }
        if (wifiSetup.busy()) { sendError(409, F("wifi_busy")); return; }
    } else if (queue.active()) {
        // Disabling is an authorised stop: it takes the node back from the queue
        // before the controller action and stays available while the UART or
        // Wi-Fi owns the bus.
        queue.cancel("stopped");
    }
    const uint8_t target = static_cast<uint8_t>(id);
    sendResult("enable", id, motor.enable(target, enabling));
}

void handleMove() {
    if (queue.active()) { sendError(409, F("queue_busy")); return; }
    if (endpoint.busy()) { sendError(409, F("uart_operation_active")); return; }
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
    const auto& limits = motor.debugLimits();
    if (fabs(angle) < 0.1 || fabs(angle) > limits.maxAngleTenths / 10.0) {
        sendError(400, F("angle_out_of_range"));
        return;
    }

    double speed = 0.0;
    if (!argDecimal("speed", speed)) {
        sendError(400, F("speed must be a number"));
        return;
    }
    if (speed < 0.1 || speed > limits.maxSpeedTenths / 10.0) {
        sendError(400, F("speed_out_of_range"));
        return;
    }

    long accel = 0;
    if (!argInteger("accel", accel) || accel < 1 || static_cast<uint32_t>(accel) > limits.maxAccelRpmS) {
        sendError(400, F("accel_out_of_range"));
        return;
    }
    long decel = 0;
    if (!argInteger("decel", decel) || decel < 1 || static_cast<uint32_t>(decel) > limits.maxAccelRpmS) {
        sendError(400, F("decel_out_of_range"));
        return;
    }
    long currentMa = 0;
    if (!argInteger("current", currentMa) || currentMa < 100 || static_cast<uint32_t>(currentMa) > limits.maxCurrentMa) {
        sendError(400, F("current_out_of_range"));
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
    // An authorised stop cancels a running queue first and always stays
    // available, even when the queue software already failed.
    if (queue.active()) queue.cancel("stopped");
    const uint8_t target = static_cast<uint8_t>(id);
    sendResult("stop", id, motor.stop(target));
}

// Operator-requested software reset of the board's volatile control ownership.
//
// It is deliberately NOT behind motionBusy(): taking ownership away from a stuck
// queue, UART exec, fault or pending stop is exactly what it is for. It never
// enables, moves, re-inits CAN, writes NVS, touches Wi-Fi/limits/rotation, and
// it never claims the shaft physically stopped - clearing internal bookkeeping
// is not evidence of that. GET cannot reach this handler.
void handleControlReset() {
    // First record what is being cleared: without this the reset itself would
    // erase the only evidence of the state it repaired.
    debugLog.addf(millis(), "warn", "control.reset",
                  "before: queue=%u run=%lu msg=%s busy=%d motion=%d uart=%d fault=%s",
                  static_cast<unsigned>(queue.state()),
                  static_cast<unsigned long>(queue.runId()),
                  queue.message() ? queue.message() : "",
                  motor.operationBusy() ? 1 : 0,
                  motor.hasActiveMotion() ? 1 : 0,
                  endpoint.busy() ? 1 : 0,
                  motor.faultTag() ? motor.faultTag() : "none");

    // Terminate UART ownership first: at most one exec and one stop record, each
    // finished as Cancelled exactly once and answered to the brain.
    babytech::v2::Frame event;
    uint8_t cancelled = 0;
    while (cancelled < 2 && endpoint.cancelPending(event)) {
        sendFrame(event);
        ++cancelled;
    }

    // One queue reset: it cancels the run (the existing cancel path sends the
    // abort/stop once), then clears queue and controller ownership whether or not
    // that stop could actually be transmitted.
    const motion::Result stopped = queue.clearControlState();
    const bool stopSent = stopped.code < 300;
    debugLog.addf(millis(), stopSent ? "warn" : "error", "control.cleared",
                  "uart_cancelled=%u stop_code=%u stop_sent=%d stop_message=%s",
                  static_cast<unsigned>(cancelled),
                  static_cast<unsigned>(stopped.code),
                  stopSent ? 1 : 0,
                  stopped.message ? stopped.message : "");

    if (stopSent) {
        sendJson(200, F("{\"ok\":true,\"stateCleared\":true,\"stopSent\":true,"
                        "\"message\":\"control_state_cleared\"}"));
        return;
    }
    // The internal state WAS cleared; the stop could not be confirmed. Both facts
    // are reported, and nothing implies the motor is physically stopped.
    sendJson(503, F("{\"ok\":false,\"stateCleared\":true,\"stopSent\":false,"
                    "\"error\":\"control_state_cleared_stop_unconfirmed\"}"));
}

void handleEnableAll() {
    long enabled = 0;
    if (!argInteger("enabled", enabled) || (enabled != 0 && enabled != 1)) {
        sendError(400, F("enabled must be 0 or 1")); return;
    }
    if (enabled) {
        if (queue.active()) { sendError(409, F("queue_busy")); return; }
        if (endpoint.busy()) { sendError(409, F("uart_operation_active")); return; }
        if (wifiSetup.busy()) { sendError(409, F("wifi_busy")); return; }
    } else {
        babytech::v2::Frame event;
        for (uint8_t i = 0; i < 2 && endpoint.cancelPending(event); ++i) sendFrame(event);
        queue.cancel("disabled_all");
    }
    sendResult("enable-all", -1, motor.broadcastEnable(enabled == 1));
}

void handleStopAll() {
    const auto result = queue.cancel("stopped");
    sendResult("stop-all", -1, result.code < 300 ? motion::Result{202,"queued"} : result);
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
    const auto kind=motion::validateCommand(bytes,hex.length()/2,motor.debugLimits());
    // Stop, interrupt and disable are authorised cancellations: they take the
    // node back from the queue before they are dispatched. The validated kind
    // decides, never the raw byte alone, so a malformed frame can never cancel a
    // running program.
    const bool stopLike = kind==motion::CommandKind::Stop || kind==motion::CommandKind::Interrupt ||
        (kind==motion::CommandKind::Enable && bytes[3]==0);
    if (queue.active()) {
        // Reads stay available while a queue runs: only mutations need the
        // single-owner rule.
        if (kind == motion::CommandKind::Read) {
            // fall through to the dispatch below
        } else if (stopLike) {
            queue.cancel("stopped");
        } else {
            sendError(409, F("queue_busy")); return;
        }
    }
    if (endpoint.busy() && kind!=motion::CommandKind::Stop && kind!=motion::CommandKind::Interrupt) {
        sendError(409,F("uart_operation_active")); return;
    }
    // Stops and disable remain accessible while the radio is occupied.
    if (wifiSetup.busy() && bytes[1] != 0xFE && bytes[1] != 0x9C &&
        !(bytes[1] == 0xF3 && hex.length() == 12 && bytes[3] == 0)) {
        sendError(409, F("wifi_busy")); return;
    }
    sendResult("command", bytes[0], motor.command(bytes, hex.length()/2));
}

String limitsJson() {
    const auto& v = motor.debugLimits();
    String json = "{\"maxSpeedRpm\":";
    json += String(v.maxSpeedTenths / 10.0, 1);
    json += ",\"maxAccelRpmS\":"; json += v.maxAccelRpmS;
    json += ",\"maxCurrentMa\":"; json += v.maxCurrentMa;
    json += ",\"maxAngleDeg\":"; json += String(v.maxAngleTenths / 10.0, 1);
    json += ",\"maxMoveSeconds\":"; json += v.maxMoveDurationMs / 1000;
    json += ",\"experimentSeconds\":"; json += v.experimentDurationMs / 1000;
    json += "}";
    return json;
}

void loadDebugLimits() {
    Preferences prefs;
    if (!prefs.begin("debug-limits", true)) return;
    motion::DebugLimits saved;
    const bool loaded = prefs.getBytesLength("config") == sizeof(saved) &&
        prefs.getBytes("config", &saved, sizeof(saved)) == sizeof(saved);
    prefs.end();
    if (loaded && motion::validDebugLimits(saved)) motor.setDebugLimits(saved);
}

void handleLimits() {
    if (endpoint.busy() || motor.operationBusy() || queue.active()) {
        sendError(409, F("limits_busy")); return;
    }
    double speed = 0, angle = 0;
    long accel = 0, current = 0, moveSeconds = 0, experimentSeconds = 0;
    if (!argDecimal("maxSpeedRpm", speed) || speed < 0.1 || speed > 3000 ||
        fabs(speed * 10 - round(speed * 10)) > 0.000001 ||
        !argDecimal("maxAngleDeg", angle) || angle < 0.1 || angle > 360000 ||
        fabs(angle * 10 - round(angle * 10)) > 0.000001 ||
        !argInteger("maxAccelRpmS", accel) || accel < 1 || accel > 65535 ||
        !argInteger("maxCurrentMa", current) || current < 100 || current > 5000 ||
        !argInteger("maxMoveSeconds", moveSeconds) || moveSeconds < 1 || moveSeconds > 3600 ||
        !argInteger("experimentSeconds", experimentSeconds) || experimentSeconds < 0 || experimentSeconds > 3600) {
        sendError(400, F("limits_invalid")); return;
    }
    motion::DebugLimits next;
    next.maxSpeedTenths = static_cast<uint32_t>(lround(speed * 10));
    next.maxAngleTenths = static_cast<uint32_t>(lround(angle * 10));
    next.maxAccelRpmS = accel; next.maxCurrentMa = current;
    next.maxMoveDurationMs = static_cast<uint32_t>(moveSeconds) * 1000;
    next.experimentDurationMs = static_cast<uint32_t>(experimentSeconds) * 1000;
    if (!motion::validDebugLimits(next)) { sendError(400, F("limits_invalid")); return; }
    Preferences prefs;
    if (!prefs.begin("debug-limits", false)) { sendError(500, F("limits_save_failed")); return; }
    const bool saved = prefs.putBytes("config", &next, sizeof(next)) == sizeof(next);
    prefs.end();
    if (!saved) { sendError(500, F("limits_save_failed")); return; }
    // HTTP and motor.poll run in one loop, so no operation can start between
    // the busy check and the atomic NVS update. Never emit a motor frame here.
    motor.setDebugLimits(next);
    sendJson(200, limitsJson());
}

void handleNotFound() {
    const String uri = server.uri();
    const bool knownPath = uri == "/" || uri == "/api/status" || uri == "/api/enable-all" || uri == "/api/enable" ||
                           uri == "/api/command" || uri == "/api/trace" || uri == "/api/can-debug" ||
                           uri == "/api/move" || uri == "/api/stop" || uri == "/api/stop-all" ||
                           uri == "/api/scale" || uri == "/api/scale/tare" ||
                           uri == "/api/scale/calibrate" || uri == "/api/scale/config" || uri == "/api/limits" ||
                           uri == "/api/queue" || uri == "/api/queue/start" || uri == "/api/queue/cancel" ||
                           uri == "/api/motor-distance";
    if (knownPath) {
        server.sendHeader("Allow", "GET, POST");
        sendError(405, F("method not allowed"));
        return;
    }
    sendError(404, F("not found"));
}

// ---------------------------------------------------------------------------
// Board queue + per-ID rotation distance
// ---------------------------------------------------------------------------

String motorDistanceJson(uint8_t id) {
    String body = F("{\"id\":");
    body += static_cast<unsigned int>(id);
    body += F(",\"rotationDistance\":");
    // Six decimals: the smallest accepted distance (0.000001 mm/rev) still
    // prints as a non-zero number, so an allowed value never reads back as 0.
    if (rotationMmValid[id]) body += String(rotationMmValue[id], 6);
    else body += F("null");
    body += '}';
    return body;
}

void handleMotorDistance() {
    long id = 0;
    if (!argInteger("id", id) || id < 1 || id > 255) {
        sendError(400, F("id must be an integer 1..255"));
        return;
    }
    const uint8_t target = static_cast<uint8_t>(id);
    if (server.method() == HTTP_GET) {
        sendJson(200, motorDistanceJson(target));
        return;
    }
    // A running queue has already resolved its mm steps: the distances are
    // immutable for that run, so the write is refused instead of taking effect
    // half way through.
    if (queue.active() || endpoint.busy() || motor.operationBusy()) {
        sendError(409, F("distance_busy"));
        return;
    }
    double value = 0.0;
    if (!argDecimal("rotationDistance", value) || value < 0.0 || value > 1000000.0) {
        sendError(400, F("rotationDistance_out_of_range"));
        return;
    }
    if (value == 0.0) {
        if (!clearRotationMm(target)) { sendError(500, F("distance_save_failed")); return; }
        rotationMmValue[target] = 0.0;
        rotationMmValid[target] = false;
        sendJson(200, motorDistanceJson(target));
        return;
    }
    if (!rotationMmAllowed(value)) { sendError(400, F("rotationDistance_out_of_range")); return; }
    if (!saveRotationMm(target, value)) { sendError(500, F("distance_save_failed")); return; }
    // Only a successful NVS write updates RAM.
    rotationMmValue[target] = value;
    rotationMmValid[target] = true;
    sendJson(200, motorDistanceJson(target));
}

void sendQueueResult(const motion::Result& result, bool started) {
    if (result.code < 300) {
        sendJson(started ? 202 : 200, queue.statusJson());
        return;
    }
    String body = F("{\"ok\":false,\"error\":\"");
    body += result.message;
    body += F("\",\"line\":");
    body += static_cast<unsigned int>(queue.lastErrorLine());
    body += '}';
    sendJson(result.code, body);
}

void handleQueueStart() {
    // The queue is a command sender: a fault, a pending stop, a busy UART/Wi-Fi
    // or an unloaded limit set must not refuse a program the operator wrote. Only
    // a queue that is already running is refused (one program owns the order of
    // frames), and the text itself has to parse.
    if (queue.active()) { sendError(409, F("queue_busy")); return; }
    if (!server.hasArg("program")) { sendError(400, F("program_required")); return; }
    long repeat = 1;
    if (!argInteger("repeat", repeat) || repeat < 1 ||
        repeat > static_cast<long>(motion::kQueueMaxRepeat)) {
        sendError(400, F("repeat_out_of_range"));
        return;
    }
    const String program = server.arg("program");
    const motion::Result started =
        queue.start(program.c_str(), program.length(), repeat, boardRotation, millis());
    if (started.code < 300) {
        // Only a started program takes the bus over: finish any pending UART
        // record so the two owners cannot interleave. An invalid program has no
        // side effects at all - nothing is cancelled and nothing is sent.
        babytech::v2::Frame event;
        uint8_t cancelled = 0;
        while (cancelled < 2 && endpoint.cancelPending(event)) {
            sendFrame(event);
            ++cancelled;
        }
    }
    sendQueueResult(started, true);
}

void handleQueueCancel() {
    // Cancelling also stops everything, and it stays available even when the
    // queue itself already failed: an authorised stop must keep working.
    sendQueueResult(queue.cancel("cancelled"), false);
}


}  // namespace

void setup() {
    Serial.begin(115200);
    Serial.println();
    Serial.println("[boot] Babytech Motion: AP + HTTP debug bridge. No motion on boot.");

    uint64_t boot=(uint64_t(esp_random())<<32)|esp_random();
    // The diagnostic log starts with this boot's identity and the reset reason,
    // once. It stores no credentials and nothing about the previous session.
    debugLog.begin(bootIdHex(boot).c_str());
    debugLog.addf(millis(), "info", "boot", "boot=%s reset=%d",
                  debugLog.bootId(), static_cast<int>(esp_reset_reason()));
    endpoint.begin(boot);
    brain.begin(kLinkBaud, SERIAL_8N1, kLinkRxPin, kLinkTxPin);
    Serial.println("[uart] brain link ready on TX43/RX44 @115200");

    loadScalePins();
    if (!powderScale.begin(makeScaleConfig(), millis())) {
        Serial.println("[scale] HX711 initialization failed; scale API stays diagnostic-only");
    } else {
        Serial.printf("[scale] HX711 ready on DOUT%d/SCK%d; calibration=%s\n",
                      scaleDoutPin, scaleSckPin,
                      powderScale.calibrationPersisted() ? "stored" : "required");
    }

    if (!motor.begin(kCanTxPin, kCanRxPin, kCanBitrate)) {
        Serial.println("[can] init failed; HTTP stays up, motor commands will be rejected");
        debugLog.addf(millis(), "error", "can.init", "ready=0 tx=%d rx=%d", kCanTxPin, kCanRxPin);
    } else {
        Serial.printf("[can] ready on TX%d/RX%d @%ld\n", kCanTxPin, kCanRxPin, kCanBitrate);
        debugLog.addf(millis(), "info", "can.init", "ready=1 tx=%d rx=%d bitrate=%ld",
                      kCanTxPin, kCanRxPin, kCanBitrate);
    }

    loadDebugLimits();
    loadRotationDistances();
    wifiSetup.begin();

    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/limits", HTTP_GET, []() { sendJson(200, limitsJson()); });
    server.on("/api/limits", HTTP_POST, handleLimits);
    server.on("/api/can-debug", HTTP_GET, []() { sendJson(200, motor.canDebugJson()); });
    server.on("/api/config-result", HTTP_GET, []() { sendJson(200, motor.configJson()); });
    server.on("/api/polling", HTTP_POST, []() {
        long enabled = 0;
        if (!argInteger("enabled", enabled) || (enabled != 0 && enabled != 1)) {
            sendError(400, F("enabled must be 0 or 1"));
            return;
        }
        motor.setAutoQueriesEnabled(enabled == 1);
        sendJson(200, enabled ? "{\"autoQueriesEnabled\":true}" : "{\"autoQueriesEnabled\":false}");
    });
    server.on("/api/trace", HTTP_GET, []() { sendJson(200, motor.traceJson()); });
    // Read-only diagnostic log of this boot (RAM ring). It never logs its own
    // request and never changes any board state.
    server.on("/api/logs", HTTP_GET, []() {
        static char logJson[motion::DebugLog::kJsonMax];
        const size_t length = debugLog.writeJson(logJson, sizeof(logJson), millis());
        if (length == 0) {
            sendJson(500, F("{\"ok\":false,\"message\":\"log_json_overflow\"}"));
            return;
        }
        sendJson(200, String(logJson, length));
    });
    server.on("/api/scale", HTTP_GET, []() { sendJson(200, scaleStatusJson()); });
    server.on("/api/scale/config", HTTP_POST, handleScaleConfig);
    server.on("/api/scale/tare", HTTP_POST, handleScaleTare);
    server.on("/api/scale/calibrate", HTTP_POST, handleScaleCalibrate);
    server.on("/api/command", HTTP_POST, handleCommand);
    server.on("/api/enable", HTTP_POST, handleEnable);
    server.on("/api/enable-all", HTTP_POST, handleEnableAll);
    server.on("/api/move", HTTP_POST, handleMove);
    server.on("/api/stop", HTTP_POST, handleStop);
    server.on("/api/stop-all", HTTP_POST, handleStopAll);
    // Operator reset of volatile control ownership: POST only, available while
    // the queue, UART or a supervised action is busy, and never a re-enable.
    server.on("/api/control/reset", HTTP_POST, handleControlReset);
    // Board queue: GET never executes anything, start validates the whole
    // program before the first CAN frame, cancel also stops everything.
    server.on("/api/queue", HTTP_GET, []() { sendJson(200, queue.statusJson()); });
    server.on("/api/queue/start", HTTP_POST, handleQueueStart);
    server.on("/api/queue/cancel", HTTP_POST, handleQueueCancel);
    server.on("/api/motor-distance", HTTP_GET, handleMotorDistance);
    server.on("/api/motor-distance", HTTP_POST, handleMotorDistance);
    server.onNotFound(handleNotFound);
    server.begin();
    debugLog.add(millis(), "info", "http.ready", "port=80 no_motion_on_boot");
    Serial.println("[http] listening on port 80; no enable or movement command sent on boot");
    // Establish the state baseline without reporting anything: the first poll
    // must not invent transitions for states that were never observed changing.
    debugLogPoll(millis());
}

void loop() {
    // Cheap scalar sampling for the diagnostic log: comparisons only, and an
    // entry is written only when something actually changed.
    debugLogPoll(millis());
    powderScale.poll(millis());
    motor.poll();
    // The queue drives one supervised action per poll and never blocks.
    queue.poll(millis());
    serviceBrainLink();
    server.handleClient();
    wifiSetup.poll();
    powderScale.poll(millis());
    motor.poll();
    queue.poll(millis());
    serviceBrainLink();
    delay(1);
}
