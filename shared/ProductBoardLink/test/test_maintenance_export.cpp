#include "MaintenanceUsbConsole.h"
#include "BrainStateRecord.h"
#include "FakeCommissioning.h"
#include "FakeProductCrypto.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace babytech::boardlink;
namespace v4 = babytech::v4;
namespace fake = fake_brain;
namespace mac = fake_commissioning;
using fake::Bytes;
using fake::Op;
using fake::io;

namespace {
unsigned scenarios = 0, failures = 0;
bool motionAudit = false;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + \
    std::to_string(__LINE__) + ": " #x); } while (false)
constexpr const char* device = "Babytech_01-test";
constexpr const char* challenge = "0123456789abcdef0123456789abcdef";
constexpr const char* physical = "012345abcdef";
constexpr const char* secret = "DO_NOT_EXPORT_CREDENTIAL_79f2";
constexpr uint64_t boot = UINT64_C(0xfedcba9876543210);
constexpr uint32_t captured = 123;
const std::string prefix = "[maint-export] ";
const char* stateSpace(bool motion) { return motion ? "productstate" : "brainstate"; }
v4::Role role(bool motion) { return motion ? v4::Role::Motion : v4::Role::Brain; }

void readOnlyCall(const fake::Call& call) {
    CHECK(call.op == Op::OpenRO || call.op == Op::Query || call.op == Op::Read || call.op == Op::Close);
    CHECK(call.name == "productpair" || call.name == stateSpace(motionAudit) ||
          (motionAudit && (call.name == "productctx" || call.name == "formulaevt")));
    if (call.op == Op::Query || call.op == Op::Read) {
        CHECK(call.key == ((call.name == "productctx" || call.name == "formulaevt") ? "payload" : "record"));
        // The old journal is presence-only, never read or presented as decoded.
        CHECK(call.name != "formulaevt" || call.op == Op::Query);
    }
}

void audit() {
    CHECK(io.handles.empty());
    for (const auto op : {Op::OpenRW, Op::Set, Op::Commit, Op::Erase, Op::Init}) CHECK(fake::count(op) == 0);
    for (const auto& call : io.calls) readOnlyCall(call);
    fake::verifyFaults();
}

fake::Value stringValue(const std::string& text) {
    Bytes bytes(text.begin(), text.end());
    bytes.push_back(0);
    return {bytes, fake::Type::String};
}

void scenario(const std::string& name, bool motion, const std::function<void()>& body) {
    ++scenarios;
    fake::reset();
    mac::reset();
    fake_product_crypto::reset();
    motionAudit = motion;
    // Seed tempting keys both in allowed namespaces and unrelated credential stores.
    for (const char* space : {"wifi", "mqtt", "credentials", "productpair", "brainstate",
                              "productstate", "productctx", "formulaevt"}) {
        for (const char* key : {"password", "token", "secret", "ssid", "private_key"})
            io.disk[space][key] = stringValue(secret);
    }
    io.before = readOnlyCall;
    try { body(); audit(); }
    catch (const std::exception& e) {
        ++failures;
        std::fprintf(stderr, "FAIL %s (%s): %s\n", name.c_str(), motion ? "motion" : "brain", e.what());
        // Do not conceal leaked NVS handles when continuing other scenarios.
        if (!io.handles.empty()) throw;
    }
}

v4::Pairing pairing(bool motion, const std::string& id = device) {
    v4::Pairing p;
    p.role = role(motion);
    std::strcpy(p.deviceId, id.c_str());
    std::strcpy(p.epoch, challenge);
    std::strcpy(p.localPhysicalId, physical);
    std::strcpy(p.peerPhysicalId, "fedcba987654");
    return p;
}

ProductContext context(int kind = 1, const std::string& id = device) {
    ProductContext c;
    std::strcpy(c.deviceId, id.c_str());
    c.profileVersion = INT32_MAX;
    c.cleared = kind == 2;
    if (!c.cleared) {
        std::strcpy(c.babyId, (kind == 3 ? std::string(96, 'i') : "baby-1").c_str());
        std::strcpy(c.babyName, (kind == 3 ? std::string(320, 'n') : "Full \"name\" \\ \xe5\xae\x9d").c_str());
        std::strcpy(c.formulaBrand, (kind == 3 ? std::string(480, 'f') : "Full formula brand").c_str());
        c.waterMl = 500;
        c.temperatureC = 60;
        c.powderGPer100Ml = 50;
    }
    CHECK(validProductContext(c));
    return c;
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
    CHECK(!doc.overflowed());
    std::string out;
    serializeJson(doc, out);
    return out;
}

template<typename T, size_t N>
Bytes encoded(const T& value, size_t (*encoder)(const T&, uint8_t*, size_t)) {
    std::array<uint8_t, N> bytes{};
    const auto n = encoder(value, bytes.data(), bytes.size());
    CHECK(n > 0 && n <= N);
    return Bytes(bytes.begin(), bytes.begin() + n);
}
Bytes pairBytes(const v4::Pairing& p) { return encoded<v4::Pairing, kPairingRecordMaxSize>(p, encodePairingRecord); }
Bytes contextBytes(const ProductContext& c) { return encoded<ProductContext, kContextIdentityMaxSize>(c, encodeContextIdentity); }

ProductRequest request(const v4::Pairing& p, const ProductContext& c) {
    ProductRequest r;
    r.command = c.cleared ? ProductCommand::Clean : ProductCommand::Prepare;
    r.sequence = v4::kMaxSequence;
    std::strcpy(r.deviceId, p.deviceId);
    auto brain = p;
    brain.role = v4::Role::Brain;
    if (p.role == v4::Role::Motion) std::swap(brain.localPhysicalId, brain.peerPhysicalId);
    CHECK(makeLocalCommandId(brain, r.sequence, r.commandId));
    if (!c.cleared) {
        std::strcpy(r.babyId, c.babyId);
        r.profileVersion = c.profileVersion;
        r.waterMl = c.waterMl;
        r.temperatureC = c.temperatureC;
        r.powderGPer100Ml = c.powderGPer100Ml;
    }
    CHECK(validProductRequest(r));
    return r;
}

BrainState brainState(const v4::Pairing& p, int kind, bool used) {
    BrainState s;
    s.pairing = p;
    s.hasContext = kind != 0;
    if (kind) s.context = context(kind, p.deviceId);
    if (used) {
        s.localSequence = v4::kMaxSequence;
        s.pending = true;
        s.pendingRequest = request(p, kind ? s.context : context(2, p.deviceId));
        CHECK(requestDigest(s.pendingRequest, s.pendingDigest));
    }
    CHECK(validBrainState(s));
    return s;
}

MotionState motionState(const v4::Pairing& p, int kind, bool used) {
    MotionState s;
    s.pairing = p;
    const auto c = context(kind ? kind : 2, p.deviceId);
    if (kind) CHECK(makeMotionContextBarrier(c, s.context));
    if (used) {
        s.localSequence = s.cloudSequence = v4::kMaxSequence;
        s.cloudResult.kind = MotionResultKind::CloudStop;
        s.cloudResult.stopSequence = s.cloudSequence;
        std::strcpy(s.cloudResult.stopCommandId, std::string(128, 's').c_str());
        std::strcpy(s.cloudResult.stopExecutionId, challenge);
        std::strcpy(s.cloudResult.reason, "busy");
        auto& result = s.localResult;
        result.kind = MotionResultKind::Ordinary;
        result.request = request(p, c);
        CHECK(requestDigest(result.request, result.digest));
        result.accepted = true;
        std::strcpy(result.reason, "accepted");
        s.slot.kind = c.cleared ? MotionSlotKind::Intent : MotionSlotKind::Terminal;
        s.slot.request = result.request;
        std::memcpy(s.slot.digest, result.digest, sizeof(result.digest));
        std::strcpy(s.slot.executionId, challenge);
        if (!c.cleared) {
            CHECK(makeProductEventId(p, result.request.source, result.request.sequence, s.slot.eventId));
            s.slot.targetPowderG = productTargetPowderG(result.request);
            s.slot.uptimeMs = UINT32_MAX;
            std::strcpy(s.slot.reason, "interrupted");
            std::strcpy(s.slot.errorCode, "E_POWER");
        }
    }
    CHECK(validMotionState(s));
    return s;
}

Bytes stateBytes(bool motion, const v4::Pairing& p, int kind = 1, bool used = true) {
    return motion ? encoded<MotionState, kMotionStateMaxSize>(motionState(p, kind, used), encodeMotionState)
                  : encoded<BrainState, kBrainStateMaxSize>(brainState(p, kind, used), encodeBrainState);
}

void seed(bool motion, int kind = 1, bool used = true, const std::string& id = device) {
    const auto p = pairing(motion, id);
    io.disk["productpair"]["record"] = {pairBytes(p), fake::Type::Blob};
    io.disk[stateSpace(motion)]["record"] = {stateBytes(motion, p, kind, used), fake::Type::Blob};
    if (motion && kind) io.disk["productctx"]["payload"] = stringValue(legacyJson(context(kind, id)));
}

struct Capture {
    std::map<std::string, std::string> fields;
    Bytes pair, state, legacy;
    std::string at(const char* key) const { return fields.at(key); }
};

// Decode the transport independently, then verify the embedded production codecs.
Bytes unhex(const std::string& value) {
    CHECK(value.size() % 2 == 0);
    const auto nibble = [](char c) -> unsigned {
        CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        return c <= '9' ? unsigned(c - '0') : unsigned(c - 'a' + 10);
    };
    Bytes out;
    for (size_t i = 0; i < value.size(); i += 2) out.push_back(uint8_t(16 * nibble(value[i]) + nibble(value[i + 1])));
    return out;
}

Capture decode(const std::string& wire, bool motion, uint32_t now = captured, const std::string& id = device) {
    CHECK(wire.compare(0, prefix.size(), prefix) == 0);
    CHECK(wire.size() > prefix.size() + 3 && wire.substr(wire.size() - 3) == "\"}\n");
    CHECK(std::count(wire.begin(), wire.end(), '\n') == 1);
    CHECK(wire.find(secret) == std::string::npos);
    const std::string json = wire.substr(prefix.size());
    std::istringstream stream(json);
    DynamicJsonDocument doc(32768);
    CHECK(!deserializeJson(doc, stream));
    CHECK(stream.get() == '\n' && stream.peek() == std::char_traits<char>::eof());
    CHECK(!doc.overflowed() && doc.is<JsonObject>() && doc.size() == 14);
    CHECK(doc["schema"].is<unsigned>() && doc["schema"].as<unsigned>() == 1);
    CHECK(doc["captured_ms"].is<uint32_t>() && doc["captured_ms"].as<uint32_t>() == now);
    Capture result;
    for (const char* key : {"role", "device_id", "physical_id", "boot", "challenge", "pair_status",
                            "state_status", "legacy_status", "legacy_event", "pair_hex", "state_hex", "legacy_hex"}) {
        CHECK(doc[key].is<const char*>());
        result.fields[key] = doc[key].as<const char*>();
    }
    CHECK(result.at("role") == (motion ? "motion" : "brain"));
    CHECK(result.at("device_id") == id && result.at("physical_id") == physical);
    CHECK(result.at("boot") == "fedcba9876543210" && result.at("challenge") == challenge);
    result.pair = unhex(result.at("pair_hex"));
    result.state = unhex(result.at("state_hex"));
    result.legacy = unhex(result.at("legacy_hex"));
    for (const auto& blob : {result.pair, result.state, result.legacy})
        CHECK(std::search(blob.begin(), blob.end(), secret, secret + std::strlen(secret)) == blob.end());
    CHECK(result.pair.empty() == (result.at("pair_status") != "ready"));
    CHECK(result.state.empty() == (result.at("state_status") != "ready"));
    CHECK(result.legacy.empty() == (result.at("legacy_status") != "ready"));
    if (!result.pair.empty()) { v4::Pairing p; CHECK(decodePairingRecord(result.pair.data(), result.pair.size(), p)); }
    if (!result.state.empty()) {
        if (motion) { MotionState s; CHECK(decodeMotionState(result.state.data(), result.state.size(), s)); }
        else { BrainState s; CHECK(decodeBrainState(result.state.data(), result.state.size(), s)); }
    }
    if (!result.legacy.empty()) { ProductContext c; CHECK(decodeContextIdentity(result.legacy.data(), result.legacy.size(), c)); }
    return result;
}

std::string drain(MaintenanceExport& out, size_t width = 64) {
    std::string text;
    std::array<uint8_t, 257> bytes{}, repeated{};
    CHECK(width && width < bytes.size());
    for (unsigned chunks = 0; out.active(); ++chunks) {
        CHECK(chunks < 20000);
        bytes.fill(0xa5);
        const auto n = out.peek(bytes.data(), width);
        CHECK(n > 0 && n <= width && bytes[n] == 0xa5);
        CHECK(out.peek(repeated.data(), width) == n && std::equal(bytes.begin(), bytes.begin() + n, repeated.begin()));
        out.consume(0);
        text.append(reinterpret_cast<const char*>(bytes.data()), n);
        out.consume(n);
    }
    CHECK(out.peek(bytes.data(), bytes.size()) == 0);
    return text;
}

Capture capture(bool motion, size_t width = 64, const std::string& id = device) {
    const auto before = io.disk;
    MaintenanceExport out;
    CHECK(out.begin(role(motion), id.c_str(), challenge, boot, captured));
    const auto result = decode(drain(out, width), motion, captured, id);
    CHECK(io.disk == before);
    audit();
    return result;
}

void statuses(const Capture& c, const char* pair, const char* state, const char* legacy, const char* event) {
    CHECK(c.at("pair_status") == pair);
    CHECK(c.at("state_status") == state);
    CHECK(c.at("legacy_status") == legacy);
    CHECK(c.at("legacy_event") == event);
}

std::string exportLine(const std::string& id = device, const std::string& token = challenge) {
    return "MAINT EXPORT " + id + " " + token;
}

void parsing() {
    scenario("strict command grammar and unchanged outputs on rejection", false, [] {
        const std::vector<std::string> invalid = {
            "MAINT EXPORT", "MAINT EXPORTX", "MAINT EXPORT ", "MAINT EXPORT  " + std::string(challenge),
            exportLine("_bad"), exportLine("-bad"), exportLine("a.b"), exportLine("a/b"), exportLine("a\"b"),
            exportLine(std::string(65, 'a')), exportLine("a", ""), exportLine("a", std::string(31, '1')),
            exportLine("a", std::string(33, '1')), exportLine("a", std::string(32, '0')),
            exportLine("a", std::string(32, 'A')), exportLine("a", std::string(32, 'g')),
            exportLine() + " ", exportLine() + " trailing", exportLine() + "\n",
            "MAINT EXPORT\ta " + std::string(challenge), "MAINT EXPORT a  " + std::string(challenge)
        };
        for (const auto& text : invalid) {
            char id[65] = "unchanged", token[33] = "untouched";
            CHECK(parseMaintenanceExport(text.c_str(), id, token) == ExportCommand::Invalid);
            CHECK(std::string(id) == "unchanged" && std::string(token) == "untouched");
        }
        for (const char* text : {static_cast<const char*>(nullptr), "", "MAINT", "MAINT STATUS", "CODE 123456", "maint export a", " MAINT EXPORT"}) {
            char id[65] = "unchanged", token[33] = "untouched";
            CHECK(parseMaintenanceExport(text, id, token) == ExportCommand::NotExport);
            CHECK(std::string(id) == "unchanged" && std::string(token) == "untouched");
        }
        for (const auto& id : {std::string("a"), std::string("0_A-z"), std::string(64, 'Z')}) {
            for (const auto& token : {std::string(challenge), std::string(31, '0') + "1", "1" + std::string(31, '0')}) {
                char gotId[65]{}, gotToken[33]{};
                CHECK(parseMaintenanceExport(exportLine(id, token).c_str(), gotId, gotToken) == ExportCommand::Valid);
                CHECK(id == gotId && token == gotToken);
            }
        }
        CHECK(io.calls.empty() && mac::macCalls == 0);
    });
    scenario("bounded reader accepts maximum command and rejects overflow/control/timeout", false, [] {
        static_assert(MaintenanceLineReader::kCapacity == 768, "export reader contract");
        static_assert(MaintenanceLineReader::kPollBytes == 64, "USB budget contract");
        MaintenanceLineReader reader;
        const auto text = exportLine(std::string(64, 'a'));
        for (const char c : text) CHECK(reader.feed(uint8_t(c), 10) == MaintenanceLineReader::Result::None);
        CHECK(reader.feed('\r', 10) == MaintenanceLineReader::Result::Line);
        CHECK(text == reader.line());
        CHECK(reader.feed('\n', 10) == MaintenanceLineReader::Result::None);
        for (size_t size : {MaintenanceLineReader::kCapacity - 1, MaintenanceLineReader::kCapacity, size_t(1024)}) {
            for (size_t i = 0; i < size; ++i) reader.feed('x', 20);
            CHECK(reader.feed('\n', 20) == (size < MaintenanceLineReader::kCapacity ? MaintenanceLineReader::Result::Line : MaintenanceLineReader::Result::Rejected));
        }
        for (uint8_t bad : {uint8_t(0), uint8_t(9), uint8_t(0x7f), uint8_t(0xff)}) {
            reader.feed('M', 30); reader.feed(bad, 30);
            CHECK(reader.feed('\n', 30) == MaintenanceLineReader::Result::Rejected);
        }
        reader.feed('M', UINT32_MAX - 10);
        CHECK(reader.feed('\n', uint32_t(UINT32_MAX - 10 + MaintenanceLineReader::kTimeoutMs)) == MaintenanceLineReader::Result::Rejected);
    });
}

void records() {
    for (bool motion : {false, true}) {
        for (bool missingNamespace : {false, true}) scenario("missing namespace/key remains missing", motion, [=] {
            if (missingNamespace) for (const char* s : {"productpair", stateSpace(motion), "productctx", "formulaevt"}) io.disk.erase(s);
            statuses(capture(motion), "missing", "missing", motion ? "missing" : "not_applicable", motion ? "missing" : "not_applicable");
        });
        for (int kind : {0, 1, 2, 3}) for (bool used : {false, true}) scenario("exact records, full context, tombstone and watermarks", motion, [=] {
            const std::string id = kind == 3 ? std::string(64, 'D') : device;
            seed(motion, kind, used, id);
            const auto c = capture(motion, kind == 3 ? 1 : 63, id);
            statuses(c, "ready", "ready", motion ? (kind ? "ready" : "missing") : "not_applicable", motion ? "missing" : "not_applicable");
            CHECK(c.pair == io.disk.at("productpair").at("record").bytes);
            CHECK(c.state == io.disk.at(stateSpace(motion)).at("record").bytes);
            const auto p = pairing(motion, id);
            if (motion) {
                MotionState actual;
                CHECK(decodeMotionState(c.state.data(), c.state.size(), actual));
                CHECK(sameMotionState(actual, motionState(p, kind, used)));
                if (kind) CHECK(c.legacy == contextBytes(context(kind, id)));
            } else {
                BrainState actual;
                CHECK(decodeBrainState(c.state.data(), c.state.size(), actual));
                CHECK(sameBrainState(actual, brainState(p, kind, used)));
            }
            if (kind == 3) CHECK(contextBytes(context(kind, id)).size() == kContextIdentityMaxSize);
        });
        for (bool orphanState : {false, true}) scenario("partial install capture reports facts without repairing", motion, [=] {
            seed(motion);
            io.disk[orphanState ? "productpair" : stateSpace(motion)].erase("record");
            const auto c = capture(motion);
            statuses(c, orphanState ? "missing" : "ready", orphanState ? "ready" : "missing",
                     motion ? "ready" : "not_applicable", motion ? "missing" : "not_applicable");
            if (orphanState) CHECK(c.state == io.disk.at(stateSpace(motion)).at("record").bytes);
        });
        for (bool state : {false, true}) for (unsigned mutation = 0; mutation < 5; ++mutation) scenario("identity/epoch/peer discrepancies", motion, [=] {
            seed(motion);
            auto p = pairing(motion);
            if (mutation == 0) std::strcpy(p.deviceId, "other-device");
            if (mutation == 1) p.localPhysicalId[0] = '9';
            if (mutation == 2) p.epoch[0] = '9';
            if (mutation == 3) p.peerPhysicalId[0] = '9';
            if (mutation == 4) p.role = role(!motion);
            const auto bytes = state ? stateBytes(mutation == 4 ? !motion : motion, p) : pairBytes(p);
            io.disk[state ? stateSpace(motion) : "productpair"]["record"] = {bytes, fake::Type::Blob};
            const auto c = capture(motion);
            if (mutation == 2 || mutation == 3) statuses(c, "ready", "conflict", motion ? "ready" : "not_applicable", motion ? "missing" : "not_applicable");
            else statuses(c, state ? "ready" : "identity_mismatch", state ? (mutation == 4 ? "corrupt" : "identity_mismatch") : "ready",
                          motion ? "ready" : "not_applicable", motion ? "missing" : "not_applicable");
        });
        for (const char* space : {"productpair", stateSpace(motion)}) for (unsigned mutation = 0; mutation < 7; ++mutation) scenario("corrupt vs typed I/O record", motion, [=] {
            seed(motion);
            auto& value = io.disk[space]["record"];
            if (mutation == 0) value.bytes.clear();
            if (mutation == 1) value.bytes = Bytes(4097, 0);
            if (mutation == 2) value.bytes[0] ^= 1;
            if (mutation == 3) value.bytes.back() ^= 1;
            if (mutation == 4) value.bytes.pop_back();
            if (mutation == 5) value.type = fake::Type::String;
            if (mutation == 6) value.type = fake::Type::U32;
            const auto c = capture(motion);
            const auto status = mutation >= 5 ? "io_error" : "corrupt";
            statuses(c, std::string(space) == "productpair" ? status : "ready", std::string(space) == "productpair" ? "ready" : status,
                     motion ? "ready" : "not_applicable", motion ? "missing" : "not_applicable");
        });
    }
}

void legacy() {
    for (int kind : {1, 2, 3}) scenario("unpaired old active/tombstone/max context is diagnostic only", true, [=] {
        io.disk["productctx"]["payload"] = stringValue(legacyJson(context(kind)));
        const auto c = capture(true);
        statuses(c, "missing", "missing", "ready", "missing");
        CHECK(c.legacy == contextBytes(context(kind)));
    });
    const std::vector<fake::Value> badContexts = {
        stringValue("{bad"), stringValue(""), stringValue(legacyJson(context(1, "wrong-device"))),
        {{'{', '}'}, fake::Type::String}, {{'{', '}', 0, 'x', 0}, fake::Type::String},
        {Bytes(2049, 'x'), fake::Type::String}, {{}, fake::Type::String},
        {{1, 2}, fake::Type::Blob}, {{1, 0, 0, 0}, fake::Type::U32}
    };
    for (const auto& value : badContexts) scenario("invalid old context cannot appear missing/ready", true, [&] {
        io.disk["productctx"]["payload"] = value;
        statuses(capture(true), "missing", "missing", value.type == fake::Type::String ? "corrupt" : "io_error", "missing");
    });
    const std::vector<fake::Value> events = {
        stringValue("{}"), stringValue("{bad-json"), stringValue(secret), {{'x', 'y'}, fake::Type::String},
        {Bytes(2048, 'x'), fake::Type::String}, stringValue(""), {{}, fake::Type::String},
        {Bytes(2049, 'x'), fake::Type::String}, {{1, 2}, fake::Type::Blob}, {{1, 0, 0, 0}, fake::Type::U32}
    };
    for (size_t i = 0; i < events.size(); ++i) scenario("old journal present is not decoded or exported", true, [&, i] {
        io.disk["formulaevt"]["payload"] = events[i];
        statuses(capture(true), "missing", "missing", "missing", i < 5 ? "present" : i < 8 ? "corrupt" : "io_error");
        CHECK(mac::stringQueries == 2 && mac::stringReads == 0);
    });
    scenario("brain ignores even malformed legacy and credential namespaces", false, [] {
        io.disk["formulaevt"]["payload"] = stringValue(secret);
        io.disk["productctx"]["payload"] = stringValue("bad");
        capture(false);
        CHECK(mac::stringQueries == 0 && mac::stringReads == 0);
    });
}

void faults() {
    for (bool motion : {false, true}) {
        fake::Database disk;
        std::vector<fake::Call> trace;
        scenario("fault replay baseline", motion, [&] {
            seed(motion, 3);
            if (motion) io.disk["formulaevt"]["payload"] = stringValue("bad but present");
            disk = io.disk;
            capture(motion);
            trace = io.calls;
        });
        for (const auto& call : trace) {
            if (call.op == Op::Close) continue;
            const std::string field = call.name == "productpair" ? "pair_status" : call.name == stateSpace(motion) ? "state_status" :
                                      call.name == "productctx" ? "legacy_status" : "legacy_event";
            for (const auto error : {ESP_FAIL, ESP_ERR_NVS_TYPE_MISMATCH, ESP_ERR_NVS_NOT_FOUND}) {
                scenario("I/O replay " + call.name + " op=" + std::to_string(int(call.op)) + " error=" + std::to_string(error), motion, [&] {
                    io.disk = disk;
                    fake::fail(call.op, call.occurrence, error);
                    const auto c = capture(motion);
                    CHECK(c.fields.at(field) == (error == ESP_ERR_NVS_NOT_FOUND && call.op != Op::Read ? "missing" : "io_error"));
                    for (const char* other : {"pair_status", "state_status", "legacy_status", "legacy_event"}) {
                        if (field == other) continue;
                        const std::string expected = std::string(other) == "legacy_event" ? (motion ? "present" : "not_applicable") :
                            std::string(other) == "legacy_status" && !motion ? "not_applicable" : "ready";
                        CHECK(c.at(other) == expected);
                    }
                });
            }
            if (call.op == Op::Read) for (int delta : {-1, 1}) scenario("short/growing read " + call.name, motion, [&] {
                io.disk = disk;
                const size_t n = disk.at(call.name).at(call.key).bytes.size();
                fake::fail(Op::Read, call.occurrence, ESP_OK, false, size_t(int(n) + delta));
                CHECK(capture(motion).fields.at(field) == "io_error");
            });
            if (call.op == Op::Query) for (size_t length : {size_t(0), size_t(4097)}) scenario("invalid queried bound " + call.name, motion, [&] {
                io.disk = disk;
                fake::fail(Op::Query, call.occurrence, ESP_OK, false, length);
                CHECK(capture(motion).fields.at(field) == "corrupt");
            });
        }
        for (unsigned failedCall : {1u, 2u}) scenario("MAC failure during capture/pair validation", motion, [=] {
            seed(motion);
            const auto before = io.disk;
            mac::failMacCall = failedCall;
            MaintenanceExport out;
            CHECK(out.begin(role(motion), device, challenge, boot, captured) == (failedCall == 2));
            if (failedCall == 2) CHECK(decode(drain(out), motion).at("pair_status") == "io_error");
            else CHECK(!out.active() && io.calls.empty());
            CHECK(mac::macCalls >= failedCall && io.disk == before);
        });
    }
}

void lifecycle() {
    scenario("begin validates inputs before any I/O", false, [] {
        MaintenanceExport out;
        for (unsigned invalid = 0; invalid < 8; ++invalid) {
            CHECK(!out.begin(invalid == 0 ? static_cast<v4::Role>(255) : v4::Role::Brain,
                invalid == 1 ? nullptr : invalid == 2 ? "_bad" : invalid == 3 ? "" : device,
                invalid == 4 ? nullptr : invalid == 5 ? "00000000000000000000000000000000" : invalid == 6 ? "bad" : challenge,
                invalid == 7 ? 0 : boot, captured));
            CHECK(!out.active() && io.calls.empty() && mac::macCalls == 0);
        }
        mac::mac.fill(0);
        CHECK(!out.begin(v4::Role::Brain, device, challenge, boot, captured));
        CHECK(io.calls.empty());
    });
    for (bool motion : {false, true}) scenario("snapshot, cursor, cancellation, expiry wrap and reuse", motion, [=] {
        seed(motion, 3);
        MaintenanceExport out;
        CHECK(out.begin(role(motion), device, challenge, boot, captured));
        const auto calls = io.calls.size();
        CHECK(!out.begin(role(motion), "other-device", challenge, boot, 456));
        CHECK(io.calls.size() == calls);
        uint8_t byte = 0xa5;
        CHECK(out.peek(nullptr, 64) == 0 && out.peek(&byte, 0) == 0 && byte == 0xa5);
        const auto saved = io.disk.at(stateSpace(motion)).at("record").bytes;
        // Streaming is the already captured snapshot, with no lazy reads.
        io.disk[stateSpace(motion)]["record"].bytes = {0};
        CHECK(decode(drain(out, 256), motion).state == saved && io.calls.size() == calls);
        CHECK(out.begin(role(motion), device, challenge, boot, UINT32_MAX - 100));
        CHECK(!out.expired(uint32_t(UINT32_MAX - 100 + MaintenanceExport::kLifetimeMs - 1)));
        CHECK(out.expired(uint32_t(UINT32_MAX - 100 + MaintenanceExport::kLifetimeMs)));
        out.cancel();
        out.cancel();
        CHECK(!out.active() && out.peek(&byte, 1) == 0);
        CHECK(out.begin(role(motion), device, challenge, boot, captured));
        out.consume(std::numeric_limits<size_t>::max());
        CHECK(!out.active());
        io.disk[stateSpace(motion)].erase("record");
        CHECK(out.begin(role(motion), device, challenge, boot, captured));
        CHECK(decode(drain(out), motion).state.empty());
    });
}

struct FakePort {
    std::string input, output;
    size_t position = 0, reads = 0, writeCalls = 0, offered = 0;
    int writable = 64;
    size_t writeLimit = std::numeric_limits<size_t>::max();
    bool overreport = false, failRead = false;
    int available() const { return int(input.size() - position); }
    int read() {
        ++reads;
        if (failRead || position == input.size()) return -1;
        return static_cast<unsigned char>(input[position++]);
    }
    int availableForWrite() const { return writable; }
    size_t write(const uint8_t* bytes, size_t length) {
        ++writeCalls;
        offered += length;
        CHECK(writable > 0 && length <= size_t(writable) && length <= 64);
        const size_t n = std::min(length, writeLimit);
        output.append(reinterpret_cast<const char*>(bytes), n);
        return overreport ? length + 1 : n;
    }
    // Compile-time protection: every response, including OTA, must use write().
    void print(const char*) = delete;
    void println(const char*) = delete;
    void enqueue(const std::string& line) { input += line + '\n'; }
};

std::vector<std::string> extraLines;
unsigned codes = 0;
const std::string otaReply = "[ota] code accepted " + std::string(60, 'x') + "\n";
bool extraCommand(const char* line, char* output, size_t capacity) {
    extraLines.emplace_back(line);
    if (std::string(line) == "CODE 123456") {
        ++codes;
        CHECK(otaReply.size() + 1 <= capacity);
        std::memcpy(output, otaReply.c_str(), otaReply.size() + 1);
        return true;
    }
    if (std::string(line) == "CODE BAD_REPLY") {
        std::memset(output, 'x', capacity);
        return true;
    }
    return false;
}

std::string reply(const char* status) { return std::string("\n[maint] ") + status + "\n"; }

// This tests the wire contract, not the separately owned host capture tool.
// Only complete frames are decoded; ordinary between-frame log lines are ignored.
std::string unwrapFrames(const std::string& wire, bool allowLogs = false) {
    std::string reconstructed;
    std::istringstream stream(wire);
    std::string line;
    while (std::getline(stream, line)) {
        CHECK(!stream.eof());
        if (line.empty()) continue;
        if (line.compare(0, 5, "[mx] ") != 0) { CHECK(allowLogs); continue; }
        CHECK(line.size() >= 17 && line.size() <= 47 && line[9] == ':' && line[14] == ':');
        const auto offsetBytes = unhex(line.substr(5, 4));
        const auto crcBytes = unhex(line.substr(10, 4));
        const size_t offset = size_t(offsetBytes[0]) * 256 + offsetBytes[1];
        const uint16_t expectedCrc = uint16_t(unsigned(crcBytes[0]) * 256 + crcBytes[1]);
        const auto payload = unhex(line.substr(15));
        CHECK(!payload.empty() && payload.size() <= 16 && offset == reconstructed.size());
        Bytes checked{uint8_t(offset), uint8_t(offset >> 8), uint8_t(payload.size())};
        checked.insert(checked.end(), payload.begin(), payload.end());
        CHECK(babytech::crc16(checked.data(), checked.size()) == expectedCrc);
        reconstructed.append(payload.begin(), payload.end());
    }
    return reconstructed;
}

void poll(MaintenanceUsbConsole& usb, FakePort& port, uint32_t now = captured, bool safe = true, bool callback = false) {
    const auto disk = io.disk;
    const auto reads = port.reads, writes = port.writeCalls, offered = port.offered;
    const auto outputSize = port.output.size();
    const bool exporting = usb.exporting();
    usb.poll(port, now, safe, callback ? extraCommand : nullptr);
    CHECK(port.reads - reads <= 64 && port.writeCalls - writes <= 1 && port.offered - offered <= 64);
    CHECK(port.output.size() - outputSize <= 64);
    if (port.reads != reads) CHECK(port.output.size() == outputSize);
    if (port.writeCalls != writes) CHECK(port.reads == reads);
    if (exporting) CHECK(port.reads == reads);
    CHECK(io.disk == disk);
    audit();
}

void input(MaintenanceUsbConsole& usb, FakePort& port, bool safe = true, bool callback = false, uint32_t now = captured) {
    for (unsigned n = 0; !usb.exporting(); ++n) {
        CHECK(n < 20000);
        const auto reads = port.reads, writes = port.writeCalls;
        poll(usb, port, now, safe, callback);
        if (port.reads == reads && port.writeCalls == writes) break;
    }
}

void start(MaintenanceUsbConsole& usb, FakePort& port, bool motion, uint32_t now = captured, const std::string& id = device) {
    usb.begin(role(motion), boot);
    port.enqueue("MAINT BEGIN");
    input(usb, port, true, false, now);
    CHECK(usb.active() && port.output == reply("active"));
    port.output.clear();
    port.enqueue(exportLine(id));
    input(usb, port, true, false, now);
    CHECK(usb.exporting() && port.output.empty());
}

void finish(MaintenanceUsbConsole& usb, FakePort& port, uint32_t now = captured) {
    for (unsigned n = 0; usb.exporting(); ++n) {
        CHECK(n < 40000);
        poll(usb, port, now);
    }
}

void runtime() {
    for (bool motion : {false, true}) {
        scenario("one command per poll and replies never bypass their queue", motion, [=] {
            MaintenanceUsbConsole usb;
            FakePort port;
            usb.begin(role(motion), boot);
            port.enqueue("MAINT BEGIN"); port.enqueue("MAINT END");
            poll(usb, port);
            CHECK(port.position == std::strlen("MAINT BEGIN\n") && port.output.empty() && usb.active());
            const auto at = port.position;
            port.writeLimit = 2;
            poll(usb, port);
            CHECK(port.output == reply("active").substr(0, 2) && port.position == at);
            port.writeLimit = 0; poll(usb, port);
            CHECK(port.output.size() == 2 && port.position == at && usb.active());
            port.writeLimit = 64;
            poll(usb, port);
            CHECK(port.output == reply("active") && port.position == at && usb.active());
            poll(usb, port);
            CHECK(!usb.active() && port.available() == 0 && port.output == reply("active"));
            poll(usb, port);
            CHECK(port.output == reply("active") + reply("inactive"));
            CHECK(io.calls.empty());
        });
        scenario("queued OTA response obeys 64-byte total output budget", motion, [=] {
            extraLines.clear(); codes = 0;
            MaintenanceUsbConsole usb;
            FakePort port;
            usb.begin(role(motion), boot);
            port.enqueue("CODE 123456"); port.enqueue("MAINT BEGIN");
            poll(usb, port, captured, true, true);
            CHECK(codes == 1 && port.output.empty() && !usb.active());
            const auto at = port.position;
            const std::string expected = "\n" + otaReply;
            port.writable = 1000;
            poll(usb, port, captured, true, true);
            CHECK(port.output == expected.substr(0, 64) && port.position == at && !usb.active());
            poll(usb, port, captured, true, true);
            CHECK(port.output == expected && port.position == at && !usb.active());
            input(usb, port, true, true);
            CHECK(usb.active() && port.output == expected + reply("active"));
            port.output.clear(); port.enqueue("CODE BAD_REPLY"); input(usb, port, true, true);
            CHECK(port.output == reply("invalid_response") && io.calls.empty());
        });
        for (bool partial : {false, true}) scenario("five-second reply expiry discards unsent bytes without unlocking", motion, [=] {
            MaintenanceUsbConsole usb;
            FakePort port;
            const uint32_t now = UINT32_MAX - 100;
            usb.begin(role(motion), boot);
            port.enqueue("MAINT BEGIN");
            poll(usb, port, now);
            CHECK(usb.active() && port.output.empty());
            if (partial) { port.writeLimit = 3; poll(usb, port, now); }
            const auto saved = port.output;
            const auto reads = port.reads;
            port.writable = 0;
            poll(usb, port, uint32_t(now + MaintenanceExport::kLifetimeMs - 1));
            CHECK(usb.active() && port.output == saved && port.reads == reads);
            port.writable = 64; port.writeLimit = 64;
            poll(usb, port, uint32_t(now + MaintenanceExport::kLifetimeMs));
            CHECK(usb.active() && port.output == saved && port.reads == reads);
            poll(usb, port, uint32_t(now + MaintenanceExport::kLifetimeMs + 1));
            CHECK(usb.active() && port.output == saved);
            port.enqueue("MAINT STATUS");
            input(usb, port, true, false, uint32_t(now + MaintenanceExport::kLifetimeMs + 2));
            CHECK(port.output == saved + reply("active") && io.calls.empty());
        });
        scenario("last payload remains active until whole frame written; logs between frames", motion, [=] {
            seed(motion, 3);
            MaintenanceExport exporter;
            CHECK(exporter.begin(role(motion), device, challenge, boot, captured));
            const auto expected = drain(exporter);
            MaintenanceUsbConsole usb;
            FakePort port;
            start(usb, port, motion);
            port.enqueue("MAINT END");
            const auto reads = port.reads;
            const size_t frames = (expected.size() + 15) / 16;
            for (size_t n = 0; n + 1 < frames; ++n) {
                poll(usb, port);
                CHECK(usb.exporting() && port.output.back() == '\n');
                port.output += "[runtime] ordinary log\n";
            }
            const size_t finalPayload = expected.size() - (frames - 1) * 16;
            port.writeLimit = 16 + 2 * finalPayload;  // Everything but the frame's final newline.
            poll(usb, port);
            CHECK(usb.exporting() && port.output.back() != '\n' && port.reads == reads);
            const auto partial = port.output;
            port.writeLimit = 0; poll(usb, port);
            CHECK(usb.exporting() && port.output == partial && port.reads == reads);
            port.writeLimit = 1; poll(usb, port);
            CHECK(!usb.exporting() && usb.active() && port.reads == reads);
            CHECK(unwrapFrames(port.output, true) == expected);
            decode(unwrapFrames(port.output, true), motion);
            port.output.clear(); port.writeLimit = 64; input(usb, port);
            CHECK(!usb.active() && port.output == reply("inactive"));
        });
        scenario("impossible write count cancels incomplete export without automatic unlock", motion, [=] {
            MaintenanceUsbConsole usb;
            FakePort port;
            start(usb, port, motion);
            poll(usb, port);
            const auto partial = port.output;
            const auto reads = port.reads;
            port.enqueue("MAINT END");
            port.writeLimit = 0; port.overreport = true;
            poll(usb, port);
            CHECK(!usb.exporting() && usb.active() && port.output == partial && port.reads == reads);
            CHECK(port.available() == int(std::strlen("MAINT END\n")));
            const auto reconstructed = unwrapFrames(partial);
            CHECK(reconstructed.compare(0, prefix.size(), prefix) == 0 && reconstructed.back() != '\n');
            DynamicJsonDocument doc(32768);
            CHECK(deserializeJson(doc, reconstructed.substr(prefix.size())));
            // An SDK cannot validly overreport. Defensive cancellation need not
            // emit an abort; only the explicit queued END releases maintenance.
            port.overreport = false; port.writeLimit = 64;
            poll(usb, port);
            CHECK(!usb.active() && port.available() == 0 && port.output == partial);
            CHECK(port.reads - reads == std::strlen("MAINT END\n"));
            poll(usb, port);
            CHECK(!usb.active() && port.output == partial + reply("inactive"));
        });
        scenario("active and locally safe required; status/import do not install", motion, [=] {
            MaintenanceUsbConsole usb;
            FakePort port;
            usb.begin(role(motion), boot);
            port.enqueue(exportLine());
            input(usb, port);
            CHECK(port.output == reply("unsafe") && !usb.exporting() && !usb.active());
            port.output.clear();
            port.enqueue("MAINT BEGIN");
            input(usb, port, false);
            CHECK(port.output == reply("unsafe") && !usb.active());
            port.output.clear();
            for (const char* line : {"MAINT STATUS", "MAINT BEGIN", "MAINT STATUS", "MAINT IMPORT", "MAINT INSTALL", "MAINT END", "MAINT STATUS"}) port.enqueue(line);
            input(usb, port);
            CHECK(port.output == reply("inactive") + reply("active") + reply("active") +
                  reply("unknown_command") + reply("unknown_command") + reply("inactive") + reply("inactive"));
            CHECK(!usb.active() && io.calls.empty() && mac::macCalls == 0);
            port.enqueue("MAINT BEGIN"); input(usb, port);
            port.output.clear(); port.enqueue(exportLine()); input(usb, port, false);
            CHECK(port.output == reply("unsafe") && !usb.exporting() && io.calls.empty());
        });
        scenario("input 64-byte budget and negative read", motion, [=] {
            MaintenanceUsbConsole usb;
            FakePort port;
            usb.begin(role(motion), boot);
            port.enqueue(std::string(127, 'x'));
            poll(usb, port);
            CHECK(port.reads == 64 && port.position == 64 && port.output.empty());
            poll(usb, port);
            CHECK(port.reads == 128 && port.output.empty());
            poll(usb, port);
            CHECK(port.reads == 128 && port.output == reply("unknown_command"));
            port.enqueue("MAINT BEGIN");
            port.failRead = true;
            const auto before = port.position;
            poll(usb, port);
            CHECK(port.position == before && !usb.active());
            port.failRead = false; input(usb, port); CHECK(usb.active());
            CHECK(io.calls.empty());
        });
        for (int capacity : {1, 7, 63, 64, 1000}) for (size_t limit : {size_t(1), size_t(11), size_t(1000)}) scenario("stream budget/backpressure capacity=" + std::to_string(capacity) + " limit=" + std::to_string(limit), motion, [=] {
            seed(motion, 3);
            MaintenanceUsbConsole usb;
            FakePort port;
            start(usb, port, motion);
            const auto calls = io.calls.size();
            const auto readAtStart = port.reads;
            port.enqueue("MAINT END"); port.enqueue("MAINT STATUS");
            port.writable = capacity; port.writeLimit = limit;
            poll(usb, port);
            const auto partial = port.output;
            const auto writes = port.writeCalls;
            for (int stalled : {-1, 0}) {
                port.writable = stalled;
                poll(usb, port);
                CHECK(port.writeCalls == writes && port.output == partial && usb.exporting());
            }
            port.writable = capacity; port.writeLimit = 0;
            poll(usb, port);
            CHECK(port.output == partial && usb.exporting());
            port.writeLimit = limit;
            finish(usb, port);
            CHECK(port.reads == readAtStart && usb.active() && io.calls.size() == calls);
            const auto c = decode(unwrapFrames(port.output), motion);
            CHECK(c.state == io.disk.at(stateSpace(motion)).at("record").bytes);
            CHECK(c.pair == io.disk.at("productpair").at("record").bytes);
            if (motion) CHECK(c.legacy == contextBytes(context(3)));
            port.output.clear(); input(usb, port);
            CHECK(!usb.active() && port.output == reply("inactive") + reply("inactive"));
        });
        for (unsigned cause = 0; cause < 2; ++cause) scenario("safety/expiry replaces queued frame with abort, never valid JSON", motion, [=] {
            seed(motion);
            MaintenanceUsbConsole usb;
            FakePort port;
            const uint32_t now = UINT32_MAX - 100;
            start(usb, port, motion, now);
            poll(usb, port, now);
            const auto partial = port.output;
            const auto reconstructed = unwrapFrames(partial);
            CHECK(reconstructed.compare(0, prefix.size(), prefix) == 0);
            port.enqueue("MAINT END");
            const auto readAtStart = port.reads;
            port.writable = 0;
            poll(usb, port, now);
            if (cause == 1) {
                poll(usb, port, uint32_t(now + MaintenanceExport::kLifetimeMs - 1));
                CHECK(usb.exporting() && port.output == partial);
            }
            const auto abortedAt = cause == 1 ? uint32_t(now + MaintenanceExport::kLifetimeMs) : now;
            poll(usb, port, abortedAt, cause != 0);
            CHECK(!usb.exporting() && usb.active() && port.reads == readAtStart);
            CHECK(port.output == partial);
            port.writable = 64;
            poll(usb, port, abortedAt);
            CHECK(port.output == partial + reply("export_aborted") && port.reads == readAtStart);
            CHECK(unwrapFrames(port.output, true) == reconstructed);
            DynamicJsonDocument doc(32768);
            CHECK(deserializeJson(doc, reconstructed.substr(prefix.size())));
            port.output.clear(); input(usb, port, true, false, abortedAt);
            CHECK(!usb.active() && port.output == reply("inactive"));
            port.output.clear(); start(usb, port, motion); finish(usb, port);
            decode(unwrapFrames(port.output), motion);
        });
        scenario("OTA CODE callback preserved and input cannot interleave with capture", motion, [=] {
            seed(motion);
            extraLines.clear(); codes = 0;
            MaintenanceUsbConsole usb;
            FakePort port;
            usb.begin(role(motion), boot);
            port.enqueue("CODE 123456"); port.enqueue("MAINT BEGIN"); port.enqueue(exportLine());
            port.enqueue("CODE 123456"); port.enqueue(exportLine()); port.enqueue("MAINT END");
            input(usb, port, true, true);
            CHECK(usb.exporting() && codes == 1 && extraLines.size() == 3);
            CHECK(port.output == "\n" + otaReply + reply("active")); port.output.clear();
            const auto at = port.position;
            for (unsigned chunks = 0; usb.exporting(); ++chunks) {
                CHECK(chunks < 40000);
                poll(usb, port, captured, true, true);
                CHECK(port.position == at && codes == 1);
            }
            decode(unwrapFrames(port.output), motion); port.output.clear();
            input(usb, port, true, true);
            CHECK(usb.exporting() && codes == 2 && extraLines.size() == 5);
            CHECK(port.output == "\n" + otaReply); port.output.clear();
            finish(usb, port); decode(unwrapFrames(port.output), motion); port.output.clear();
            input(usb, port, true, true);
            CHECK(!usb.active() && extraLines.back() == "MAINT END" && port.output == reply("inactive"));
        });
        scenario("malformed/overflow/control export never captures and reader recovers", motion, [=] {
            MaintenanceUsbConsole usb;
            FakePort port;
            usb.begin(role(motion), boot);
            port.enqueue("MAINT BEGIN"); input(usb, port); port.output.clear();
            for (const auto& text : {std::string("MAINT EXPORT"), exportLine("_bad"), exportLine("a", std::string(32, '0')),
                                    exportLine() + " extra", exportLine(std::string(65, 'a'))}) {
                port.enqueue(text); input(usb, port);
                CHECK(!usb.exporting() && port.output == reply("invalid_export")); port.output.clear();
            }
            for (const auto& text : {exportLine() + std::string(MaintenanceLineReader::kCapacity, 'x'), std::string("MAINT EXPORT ") + std::string(1, '\0') + "a " + challenge}) {
                port.enqueue(text); input(usb, port);
                CHECK(!usb.exporting() && port.output == reply("invalid_line")); port.output.clear();
            }
            CHECK(io.calls.empty() && mac::macCalls == 0);
            port.enqueue(exportLine(std::string(64, 'D'))); input(usb, port);
            CHECK(usb.exporting()); finish(usb, port);
            statuses(decode(unwrapFrames(port.output), motion, captured, std::string(64, 'D')), "missing", "missing",
                     motion ? "missing" : "not_applicable", motion ? "missing" : "not_applicable");
        });
        scenario("failed capture can retry and never changes maintenance permission", motion, [=] {
            MaintenanceUsbConsole usb;
            FakePort port;
            usb.begin(role(motion), 0);
            port.enqueue("MAINT BEGIN"); input(usb, port); port.output.clear();
            port.enqueue(exportLine()); input(usb, port);
            CHECK(port.output == reply("export_failed") && usb.active() && !usb.exporting());
            CHECK(io.calls.empty());
            usb.begin(role(motion), boot); port.output.clear();
            port.enqueue(exportLine()); input(usb, port); finish(usb, port);
            decode(unwrapFrames(port.output), motion); CHECK(usb.active());
            port.output.clear(); port.enqueue("MAINT IMPORT"); input(usb, port);
            CHECK(port.output == reply("unknown_command"));
        });
    }
}

int emitFixture(bool motion) {
    std::string frames;
    scenario("emit max-context interoperability fixture", motion, [&] {
        const std::string id(64, 'D');
        seed(motion, 3, true, id);
        MaintenanceUsbConsole usb;
        FakePort port;
        start(usb, port, motion, captured, id);
        port.writeLimit = 7;
        finish(usb, port);
        const auto c = decode(unwrapFrames(port.output), motion, captured, id);
        CHECK(c.pair == io.disk.at("productpair").at("record").bytes);
        CHECK(c.state == io.disk.at(stateSpace(motion)).at("record").bytes);
        if (motion) CHECK(c.legacy == contextBytes(context(3, id)) && c.legacy.size() == kContextIdentityMaxSize);
        CHECK(usb.active());
        frames = port.output;
    });
    if (failures) return 1;
    return std::fwrite(frames.data(), 1, frames.size(), stdout) == frames.size() ? 0 : 1;
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--emit-brain") return emitFixture(false);
    if (argc == 2 && std::string(argv[1]) == "--emit-motion") return emitFixture(true);
    const std::vector<std::pair<const char*, void (*)()>> groups = {
        {"parse", parsing}, {"records", records}, {"legacy", legacy}, {"faults", faults},
        {"lifecycle", lifecycle}, {"runtime", runtime}
    };
    bool selected = false;
    for (const auto& group : groups) {
        if (argc > 1 && group.first != std::string(argv[1])) continue;
        selected = true;
        const auto before = scenarios;
        group.second();
        std::printf("%s: %u scenarios\n", group.first, scenarios - before);
    }
    if (!selected) { std::fprintf(stderr, "Unknown test group\n"); return 2; }
    std::printf("MaintenanceExport + MaintenanceUsbConsole: %u scenarios, %u failures\n", scenarios, failures);
    return failures ? 1 : 0;
}
