#include "WifiOta.h"
#include "OtaPublicKey.h"
#include "OtaPage.h"

#include <Preferences.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>

#include <cstring>

// Arduino-ESP32 2.0.11 otherwise accepts an OTA image before setup() runs.
// The application confirms it only after its own services pass the boot check.
extern "C" bool verifyRollbackLater() { return true; }

namespace babytech {
namespace {
constexpr uint32_t kChallengeMs = 60000;
constexpr uint32_t kSessionMs = 120000;
constexpr uint32_t kHealthyAfterMs = 5000;
constexpr uint32_t kRollbackAfterMs = 30000;

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool decodeHex(const String& text, uint8_t* output, size_t size) {
    if (text.length() != size * 2) return false;
    for (size_t i = 0; i < size; ++i) {
        const int high = hexNibble(text[2 * i]);
        const int low = hexNibble(text[2 * i + 1]);
        if (high < 0 || low < 0) return false;
        output[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

String encodeHex(const uint8_t* bytes, size_t size) {
    static constexpr char kHex[] = "0123456789abcdef";
    String result;
    result.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        result += kHex[bytes[i] >> 4];
        result += kHex[bytes[i] & 15];
    }
    return result;
}

bool equalBytes(const uint8_t* a, const uint8_t* b, size_t size) {
    uint8_t difference = 0;
    for (size_t i = 0; i < size; ++i) difference |= a[i] ^ b[i];
    return difference == 0;
}

bool decimal(const String& text, uint32_t& value) {
    if (text.isEmpty() || text.length() > 10) return false;
    uint64_t result = 0;
    for (size_t i = 0; i < text.length(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        result = result * 10 + static_cast<unsigned>(text[i] - '0');
        if (result > UINT32_MAX) return false;
    }
    value = static_cast<uint32_t>(result);
    return true;
}

bool validVersion(const String& text) {
    if (text.isEmpty() || text.length() > 32) return false;
    for (size_t i = 0; i < text.length(); ++i) {
        const char c = text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) return false;
    }
    return true;
}

bool elapsed(uint32_t deadline) {
    return static_cast<int32_t>(millis() - deadline) >= 0;
}
} // namespace

WifiOta::WifiOta(WebServer& server, const char* board, const char* hardware,
                 const char* version, uint32_t build, const char* imageId,
                 bool (*safeToStart)(),
                 bool (*healthy)())
    : server_(server), board_(board), hardware_(hardware), version_(version),
      imageId_(imageId), build_(build), safeToStart_(safeToStart), healthy_(healthy) {
    mbedtls_sha256_init(&sha_);
}

bool WifiOta::loadAdminKey() {
    Preferences prefs;
    if (!prefs.begin("ota-admin", false)) return false;
    String saved = prefs.getString("key", "");
    uint8_t bytes[16];
    if (!decodeHex(saved, bytes, sizeof(bytes))) {
        esp_fill_random(bytes, sizeof(bytes));
        saved = encodeHex(bytes, sizeof(bytes));
        if (prefs.putString("key", saved) != saved.length()) {
            prefs.end();
            return false;
        }
    }
    prefs.end();
    memcpy(adminKey_, saved.c_str(), sizeof(adminKey_) - 1);
    adminKey_[sizeof(adminKey_) - 1] = '\0';
    return true;
}

void WifiOta::pollSerialRecovery() {
    for (size_t n = 0; n < 32 && Serial.available() > 0; ++n) {
        const char c = static_cast<char>(Serial.read());
        if (c == '\r' || c == '\n') {
            serialLine_[serialLength_] = '\0';
            if (adminReady_ && strcmp(serialLine_, "OTA CODE") == 0)
                Serial.printf("OTA administrator code: %s\n", adminKey_);
            serialLength_ = 0;
        } else if (serialLength_ < sizeof(serialLine_) - 1) {
            serialLine_[serialLength_++] = c;
        } else {
            serialLength_ = 0;
        }
    }
}

void WifiOta::begin() {
    Serial.printf("[ota] image=%s\n", imageId_);
    adminReady_ = loadAdminKey();
    if (!adminReady_) Serial.println("[ota] administrator storage unavailable; uploads disabled");
    bootAt_ = millis();
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t otaState;
    pendingVerify_ = running &&
        esp_ota_get_state_partition(running, &otaState) == ESP_OK &&
        otaState == ESP_OTA_IMG_PENDING_VERIFY;

    static const char* headers[] = {"X-OTA-Session"};
    server_.collectHeaders(headers, 1);
    server_.on("/ota", HTTP_GET, [this]() { handlePage(); });
    server_.on("/api/ota/status", HTTP_GET, [this]() { handleStatus(); });
    server_.on("/api/ota/challenge", HTTP_GET, [this]() { handleChallenge(); });
    server_.on("/api/ota/session", HTTP_POST, [this]() { handleSession(); });
    server_.on("/api/ota/image", HTTP_POST,
               [this]() { handleUploadDone(); },
               [this]() { handleUpload(); });
}

void WifiOta::poll() {
    pollSerialRecovery();
    if (state_ == State::Ready && elapsed(sessionDeadline_)) {
        state_ = State::Idle;
        token_ = "";
    }
    if (state_ == State::Staged && elapsed(rebootAt_)) ESP.restart();
    if (!pendingVerify_) return;
    const uint32_t age = millis() - bootAt_;
    if (age >= kHealthyAfterMs && healthy_ && healthy_()) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            pendingVerify_ = false;
            Serial.println("[ota] new application confirmed healthy");
            return;
        }
    }
    if (age >= kRollbackAfterMs && !rollbackAttempted_) {
        rollbackAttempted_ = true;
        Serial.println("[ota] boot check failed; rolling back");
        if (esp_ota_mark_app_invalid_rollback_and_reboot() != ESP_OK)
            Serial.println("[ota] automatic rollback unavailable; USB recovery required");
    }
}

bool WifiOta::maintenanceActive() const {
    return state_ == State::Ready || state_ == State::Uploading || state_ == State::Staged;
}

String WifiOta::stateName() const {
    switch (state_) {
        case State::Idle: return "idle";
        case State::Ready: return "ready";
        case State::Uploading: return "uploading";
        case State::Staged: return "rebooting";
        case State::Failed: return "failed";
    }
    return "failed";
}

void WifiOta::respond(int code, const String& body) {
    server_.sendHeader("Cache-Control", "no-store");
    server_.sendHeader("X-Content-Type-Options", "nosniff");
    server_.send(code, "application/json", body);
}

void WifiOta::handleStatus() {
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    String body = "{\"board\":\"";
    body += board_;
    body += "\",\"hardware\":\"";
    body += hardware_;
    body += "\",\"version\":\"";
    body += version_;
    body += "\",\"build\":";
    body += build_;
    body += ",\"maxBytes\":";
    body += next ? next->size : 0;
    body += ",\"enabled\":";
    body += adminReady_ && next ? "true" : "false";
    body += ",\"state\":\"";
    body += stateName();
    body += "\",\"received\":";
    body += static_cast<uint32_t>(received_);
    body += ",\"expected\":";
    body += static_cast<uint32_t>(expectedSize_);
    body += ",\"error\":\"";
    body += error_;
    body += "\"}";
    respond(200, body);
}

void WifiOta::handleChallenge() {
    if (!adminReady_) { respond(503, "{\"error\":\"ota_disabled\"}"); return; }
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    challenge_ = encodeHex(bytes, sizeof(bytes));
    challengeDeadline_ = millis() + kChallengeMs;
    respond(200, "{\"nonce\":\"" + challenge_ + "\"}");
}

bool WifiOta::verifySignature(const String& message, const String& signature) const {
    if (signature.length() < 16 || signature.length() > 160 || signature.length() % 2) return false;
    uint8_t der[80];
    const size_t derSize = signature.length() / 2;
    if (!decodeHex(signature, der, derSize)) return false;
    uint8_t hash[32];
    if (mbedtls_sha256_ret(reinterpret_cast<const unsigned char*>(message.c_str()),
                           message.length(), hash, 0) != 0) return false;
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    const int parsed = mbedtls_pk_parse_public_key(
        &key, reinterpret_cast<const unsigned char*>(kBabytechOtaPublicKey),
        strlen(kBabytechOtaPublicKey) + 1);
    const bool valid = parsed == 0 &&
        mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, hash, sizeof(hash), der, derSize) == 0;
    mbedtls_pk_free(&key);
    return valid;
}

bool WifiOta::verifyProof(const String& message, const String& nonce,
                          const String& proof) const {
    uint8_t expected[32], supplied[32];
    if (!decodeHex(proof, supplied, sizeof(supplied))) return false;
    const String data = "BABYTECH-OTA-AUTH-V1\n" + nonce + "\n" + message;
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info || mbedtls_md_hmac(info,
            reinterpret_cast<const unsigned char*>(adminKey_), strlen(adminKey_),
            reinterpret_cast<const unsigned char*>(data.c_str()), data.length(),
            expected) != 0) return false;
    return equalBytes(expected, supplied, sizeof(expected));
}

void WifiOta::handleSession() {
    if (!adminReady_) { respond(503, "{\"error\":\"ota_disabled\"}"); return; }
    if (maintenanceActive()) { respond(409, "{\"error\":\"ota_busy\"}"); return; }
    if (!safeToStart_ || !safeToStart_()) {
        respond(409, "{\"error\":\"machine_not_safe\"}"); return;
    }
    const String board = server_.arg("board");
    const String hardware = server_.arg("hardware");
    const String version = server_.arg("version");
    const String hash = server_.arg("sha256");
    uint32_t build = 0, size = 0;
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    uint8_t decodedHash[32];
    if (board != board_ || hardware != hardware_ || !validVersion(version) ||
        !decimal(server_.arg("build"), build) || build <= build_ ||
        !decimal(server_.arg("size"), size) || size < 1024 || !next ||
        size > next->size || !decodeHex(hash, decodedHash, sizeof(decodedHash))) {
        respond(400, "{\"error\":\"invalid_manifest\"}"); return;
    }
    const String message = "BABYTECH-OTA-V1\n" + board + "\n" + hardware + "\n" +
        String(build) + "\n" + version + "\n" + String(size) + "\n" + hash + "\n";
    const String nonce = server_.arg("nonce");
    if (nonce != challenge_ || challenge_.isEmpty() || elapsed(challengeDeadline_)) {
        respond(403, "{\"error\":\"challenge_expired\"}"); return;
    }
    challenge_ = "";
    if (!verifyProof(message, nonce, server_.arg("proof"))) {
        respond(403, "{\"error\":\"invalid_admin_proof\"}"); return;
    }
    if (!verifySignature(message, server_.arg("signature"))) {
        respond(403, "{\"error\":\"invalid_signature\"}"); return;
    }
    memcpy(expectedHash_, decodedHash, sizeof(expectedHash_));
    expectedSize_ = size;
    received_ = 0;
    uint8_t tokenBytes[16];
    esp_fill_random(tokenBytes, sizeof(tokenBytes));
    token_ = encodeHex(tokenBytes, sizeof(tokenBytes));
    clientIp_ = server_.client().remoteIP();
    sessionDeadline_ = millis() + kSessionMs;
    error_ = "";
    state_ = State::Ready;
    respond(200, "{\"session\":\"" + token_ + "\",\"expiresInMs\":120000}");
}

bool WifiOta::sessionMatches() const {
    return state_ == State::Ready && !elapsed(sessionDeadline_) &&
        server_.client().remoteIP() == clientIp_ &&
        server_.header("X-OTA-Session") == token_;
}

void WifiOta::fail(const char* reason) {
    if (Update.isRunning()) Update.abort();
    mbedtls_sha256_free(&sha_);
    mbedtls_sha256_init(&sha_);
    error_ = reason;
    state_ = State::Failed;
    token_ = "";
}

void WifiOta::handleUpload() {
    HTTPUpload& upload = server_.upload();
    if (upload.status == UPLOAD_FILE_START) {
        uploadRejected_ = !sessionMatches() || upload.name != "firmware";
        uploadError_ = uploadRejected_ ? "invalid_session" : "";
        if (uploadRejected_) return;
        if (!safeToStart_ || !safeToStart_()) {
            uploadRejected_ = true;
            uploadError_ = "machine_not_safe";
            return;
        }
        if (!Update.begin(expectedSize_, U_FLASH)) {
            fail("flash_begin_failed"); return;
        }
        mbedtls_sha256_free(&sha_);
        mbedtls_sha256_init(&sha_);
        if (mbedtls_sha256_starts_ret(&sha_, 0) != 0) {
            fail("hash_init_failed"); return;
        }
        received_ = 0;
        state_ = State::Uploading;
        return;
    }
    if (uploadRejected_ || state_ != State::Uploading) return;
    if (upload.status == UPLOAD_FILE_WRITE) {
        if (upload.currentSize > expectedSize_ - received_ ||
            Update.write(upload.buf, upload.currentSize) != upload.currentSize ||
            mbedtls_sha256_update_ret(&sha_, upload.buf, upload.currentSize) != 0) {
            fail("flash_write_failed"); return;
        }
        received_ += upload.currentSize;
    } else if (upload.status == UPLOAD_FILE_END) {
        uint8_t actualHash[32];
        if (received_ != expectedSize_ ||
            mbedtls_sha256_finish_ret(&sha_, actualHash) != 0 ||
            !equalBytes(actualHash, expectedHash_, sizeof(actualHash))) {
            fail("image_mismatch"); return;
        }
        mbedtls_sha256_free(&sha_);
        mbedtls_sha256_init(&sha_);
        if (!Update.end(false)) { fail("image_invalid"); return; }
        state_ = State::Staged;
        rebootAt_ = millis() + 1500;
        token_ = "";
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        fail("upload_aborted");
    }
}

void WifiOta::handleUploadDone() {
    if (uploadRejected_) {
        uploadRejected_ = false;
        respond(403, String("{\"error\":\"") + uploadError_ + "\"}");
        return;
    }
    if (state_ != State::Staged) {
        respond(422, String("{\"error\":\"") +
                     (state_ == State::Ready ? "upload_missing" : error_) + "\"}");
        return;
    }
    respond(200, "{\"ok\":true,\"rebooting\":true}");
}

void WifiOta::handlePage() {
    server_.sendHeader("Cache-Control", "no-store");
    server_.sendHeader("X-Content-Type-Options", "nosniff");
    server_.send_P(200, "text/html; charset=utf-8", kBabytechOtaPage);
}

} // namespace babytech
