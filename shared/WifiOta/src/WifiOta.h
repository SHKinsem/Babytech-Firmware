#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <mbedtls/sha256.h>

namespace babytech {

class WifiOta {
public:
    // safeToStart must exclude all software-owned motion. Physical motor power
    // isolation is still required because the synchronous WebServer blocks poll().
    WifiOta(WebServer& server, const char* board, const char* hardware,
            const char* version, uint32_t build, const char* imageId,
            bool (*safeToStart)(),
            bool (*healthy)());

    void begin();
    void poll();
    bool maintenanceActive() const;

private:
    enum class State : uint8_t { Idle, Ready, Uploading, Staged, Failed };
    void handleStatus();
    void handleChallenge();
    void handleSession();
    void handleUpload();
    void handleUploadDone();
    void handlePage();
    void fail(const char* reason);
    void respond(int code, const String& body);
    String stateName() const;
    bool sessionMatches() const;
    bool loadAdminKey();
    void pollSerialRecovery();
    bool verifySignature(const String& message, const String& signature) const;
    bool verifyProof(const String& message, const String& nonce, const String& proof) const;

    WebServer& server_;
    const char* board_;
    const char* hardware_;
    const char* version_;
    const char* imageId_;
    uint32_t build_;
    bool (*safeToStart_)();
    bool (*healthy_)();
    State state_ = State::Idle;
    const char* error_ = "";
    String challenge_;
    uint32_t challengeDeadline_ = 0;
    String token_;
    IPAddress clientIp_;
    uint32_t sessionDeadline_ = 0;
    uint32_t rebootAt_ = 0;
    uint32_t bootAt_ = 0;
    bool pendingVerify_ = false;
    bool rollbackAttempted_ = false;
    bool adminReady_ = false;
    bool uploadRejected_ = false;
    const char* uploadError_ = "";
    char adminKey_[33] = {};
    char serialLine_[16] = {};
    uint8_t serialLength_ = 0;
    uint8_t expectedHash_[32] = {};
    size_t expectedSize_ = 0;
    size_t received_ = 0;
    mbedtls_sha256_context sha_;
};

} // namespace babytech
