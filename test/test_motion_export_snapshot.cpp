#include "MotionExportSnapshot.h"
#include "MaintenanceExport.h"
#include "RecordBytes.h"
#include "FakeCommissioning.h"
#include "FakeProductCrypto.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

using namespace babytech::boardlink;
namespace v4 = babytech::v4;
namespace fake = fake_brain;
using Bytes = std::vector<uint8_t>;

namespace allocation_fault {
unsigned remaining = 0;
bool hit = false;
bool deny() {
    if (!remaining || --remaining) return false;
    hit = true;
    return true;
}
}
// Fail only explicitly checked production allocations, not test containers.
void* operator new(size_t size, const std::nothrow_t&) noexcept {
    if (allocation_fault::deny()) return nullptr;
    try { return ::operator new(size); } catch (...) { return nullptr; }
}
void* operator new[](size_t size, const std::nothrow_t&) noexcept {
    if (allocation_fault::deny()) return nullptr;
    try { return ::operator new[](size); } catch (...) { return nullptr; }
}

namespace {
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + \
    std::to_string(__LINE__) + ": " #x); } while (false)
unsigned accepts = 0, rejects = 0;
const std::string device(64, 'D');
constexpr const char* physical = "012345abcdef";
constexpr const char* challenge = "0123456789abcdef0123456789abcdef";
constexpr uint64_t boot = UINT64_C(0xfedcba9876543210);
constexpr const char* prefix = "[maint-export] ";

std::string utf8(size_t length) {
    CHECK(length % 2 == 0);
    std::string value;
    while (value.size() < length) value += "\xc3\xa9";
    return value;
}
v4::Pairing pair() {
    v4::Pairing p;
    p.role = v4::Role::Motion;
    std::strcpy(p.deviceId, device.c_str());
    std::strcpy(p.epoch, challenge);
    std::strcpy(p.localPhysicalId, physical);
    std::strcpy(p.peerPhysicalId, "fedcba987654");
    return p;
}
ProductContext context(bool cleared = false) {
    ProductContext c;
    std::strcpy(c.deviceId, device.c_str());
    c.profileVersion = INT32_MAX;
    c.cleared = cleared;
    if (!cleared) {
        std::strcpy(c.babyId, utf8(96).c_str());
        std::strcpy(c.babyName, utf8(320).c_str());
        std::strcpy(c.formulaBrand, utf8(480).c_str());
        c.waterMl = 500;
        c.temperatureC = 60;
        c.powderGPer100Ml = 50;
    }
    CHECK(validProductContext(c));
    return c;
}
ProductRequest request(uint64_t seq, bool local = false) {
    const auto c = context();
    ProductRequest r;
    r.source = local ? v4::Source::LocalTouch : v4::Source::CloudCommand;
    r.command = local ? ProductCommand::Initialize : ProductCommand::Prepare;
    r.sequence = seq;
    std::strcpy(r.deviceId, device.c_str());
    if (local) {
        auto brain = pair();
        brain.role = v4::Role::Brain;
        std::swap(brain.localPhysicalId, brain.peerPhysicalId);
        CHECK(makeLocalCommandId(brain, seq, r.commandId));
    } else {
        const auto suffix = std::to_string(seq);
        std::strcpy(r.commandId, (std::string(128 - suffix.size(), 'c') + suffix).c_str());
        CHECK(std::strlen(r.commandId) == 128);
        std::strcpy(r.babyId, c.babyId);
        r.profileVersion = c.profileVersion;
        r.waterMl = c.waterMl;
        r.temperatureC = c.temperatureC;
        r.powderGPer100Ml = c.powderGPer100Ml;
    }
    CHECK(validProductRequest(r));
    return r;
}
MotionState fullState() {
    MotionState s;
    s.pairing = pair();
    CHECK(makeMotionContextBarrier(context(), s.context));
    s.cloudSequence = s.localSequence = v4::kMaxSequence;
    s.cloudResult.kind = s.localResult.kind = MotionResultKind::Ordinary;
    s.cloudResult.request = request(s.cloudSequence);
    CHECK(requestDigest(s.cloudResult.request, s.cloudResult.digest));
    std::strcpy(s.cloudResult.reason, std::string(64, 'r').c_str());
    s.localResult.request = request(s.localSequence, true);
    s.localResult.accepted = true;
    CHECK(requestDigest(s.localResult.request, s.localResult.digest));
    std::strcpy(s.localResult.reason, "accepted");
    s.slot.kind = MotionSlotKind::Intent;
    s.slot.request = s.localResult.request;
    std::memcpy(s.slot.digest, s.localResult.digest, sizeof(s.slot.digest));
    std::strcpy(s.slot.executionId, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    s.pendingResultCount = kMotionResultQueueCapacity;
    for (size_t n = 0; n < kMotionResultQueueCapacity; ++n) {
        auto& slot = s.pendingResults[n];
        slot.kind = MotionSlotKind::Terminal;
        slot.request = request(v4::kMaxSequence - 5 + n);
        CHECK(requestDigest(slot.request, slot.digest));
        std::snprintf(slot.executionId, sizeof(slot.executionId), "%032llx",
                      static_cast<unsigned long long>(slot.request.sequence));
        CHECK(makeProductEventId(s.pairing, slot.request.source, slot.request.sequence, slot.eventId));
        slot.targetPowderG = productTargetPowderG(slot.request);
        slot.uptimeMs = UINT32_MAX;
        std::strcpy(slot.reason, std::string(64, 'r').c_str());
        std::strcpy(slot.errorCode, std::string(64, 'e').c_str());
    }
    CHECK(validMotionState(s));
    return s;
}
template<class T> Bytes encode(const T& value, size_t capacity,
                              size_t (*encoder)(const T&, uint8_t*, size_t)) {
    Bytes bytes(capacity);
    const auto n = encoder(value, bytes.data(), bytes.size());
    CHECK(n && n <= capacity);
    bytes.resize(n);
    return bytes;
}
Bytes pairBytes(const v4::Pairing& p) { return encode(p, kPairingRecordMaxSize, encodePairingRecord); }
Bytes stateBytes(const MotionState& s) { return encode(s, kMotionStateMaxSize, encodeMotionState); }
Bytes contextBytes(const ProductContext& c) { return encode(c, kContextIdentityMaxSize, encodeContextIdentity); }
std::string hex(const Bytes& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (auto b : bytes) { text += digits[b >> 4]; text += digits[b & 15]; }
    return text;
}
std::string serialize(const JsonDocument& doc) {
    CHECK(!doc.overflowed());
    std::string text;
    serializeJson(doc, text);
    return std::string(prefix) + text + "\n";
}
std::string wire(const Bytes& p, const Bytes& s, const Bytes& c, uint32_t captured = UINT32_MAX) {
    DynamicJsonDocument doc(32768);
    doc["schema"] = 1;
    doc["role"] = "motion";
    doc["device_id"] = device;
    doc["physical_id"] = physical;
    doc["boot"] = "fedcba9876543210";
    doc["challenge"] = challenge;
    doc["captured_ms"] = captured;
    doc["pair_status"] = p.empty() ? "missing" : "ready";
    doc["state_status"] = s.empty() ? "missing" : "ready";
    doc["legacy_status"] = c.empty() ? "missing" : "ready";
    doc["legacy_event"] = "present";
    doc["pair_hex"] = hex(p);
    doc["state_hex"] = hex(s);
    doc["legacy_hex"] = hex(c);
    return serialize(doc);
}
std::string changed(const std::string& text, const char* key, const std::string& value) {
    DynamicJsonDocument doc(32768);
    CHECK(!deserializeJson(doc, text.c_str() + std::strlen(prefix)));
    doc[key] = value;
    return serialize(doc);
}
std::string token(std::string text, const char* key, const std::string& value) {
    const std::string start = std::string("\"") + key + "\":";
    const auto at = text.find(start);
    CHECK(at != std::string::npos);
    const auto begin = at + start.size();
    const auto end = text.find_first_of(",}", begin);
    CHECK(end != std::string::npos);
    text.replace(begin, end - begin, value);
    return text;
}
std::unique_ptr<MotionExportSnapshot> decoded(const std::string& text,
                                             uint64_t expectedBoot = boot) {
    // No spare terminator: exercise the length-bounded mutable API under ASan.
    std::vector<char> input(text.begin(), text.end());
    auto out = std::make_unique<MotionExportSnapshot>();
    const auto calls = fake::io.calls.size();
    CHECK(decodeMotionExport(input.data(), input.size(), device.c_str(), physical,
                             expectedBoot, challenge, *out));
    CHECK(std::memchr(input.data(), 0, input.size()));
    CHECK(fake::io.calls.size() == calls);
    CHECK(!std::strcmp(out->deviceId, device.c_str()) && !std::strcmp(out->physicalId, physical));
    CHECK(!std::strcmp(out->challenge, challenge) && out->boot == expectedBoot);
    ++accepts;
    return out;
}
void rejected(const std::string& text, const char* expectedDevice = nullptr,
              const char* expectedPhysical = physical, uint64_t expectedBoot = boot,
              const char* expectedChallenge = challenge) {
    auto out = std::make_unique<MotionExportSnapshot>();
    out->state.pairing = pair();
    out->state.cloudSequence = 987;
    out->state.pendingResultCount = 3;
    std::strcpy(out->state.pendingResults[2].errorCode, "previous_error");
    out->legacy = context();
    out->pairing = pair();
    out->boot = 79;
    out->capturedAtMs = 43;
    std::strcpy(out->deviceId, "previous_output");
    const auto* raw = reinterpret_cast<const uint8_t*>(out.get());
    const Bytes before(raw, raw + sizeof(*out));
    std::vector<char> input(text.begin(), text.end());
    const auto calls = fake::io.calls.size();
    CHECK(!decodeMotionExport(input.data(), input.size(), expectedDevice ? expectedDevice : device.c_str(),
                              expectedPhysical, expectedBoot, expectedChallenge, *out));
    CHECK(!std::memcmp(before.data(), out.get(), before.size()));
    CHECK(fake::io.calls.size() == calls);
    ++rejects;
}
void roundtrip(const std::string& text, const MotionState& s, const ProductContext& c) {
    const auto out = decoded(text);
    CHECK(out->pairStatus == ExportRead::Ready && out->stateStatus == ExportRead::Ready);
    CHECK(out->legacyStatus == ExportRead::Ready && out->legacyEvent == ExportRead::Present);
    CHECK(out->capturedAtMs == UINT32_MAX);
    CHECK(pairBytes(out->pairing) == pairBytes(s.pairing));
    CHECK(sameMotionState(out->state, s) && stateBytes(out->state) == stateBytes(s));
    CHECK(sameProductContext(out->legacy, c) && contextBytes(out->legacy) == contextBytes(c));
    CHECK(out->state.pendingResultCount == 4);
    for (size_t n = 0; n < 4; ++n) {
        const auto& a = out->state.pendingResults[n];
        const auto& b = s.pendingResults[n];
        CHECK(sameProductRequest(a.request, b.request));
        CHECK(!std::memcmp(a.digest, b.digest, sizeof(a.digest)));
        CHECK(!std::strcmp(a.executionId, b.executionId) && !std::strcmp(a.eventId, b.eventId));
        CHECK(a.kind == b.kind && a.targetPowderG == b.targetPowderG && a.completed == b.completed);
        CHECK(a.uptimeMs == b.uptimeMs && !std::strcmp(a.reason, b.reason) && !std::strcmp(a.errorCode, b.errorCode));
    }
}
std::string legacyJson(const ProductContext& c) {
    DynamicJsonDocument doc(4096);
    doc["type"] = "feeding_context";
    doc["device_id"] = c.deviceId;
    doc["profile_version"] = c.profileVersion;
    if (c.cleared) doc["cleared"] = true;
    else {
        doc["baby_id"] = c.babyId;
        doc["baby_name"] = c.babyName;
        doc["formula_brand"] = c.formulaBrand;
        doc["water_ml"] = c.waterMl;
        doc["temp"] = c.temperatureC;
        doc["powder_g_per_100ml"] = c.powderGPer100Ml;
    }
    std::string text;
    serializeJson(doc, text);
    CHECK(!doc.overflowed());
    return text;
}
fake::Value stringValue(const std::string& text) {
    Bytes bytes(text.begin(), text.end());
    bytes.push_back(0);
    return {bytes, fake::Type::String};
}
std::string emitted() {
    const auto before = fake::io.disk;
    const auto writes = fake::count(fake::Op::Set);
    auto exporter = std::make_unique<MaintenanceExport>();
    CHECK(exporter->begin(v4::Role::Motion, device.c_str(), challenge, boot, UINT32_MAX));
    std::string text;
    std::array<uint8_t, 73> chunk{};
    while (exporter->active()) {
        const auto n = exporter->peek(chunk.data(), chunk.size());
        CHECK(n);
        text.append(reinterpret_cast<const char*>(chunk.data()), n);
        exporter->consume(n);
    }
    CHECK(fake::io.disk == before && fake::count(fake::Op::Set) == writes);
    CHECK(!fake::count(fake::Op::OpenRW) && !fake::count(fake::Op::Commit));
    CHECK(fake::io.handles.empty());
    return text;
}
void exporterFixtures(const MotionState& s, const ProductContext& c) {
    const auto seed = [&] {
        fake::reset();
        fake_commissioning::reset();
        fake::io.disk["productpair"]["record"] = {pairBytes(s.pairing), fake::Type::Blob};
        fake::io.disk["productstate"]["record"] = {stateBytes(s), fake::Type::Blob};
        fake::io.disk["productctx"]["payload"] = stringValue(legacyJson(c));
        fake::io.disk["formulaevt"]["payload"] = stringValue("old event exists");
    };
    seed();
    roundtrip(emitted(), s, c);
    const auto cleared = context(true);
    fake::io.disk["productctx"]["payload"] = stringValue(legacyJson(cleared));
    CHECK(sameProductContext(decoded(emitted())->legacy, cleared));
    MotionState conflict;
    conflict.pairing = s.pairing;
    conflict.pairing.epoch[0] = 'f';
    fake::io.disk["productstate"]["record"] = {stateBytes(conflict), fake::Type::Blob};
    CHECK(decoded(emitted())->stateStatus == ExportRead::Conflict);
    fake::reset();
    const auto missing = decoded(emitted());
    CHECK(missing->pairStatus == ExportRead::Missing && missing->stateStatus == ExportRead::Missing);
    CHECK(missing->legacyStatus == ExportRead::Missing && missing->legacyEvent == ExportRead::Missing);
    // Real exporter namespace order: pairing, state, old event presence, legacy context.
    for (unsigned open = 1; open <= 4; ++open) {
        seed();
        fake::fail(fake::Op::OpenRO, open);
        const auto out = decoded(emitted());
        const ExportRead values[] = {out->pairStatus, out->stateStatus, out->legacyEvent, out->legacyStatus};
        CHECK(values[open - 1] == ExportRead::IoError);
        for (unsigned n = 0; n < 4; ++n)
            if (n != open - 1) CHECK(values[n] == (n == 2 ? ExportRead::Present : ExportRead::Ready));
        fake::verifyFaults();
    }
    for (unsigned field = 0; field < 4; ++field) {
        seed();
        if (field == 0) fake::io.disk["productpair"]["record"].bytes[8] ^= 1;
        if (field == 1) fake::io.disk["productstate"]["record"].bytes[8] ^= 1;
        if (field == 2) fake::io.disk["formulaevt"]["payload"] = stringValue("");
        if (field == 3) fake::io.disk["productctx"]["payload"] = stringValue("{bad}");
        const auto out = decoded(emitted());
        const ExportRead values[] = {out->pairStatus, out->stateStatus, out->legacyEvent, out->legacyStatus};
        CHECK(values[field] == ExportRead::Corrupt);
    }
}
void statuses(const Bytes& p, const Bytes& s, const Bytes& c) {
    struct Status { const char* text; ExportRead value; };
    const Status choices[] = {{"ready", ExportRead::Ready}, {"missing", ExportRead::Missing},
        {"corrupt", ExportRead::Corrupt}, {"io_error", ExportRead::IoError},
        {"identity_mismatch", ExportRead::IdentityMismatch}, {"conflict", ExportRead::Conflict},
        {"present", ExportRead::Present}, {"not_applicable", ExportRead::NotApplicable}};
    const char* keys[] = {"pair_status", "state_status", "legacy_status", "legacy_event"};
    const char* hexKeys[] = {"pair_hex", "state_hex", "legacy_hex"};
    const auto base = wire(p, s, c);
    for (size_t field = 0; field < 4; ++field) {
        for (const auto& status : choices) {
            auto text = changed(base, keys[field], status.text);
            if (field < 3 && status.value != ExportRead::Ready) text = changed(text, hexKeys[field], "");
            const bool allowed = status.value == ExportRead::Missing || status.value == ExportRead::Corrupt ||
                status.value == ExportRead::IoError || (status.value == ExportRead::Ready && field < 3) ||
                (status.value == ExportRead::IdentityMismatch && field < 2) ||
                (status.value == ExportRead::Conflict && field == 1) ||
                (status.value == ExportRead::Present && field == 3);
            if (!allowed) rejected(text);
            else {
                const auto out = decoded(text);
                const ExportRead values[] = {out->pairStatus, out->stateStatus, out->legacyStatus, out->legacyEvent};
                CHECK(values[field] == status.value);
                if (field < 3 && status.value != ExportRead::Ready) rejected(changed(text, hexKeys[field], "00"));
            }
        }
        for (const char* invalid : {"Ready", "unknown", "", "present ", "identity-mismatch"})
            rejected(changed(base, keys[field], invalid));
        for (const char* invalid : {"null", "true", "1", "[]", "{}"})
            rejected(token(base, keys[field], invalid));
    }
    // Every combination of the non-ready Motion-specific statuses is diagnostic data.
    const char* pairNames[] = {"missing", "corrupt", "io_error", "identity_mismatch"};
    const char* stateNames[] = {"missing", "corrupt", "io_error", "identity_mismatch", "conflict"};
    const char* legacyNames[] = {"missing", "corrupt", "io_error"};
    const char* eventNames[] = {"missing", "corrupt", "io_error", "present"};
    for (auto pn : pairNames) for (auto sn : stateNames) for (auto ln : legacyNames) for (auto en : eventNames) {
        auto text = changed(wire({}, {}, {}), "pair_status", pn);
        text = changed(text, "state_status", sn);
        text = changed(text, "legacy_status", ln);
        decoded(changed(text, "legacy_event", en));
    }
}
void identities(const std::string& base, const MotionState& s) {
    for (const char* key : {"device_id", "physical_id", "challenge", "role", "boot"}) {
        rejected(changed(base, key, ""));
        rejected(changed(base, key, "wrong"));
        for (const char* bad : {"null", "true", "1", "[]", "{}"}) rejected(token(base, key, bad));
    }
    rejected(changed(base, "role", "brain"));
    rejected(changed(base, "boot", "0000000000000000"));
    rejected(changed(base, "boot", "fedcba98765432100"));
    rejected(changed(base, "boot", "FEDCBA9876543210"));
    rejected(changed(base, "boot", "0000000000000001"));
    decoded(changed(base, "boot", "ffffffffffffffff"), UINT64_MAX);
    decoded(changed(base, "boot", "0000000000000001"), 1);
    rejected(base, "Different_device");
    rejected(base, "_invalid");
    rejected(base, "");
    rejected(base, std::string(65, 'D').c_str());
    rejected(base, nullptr, "000000000000");
    rejected(base, nullptr, "012345ABCDEF");
    rejected(base, nullptr, "012345abcde");
    rejected(base, nullptr, nullptr);
    rejected(base, nullptr, physical, 0);
    rejected(base, nullptr, physical, boot + 1);
    rejected(base, nullptr, physical, boot, "00000000000000000000000000000000");
    rejected(base, nullptr, physical, boot, nullptr);
    rejected(base, nullptr, physical, boot, "0123");
    for (unsigned variant = 0; variant < 5; ++variant) {
        auto p = pair();
        if (variant == 0) p.role = v4::Role::Brain;
        if (variant == 1) std::strcpy(p.deviceId, "Different_device");
        if (variant == 2) std::strcpy(p.localPhysicalId, "112345abcdef");
        if (variant == 3) p.epoch[0] = 'f';
        if (variant == 4) p.peerPhysicalId[0] = 'a';
        rejected(changed(base, "pair_hex", hex(pairBytes(p))));
        if (variant < 3) rejected(wire(pairBytes(p), {}, {}));
        else CHECK(decoded(wire(pairBytes(p), {}, {}))->pairStatus == ExportRead::Ready);
        if (variant == 0) continue; // MotionState's own codec already rejects Brain role.
        MotionState empty;
        empty.pairing = p;
        rejected(changed(base, "state_hex", hex(stateBytes(empty))));
        if (variant < 3) rejected(wire({}, stateBytes(empty), {}));
        else CHECK(decoded(wire({}, stateBytes(empty), {}))->stateStatus == ExportRead::Ready);
    }
    for (bool cleared : {false, true}) {
        auto c = context(cleared);
        std::strcpy(c.deviceId, "Other_device");
        rejected(changed(base, "legacy_hex", hex(contextBytes(c))));
    }
    // Neither legacy profile ordering nor captured time is installation authority.
    auto old = context();
    old.profileVersion = 1;
    CHECK(decoded(wire(pairBytes(s.pairing), stateBytes(s), contextBytes(old)))->legacy.profileVersion == 1);
}
void malformed(const std::string& base, const Bytes& p, const Bytes& s, const Bytes& c) {
    const char* keys[] = {"pair_hex", "state_hex", "legacy_hex"};
    const Bytes blobs[] = {p, s, c};
    const size_t limits[] = {kPairingRecordMaxSize, kMotionStateMaxSize, kContextIdentityMaxSize};
    for (size_t field = 0; field < 3; ++field) {
        for (const auto& bad : {std::string(), std::string("0"), std::string("gg"), std::string("00"),
             std::string(" 00"), std::string("0x00"), std::string(2 * (limits[field] + 1), '0')})
            rejected(changed(base, keys[field], bad));
        for (const char* bad : {"null", "true", "42", "[]", "{}"}) rejected(token(base, keys[field], bad));
        auto damaged = blobs[field];
        damaged[0] ^= 1;
        rejected(changed(base, keys[field], hex(damaged)));
        damaged = blobs[field];
        damaged.push_back(0);
        rejected(changed(base, keys[field], hex(damaged)));
        damaged = blobs[field];
        damaged.pop_back();
        rejected(changed(base, keys[field], hex(damaged)));
        auto uppercase = hex(blobs[field]);
        const auto at = uppercase.find_first_of("abcdef");
        CHECK(at != std::string::npos);
        uppercase[at] -= 'a' - 'A';
        rejected(changed(base, keys[field], uppercase));
    }
    auto damaged = s;
    damaged[8] ^= 1;
    rejected(changed(base, "state_hex", hex(damaged)));
    for (const char* key : {"schema", "captured_ms"}) {
        for (const char* bad : {"true", "false", "null", "\"1\"", "[]", "{}", "-1", "1.0", "1e0", "01", "+1", "4294967296", "18446744073709551616"})
            rejected(token(base, key, bad));
    }
    for (const char* bad : {"0", "2", "4294967295"}) rejected(token(base, "schema", bad));
    for (const char* valid : {"0", "1", "2147483648", "4294967295"})
        CHECK(decoded(token(base, "captured_ms", valid))->capturedAtMs == std::stoull(valid));
    auto out = decoded(token(base, "captured_ms", "4294967295"));
    std::vector<char> next;
    // Same destination may move across millis rollover; no newness policy here.
    const auto zero = token(base, "captured_ms", "0");
    next.assign(zero.begin(), zero.end());
    CHECK(decodeMotionExport(next.data(), next.size(), device.c_str(), physical, boot, challenge, *out));
    CHECK(out->capturedAtMs == 0);
    ++accepts;

    const char* allKeys[] = {"schema", "role", "device_id", "physical_id", "boot", "challenge", "captured_ms",
        "pair_status", "state_status", "legacy_status", "legacy_event", "pair_hex", "state_hex", "legacy_hex"};
    for (const char* key : allKeys) {
        DynamicJsonDocument doc(32768);
        CHECK(!deserializeJson(doc, base.c_str() + std::strlen(prefix)));
        doc.remove(key);
        rejected(serialize(doc));
        doc["extra"] = "unexpected"; // still exactly 14 fields, but the wrong set
        rejected(serialize(doc));
    }
    auto extra = base;
    extra.insert(extra.size() - 2, ",\"extra\":true");
    rejected(extra);
    extra = base;
    extra.insert(extra.size() - 2, ",\"schema\":1");
    rejected(extra);
    extra = base;
    extra.insert(extra.size() - 2, ",\"\\u0073chema\":1");
    rejected(extra);
    rejected(token(base, "role", "\"motion\\u0000brain\""));
    rejected(token(base, "role", "\"motion\\ud800\""));
    rejected(token(base, "pair_hex", "\"\\u0000\""));
    auto nul = base;
    nul[nul.size() / 2] = 0;
    rejected(nul);
    rejected(base + "{}\n");
    rejected(base.substr(0, base.size() - 1) + " {}\n");
    rejected(base.substr(0, base.size() - 1) + " false\n");
    rejected(base.substr(0, base.size() - 1) + " /comment/\n");
    decoded(base.substr(0, base.size() - 1) + " \t\n");
    rejected(base.substr(1));
    rejected("[maint-export] []\n");
    rejected("[maint-export] {schema:1}\n");
    rejected(base.substr(0, base.size() - 2) + ",}\n");
    rejected(base.substr(0, base.size() - 1) + "\r\n");
    extra = base;
    extra.insert(std::strlen(prefix) + 1, "\n");
    rejected(extra);
    // Every cut, both missing terminator and a transport falsely declaring completion.
    for (size_t n = 0; n < base.size() - 1; ++n) {
        rejected(base.substr(0, n));
        rejected(base.substr(0, n) + "\n");
    }
    rejected(base.substr(0, base.size() - 1));
    auto padded = base;
    CHECK(padded.size() < kMotionExportWireMaxSize);
    padded.insert(std::strlen(prefix), kMotionExportWireMaxSize - padded.size(), ' ');
    decoded(padded);
    padded.insert(std::strlen(prefix), 1, ' ');
    rejected(padded);
}
void v1Compatibility() {
    MotionState empty;
    empty.pairing = pair();
    auto bytes = stateBytes(empty);
    CHECK(bytes.back() == 0);
    bytes.pop_back(); // BMS1 ends at the slot, before the BMS2 queue count.
    babytech::boardlink::detail::finishRecord(bytes.data(), bytes.size(), "BMS1");
    const auto out = decoded(wire(pairBytes(empty.pairing), bytes, contextBytes(context(true))));
    CHECK(sameMotionState(out->state, empty) && !out->state.pendingResultCount);
    CHECK(out->legacy.cleared);
    auto populated = fullState();
    populated.pendingResultCount = 0;
    for (auto& item : populated.pendingResults) item = MotionExecutionSlot{};
    auto fullV1 = stateBytes(populated);
    CHECK(fullV1.back() == 0);
    fullV1.pop_back();
    babytech::boardlink::detail::finishRecord(fullV1.data(), fullV1.size(), "BMS1");
    const auto fullOut = decoded(wire(pairBytes(populated.pairing), fullV1, contextBytes(context())));
    CHECK(sameMotionState(fullOut->state, populated));
    auto oversized = bytes;
    oversized.resize(kMotionStateV1MaxSize + 1);
    babytech::boardlink::detail::finishRecord(oversized.data(), oversized.size(), "BMS1");
    rejected(wire({}, oversized, {}));
}
}

int main() {
    const char* stage = "maximum fixtures";
    try {
        fake_product_crypto::reset();
        const auto s = fullState();
        const auto c = context();
        const auto pbytes = pairBytes(s.pairing), sbytes = stateBytes(s), cbytes = contextBytes(c);
        CHECK(pbytes.size() == 134 && cbytes.size() == kContextIdentityMaxSize);
        CHECK(encode(s.cloudResult.request, kRequestIdentityMaxSize, encodeRequestIdentity).size() == kRequestIdentityMaxSize);
        const auto base = wire(pbytes, sbytes, cbytes);
        roundtrip(base, s, c);
        stage = "production exporter fixtures";
        exporterFixtures(s, c);
        stage = "statuses";
        statuses(pbytes, sbytes, cbytes);
        stage = "identities";
        identities(base, s);
        stage = "malformed inputs and bounds";
        malformed(base, pbytes, sbytes, cbytes);
        stage = "BMS1 compatibility";
        v1Compatibility();
        stage = "null inputs and crypto failures";
        auto out = std::make_unique<MotionExportSnapshot>();
        out->boot = 9;
        CHECK(!decodeMotionExport(nullptr, base.size(), device.c_str(), physical, boot, challenge, *out));
        CHECK(out->boot == 9);
        std::vector<char> input(base.begin(), base.end());
        CHECK(!decodeMotionExport(input.data(), input.size(), nullptr, physical, boot, challenge, *out));
        CHECK(out->boot == 9);
        stage = "checked heap allocation failures";
        // Candidate snapshot, bounded hex scratch, then the BMS codec candidate.
        for (unsigned n = 1; n <= 3; ++n) {
            allocation_fault::remaining = n;
            allocation_fault::hit = false;
            rejected(base);
            CHECK(allocation_fault::hit && !allocation_fault::remaining);
        }
        decoded(base);
        stage = "crypto failure";
        fake_product_crypto::fail = true;
        rejected(base);
        fake_product_crypto::reset();
        std::printf("MotionExportSnapshot: %u accepts, %u atomic rejections; maximum UTF-8/context/request, "
                    "four-result BMS2, BMS1, production exports and boundaries passed (state=%zu, wire=%zu bytes)\n",
                    accepts, rejects, sbytes.size(), base.size());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL (%s): %s\n", stage, error.what());
        return 1;
    }
}
