// Production WifiOta HTTP handlers; disposable public trust fixture, no private key.
#pragma once
#include <OtaPublicKey.h>

namespace motion_main_ota {
constexpr char publicKey[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAExQV2LaiTAY3yI7kQoF1DWL0vx/GJ\n"
    "0jFijfadGpJ3IFq+0XfVfCA0KnAeEjG0ZeJ7+r0eZxkAa81WvC1lAEF82g==\n"
    "-----END PUBLIC KEY-----\n";
constexpr char hash[] = "9b6ce55f379e9771551de6939556a7e6b949814ae27c2f5cfd5dbeb378ce7c2a";
constexpr char signature[] =
    "304402203504791d660d7eea2182305a67029d286e8c7501ad7b8bd5defa91f6ccc7050b"
    "02206b9b7b4734c3a38c047422d1ad97fad16d40f71a37d082e52e7888520f9facad";
constexpr char message[] =
    "BABYTECH-OTA-V1\nmotion\nesp32-s3-n16r8\n2\nhost-fixture-v2\n1024\n"
    "9b6ce55f379e9771551de6939556a7e6b949814ae27c2f5cfd5dbeb378ce7c2a\n";
std::string admin;

std::string hex(const uint8_t* bytes, size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (size_t i = 0; i < size; ++i) { result += digits[bytes[i] >> 4]; result += digits[bytes[i] & 15]; }
    return result;
}
void begin() {
    fake_motion_ota::updateAvailable = true;
    motion_main_crypto::expectedTestPem = kBabytechOtaPublicKey;
    motion_main_crypto::replacementTestPem = publicKey;
    motion_main_http::savedDebugProfile(); setup();
    const auto before = motion_io::usbTx.size();
    const std::string input = "OTA CODE\n";
    motion_io::usbRx.insert(motion_io::usbRx.end(), input.begin(), input.end()); tick(12);
    const auto response = motion_io::usbTx.substr(before);
    const std::string prefix = "OTA administrator code: ";
    const auto at = response.find(prefix);
    check(motion_io::usbRx.empty() && at != std::string::npos, "actual USB OTA CODE response missing");
    admin = response.substr(at + prefix.size(), 32);
    check(admin.size() == 32 && admin.find_first_not_of("0123456789abcdef") == std::string::npos,
          "host-only administrator code invalid");
    check(safeForOta() && !fake_motion_ota::update.beginCalls, "idle OTA fixture not safe");
}
WebServer::Arguments manifest() {
    return {{"board", "motion"}, {"hardware", "esp32-s3-n16r8"}, {"build", "2"},
            {"version", "host-fixture-v2"}, {"size", "1024"}, {"sha256", hash}, {"signature", signature}};
}
std::string challenge(uint32_t* createdAt = nullptr) {
    motion_io::freezeClock = true;
    const uint32_t at = motion_io::now + 10;
    const auto r = http(HTTP_GET, "/api/ota/challenge");
    motion_io::freezeClock = false;
    if (createdAt) *createdAt = at;
    check(r.status == 200, "OTA challenge not available");
    const std::string nonce = parseJson(r.body)["nonce"].as<std::string>();
    check(nonce.size() == 32 && nonce.find_first_not_of("0123456789abcdef") == std::string::npos,
          "OTA nonce malformed");
    return nonce;
}
std::string canonical(const WebServer::Arguments& args) {
    std::string value = "BABYTECH-OTA-V1\n";
    for (const auto* field : {"board", "hardware", "build", "version", "size", "sha256"}) {
        auto found = std::find_if(args.begin(), args.end(), [field](const auto& arg) { return arg.first == field; });
        check(found != args.end(), "canonical manifest field missing");
        value += found->second + "\n";
    }
    return value;
}
WebServer::Arguments authenticated(const std::string& nonce, WebServer::Arguments args = manifest()) {
    const auto data = std::string("BABYTECH-OTA-AUTH-V1\n") + nonce + "\n" + canonical(args);
    uint8_t proof[32];
    check(mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
          reinterpret_cast<const uint8_t*>(admin.data()), admin.size(),
          reinterpret_cast<const uint8_t*>(data.data()), data.size(), proof) == 0, "host administrator HMAC failed");
    args.push_back({"nonce", nonce}); args.push_back({"proof", hex(proof, sizeof proof)});
    return args;
}
void change(WebServer::Arguments& args, const char* name, std::string value) {
    for (auto& arg : args) if (arg.first == name) { arg.second = std::move(value); return; }
    throw std::runtime_error("unknown manifest fixture field");
}
void error(const WebServer::Response& r, int code, const char* expected) {
    check(r.status == code && parseJson(r.body)["error"] == expected, "OTA rejection code/reason mismatch");
}
std::string grant(const WebServer::Arguments& args, uint32_t& grantedAt) {
    // Clock control is SDK-only, so the exact grant time is observed, not guessed
    // from XMotor's query delays or the final delay(1).
    motion_io::freezeClock = true;
    grantedAt = motion_io::now + 10;
    const auto r = http(HTTP_POST, "/api/ota/session", args);
    motion_io::freezeClock = false;
    check(r.status == 200 && motion_io::now == grantedAt && ota.maintenanceActive(), "signed fixture session refused");
    const auto doc = parseJson(r.body);
    const auto token = doc["session"].as<std::string>();
    check(token.size() == 32 && doc["expiresInMs"] == 120000 && motion_main_crypto::testKeySubstitutions != 0,
          "session token/expiry or explicit trust fixture missing");
    return token;
}
std::string session(uint32_t& grantedAt) { return grant(authenticated(challenge()), grantedAt); }
WebServer::Response dispatch(WebServer::Request r) {
    const auto expectedCalls = r.uploads.size();
    const auto complete = r.complete;
    const auto before = server.responses.size(); server.enqueue(std::move(r)); tick();
    check(server.responses.size() == before + 1 && server.responses.back().handlerCalled == complete &&
          server.responses.back().sendCalls == (complete ? 1u : 0u) &&
          server.responses.back().uploadCalls == expectedCalls && !server.pendingRequests(),
          "multipart callbacks/final handler/response mismatch");
    return server.responses.back();
}
WebServer::Response upload(const std::string& token, IPAddress ip, bool withData,
                          unsigned mode = 0, const char* field = "firmware") {
    WebServer::Request r; r.method = HTTP_POST; r.uri = "/api/ota/image"; r.remote = ip;
    r.headers["X-OTA-Session"] = token;
    r.uploads.push_back({UPLOAD_FILE_START, {}, 0, field});
    if (withData) {
        std::vector<uint8_t> bytes(mode == 3 ? 256 : 1024, 0x42);
        if (mode == 2) bytes.back() ^= 1;
        const auto length = bytes.size();
        r.uploads.push_back({UPLOAD_FILE_WRITE, std::move(bytes), length, field});
    }
    r.uploads.push_back({mode == 3 ? UPLOAD_FILE_ABORTED : UPLOAD_FILE_END, {}, withData ? (mode == 3 ? 256u : 1024u) : 0u, field});
    r.complete = mode != 3;
    return dispatch(std::move(r));
}
void untouched(const decltype(fake_brain::io.disk)& disk, uint32_t generation) {
    check(fake_brain::io.disk == disk && motor.movementGeneration() == generation &&
          productState.state().pendingResultCount == 0 && !productRuntime.ownsMotion() &&
          !motion_io::restarts, "OTA route changed business state/motion or rebooted");
}
void auth() {
    begin(); const auto saved = fake_brain::io.disk;
    const auto generation = motor.movementGeneration();
    check(canonical(manifest()) == message, "offline signed manifest differs from actual canonical fields");
    motion_main_crypto::replacementTestPem = nullptr;
    error(http(HTTP_POST, "/api/ota/session", authenticated(challenge())), 403, "invalid_signature");
    check(motion_main_crypto::testKeySubstitutions == 0, "disabled trust substitution was used");
    motion_main_crypto::replacementTestPem = publicKey;
    auto differentPem = std::string(kBabytechOtaPublicKey);
    differentPem[32] ^= 1;
    motion_main_crypto::expectedTestPem = differentPem.c_str();
    error(http(HTTP_POST, "/api/ota/session", authenticated(challenge())), 403, "invalid_signature");
    check(motion_main_crypto::testKeySubstitutions == 0, "wrong expected PEM matched substitution");
    motion_main_crypto::expectedTestPem = kBabytechOtaPublicKey;
    motion_main_crypto::replacementTestPem = "malformed-test-key";
    error(http(HTTP_POST, "/api/ota/session", authenticated(challenge())), 403, "invalid_signature");
    motion_main_crypto::replacementTestPem = kBabytechOtaPublicKey;
    const auto parsedBefore = motion_main_crypto::nativeParseSuccesses;
    const auto verifiedBefore = motion_main_crypto::nativeVerifyCalls;
    error(http(HTTP_POST, "/api/ota/session", authenticated(challenge())), 403, "invalid_signature");
    check(motion_main_crypto::testKeySubstitutions == 1 &&
          motion_main_crypto::nativeParseSuccesses == parsedBefore + 1 &&
          motion_main_crypto::nativeVerifyCalls == verifiedBefore + 1,
          "real wrong-key parsing/verification was not reached");
    motion_main_crypto::replacementTestPem = publicKey;
    const auto consumedNonce = challenge();
    auto args = authenticated(consumedNonce); change(args, "proof", std::string(64, '0'));
    error(http(HTTP_POST, "/api/ota/session", args), 403, "invalid_admin_proof");
    error(http(HTTP_POST, "/api/ota/session", authenticated(consumedNonce)), 403, "challenge_expired");
    const auto signatureNonce = challenge();
    args = authenticated(signatureNonce); auto badSignature = std::string(signature); badSignature.back() ^= 1;
    change(args, "signature", badSignature);
    error(http(HTTP_POST, "/api/ota/session", args), 403, "invalid_signature");
    error(http(HTTP_POST, "/api/ota/session", authenticated(signatureNonce)), 403, "challenge_expired");
    args = authenticated(challenge()); auto malformed = std::string(signature); malformed[1] = '1';
    change(args, "signature", malformed);
    error(http(HTTP_POST, "/api/ota/session", args), 403, "invalid_signature");
    auto altered = manifest(); auto alteredHash = std::string(hash); alteredHash[0] = '8';
    change(altered, "sha256", alteredHash);
    error(http(HTTP_POST, "/api/ota/session", authenticated(challenge(), altered)), 403, "invalid_signature");
    const auto preservedNonce = challenge();
    args = authenticated(preservedNonce);
    change(args, "nonce", preservedNonce == std::string(32, '0') ? std::string(32, '1') : std::string(32, '0'));
    error(http(HTTP_POST, "/api/ota/session", args), 403, "challenge_expired");
    args = authenticated(preservedNonce); change(args, "board", "brain");
    error(http(HTTP_POST, "/api/ota/session", args), 400, "invalid_manifest");
    uint32_t grantedAt; const auto token = grant(authenticated(preservedNonce), grantedAt);
    error(http(HTTP_POST, "/api/ota/session", authenticated(challenge())), 409, "ota_busy");
    error(upload("wrong-token", IPAddress(192,168,4,2), false), 403, "invalid_session");
    error(upload("", IPAddress(192,168,4,2), false), 403, "invalid_session");
    error(upload(token, IPAddress(192,168,4,3), false), 403, "invalid_session");
    error(upload(token, IPAddress(192,168,4,2), false, 0, "wrong-field"), 403, "invalid_session");
    check(ota.maintenanceActive() && fake_motion_ota::update.beginCalls == 0 &&
          fake_motion_ota::update.writeCalls == 0 && fake_motion_ota::update.endCalls == 0 &&
          fake_motion_ota::update.abortCalls == 0, "invalid upload consumed/wrote session");
    error(upload(token, IPAddress(192,168,4,2), false), 422, "flash_begin_failed");
    check(fake_motion_ota::update.beginCalls == 1 && fake_motion_ota::update.abortCalls == 0 &&
          fake_motion_ota::update.writeCalls == 0 && fake_motion_ota::update.endCalls == 0 && !fake_motion_ota::update.running &&
          !ota.maintenanceActive(), "Flash begin failure did not release reservation");
    untouched(saved, generation);
    motion_main_http::expect({"/api/polling", {{"enabled", "0"}}}, 200);
    uint32_t retryAt; const auto retry = session(retryAt);
    check(retry != token && parseJson(http(HTTP_GET, "/api/ota/status").body)["error"] == "",
          "failure did not allow fresh authenticated reservation");
    untouched(saved, generation);
}
void controls(v4::Pairing pair, bool wrap) {
    begin(); BrainPeer peer(pair); peer.connect();
    if (wrap) motion_io::now = UINT32_MAX - 30000;
    uint32_t grantedAt; const auto token = session(grantedAt);
    const auto saved = fake_brain::io.disk; const auto generation = motor.movementGeneration();
    const auto before = motion_io::canTx.size();
    motion_main_http::reads();
    for (const auto& route : std::array<motion_main_http::Route, 22>{{
            {"/api/enable", {{"id", "1"}, {"enabled", "1"}}},
            {"/api/enable", {{"id", "1"}, {"enabled", "0"}}},
            {"/api/enable-all", {{"enabled", "1"}}},
            {"/api/enable-all", {{"enabled", "0"}}},
            {"/api/command", {{"hex", "011F6B"}}},
            {"/api/command", {{"hex", "01FE98006B"}}},
            {"/api/command", {{"hex", "019C486B"}}},
            {"/api/command", {{"hex", "01F3AB00006B"}}},
            {"/api/command", {{"hex", "01F3AB01006B"}}},
            {"/api/move", {}}, {"/api/limits", {}}, {"/api/control/reset", {}},
            {"/api/queue/start", {{"program", "wait 100"}, {"repeat", "1"}}},
            {"/api/polling", {{"enabled", "0"}}},
            {"/api/scale/config", {}}, {"/api/scale/tare", {}}, {"/api/scale/calibrate", {}},
            {"/api/query-budget", {}}, {"/api/sync-settings", {}},
            {"/api/motor-distance", {{"id", "1"}, {"rotationDistance", "8"}}},
            {"/api/demo/config", {{"json", demoConfigJson.c_str()}}},
            {"/api/demo/action", {{"action", "initialize"}}}
        }}) motion_main_http::expect(route, 409, "ota_active");
    for (size_t i = before; i < motion_io::canTx.size(); ++i)
        check(readOnlyQuery(motion_io::canTx[i]), "OTA reads/rejections sent cancellation/mutation");
    untouched(saved, generation);
    for (unsigned i = 0; i < 3; ++i) {
        const auto start = motion_io::canTx.size();
        motion_main_http::expect(motion_main_http::cancellations[i], i == 2 ? 200 : 202);
        std::vector<std::vector<uint8_t>> commands;
        for (size_t n = start; n < motion_io::canTx.size(); ++n) {
            const auto& f = motion_io::canTx[n];
            if (readOnlyQuery(f)) continue;
            check(f.extd && !f.rtr && f.identifier == (i == 0 ? 1u << 8 : 0u), "dedicated Stop target/flags mismatch");
            commands.emplace_back(f.data, f.data + f.data_length_code);
        }
        const std::vector<std::vector<uint8_t>> expected = i == 0
            ? std::vector<std::vector<uint8_t>>{{0xfe,0x98,0,0x6b}}
            : std::vector<std::vector<uint8_t>>{{0x9c,0x48,0x6b},{0xfe,0x98,0,0x6b}};
        check(commands == expected, "dedicated Stop payload/DLC/count/order mismatch");
    }
    check(ota.maintenanceActive() && motion_main_http::frameSince(before, 1, {0xfe,0x98,0,0x6b}) &&
          motion_main_http::frameSince(before, 0, {0x9c,0x48,0x6b}) &&
          motion_main_http::frameSince(before, 0, {0xfe,0x98,0,0x6b}), "dedicated Stop lost OTA availability");
    motion_main_http::onlyReadOrStop(before, true);
    untouched(saved, generation);
    motion_io::freezeClock = true; motion_io::now = grantedAt + 119999; loop();
    check(ota.maintenanceActive(), "OTA session expired before 120000 ms");
    // Dispatch START at the deadline before the same loop's ota.poll().
    // This independently checks sessionMatches(), not just Ready cleanup.
    motion_io::now = grantedAt + 120000 - 10;
    error(upload(token, IPAddress(192,168,4,2), false), 403, "invalid_session");
    motion_io::freezeClock = false;
    check(fake_motion_ota::update.beginCalls == 0, "expired START reached Flash begin");
    check(!ota.maintenanceActive(), "OTA session did not expire at 120000 ms");
    motion_main_http::expect({"/api/polling", {{"enabled", "0"}}}, 200);
    untouched(saved, generation);
}
void imageFailure(unsigned mode) {
    begin(); uint32_t at; const auto token = session(at);
    const auto saved = fake_brain::io.disk; const auto generation = motor.movementGeneration();
    fake_motion_ota::update.allowBegin = true; // SDK receipt/write accounting, never successful staging.
    if (mode == 0) fake_motion_ota::update.writeLimit = 511;
    const auto r = upload(token, IPAddress(192,168,4,2), true, mode);
    if (mode == 3) check(r.status == 0, "disconnected abort fabricated HTTP response");
    else error(r, 422, mode == 0 ? "flash_write_failed" : mode == 2 ? "image_mismatch" : "image_invalid");
    check(fake_motion_ota::update.beginCalls == 1 && fake_motion_ota::update.writeCalls == 1 &&
          fake_motion_ota::update.endCalls == (mode == 1 ? 1u : 0u) &&
          fake_motion_ota::update.abortCalls == (mode == 1 ? 0u : 1u) &&
          !fake_motion_ota::update.running && !ota.maintenanceActive(), "failed upload leaked Flash/session ownership");
    check(fake_motion_ota::update.received == (mode == 0 ? 511u : mode == 3 ? 256u : 1024u),
          "SDK uploaded byte count differs from failure fixture");
    const auto failedStatus = parseJson(http(HTTP_GET, "/api/ota/status").body);
    const char* failure = mode == 0 ? "flash_write_failed" : mode == 1 ? "image_invalid" :
                          mode == 2 ? "image_mismatch" : "upload_aborted";
    check(failedStatus["state"] == "failed" && failedStatus["error"] == failure,
          "failed upload lost its failure state/reason before fresh authentication");
    untouched(saved, generation);
    motion_main_http::expect({"/api/polling", {{"enabled", "0"}}}, 200);
    uint32_t retryAt; const auto retry = session(retryAt);
    const auto status = parseJson(http(HTTP_GET, "/api/ota/status").body);
    check(retry != token && status["error"] == "" && status["received"] == 0 && status["state"] == "ready",
          "fresh authentication after failure did not reset reservation");
    untouched(saved, generation);
}
void unsafeStart() {
    begin(); uint32_t at; const auto token = session(at);
    const auto saved = fake_brain::io.disk; const auto generation = motor.movementGeneration();
    motion_main_http::expect({"/api/stop", {{"id", "1"}}}, 202);
    check(!safeForOta(), "Stop without feedback did not invalidate OTA safety");
    error(upload(token, IPAddress(192,168,4,2), false), 403, "machine_not_safe");
    check(ota.maintenanceActive() && !fake_motion_ota::update.beginCalls,
          "unsafe START wrote Flash or discarded valid reservation");
    motion_main_http::stationaryReplies();
    check(safeForOta(), "new Stop feedback did not restore OTA start safety");
    error(upload(token, IPAddress(192,168,4,2), false), 422, "flash_begin_failed");
    check(fake_motion_ota::update.beginCalls == 1 && !ota.maintenanceActive(), "recovered START did not reach SDK begin failure");
    untouched(saved, generation);
}
void challengeExpiry() {
    begin(); const auto saved = fake_brain::io.disk; const auto generation = motor.movementGeneration();
    uint32_t at; auto nonce = challenge(&at); auto args = authenticated(nonce);
    change(args, "proof", std::string(64, '0'));
    motion_io::freezeClock = true; motion_io::now = at + 59999 - 10;
    error(http(HTTP_POST, "/api/ota/session", args), 403, "invalid_admin_proof");
    motion_io::freezeClock = false;
    nonce = challenge(&at); args = authenticated(nonce);
    motion_io::freezeClock = true; motion_io::now = at + 60000 - 10;
    error(http(HTTP_POST, "/api/ota/session", args), 403, "challenge_expired");
    motion_io::freezeClock = false;
    check(!ota.maintenanceActive() && !fake_motion_ota::update.beginCalls &&
          motion_main_crypto::testKeySubstitutions == 0, "expired nonce reached signature/Flash");
    uint32_t retryAt; session(retryAt);
    untouched(saved, generation);
}
} // namespace motion_main_ota
