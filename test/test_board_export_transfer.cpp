#include "BoardExportTransfer.h"
#include "MaintenanceExport.h"
#include "FakeCommissioning.h"
#include "FakeProductCrypto.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

using namespace babytech::boardlink;
namespace v4 = babytech::v4;
namespace fake = fake_brain;
using fake::Bytes;
using fake::Op;
using fake::io;

namespace allocation_fault {
enum class Kind { None, Array, Scalar };
Kind kind = Kind::None;
size_t size = 0;
unsigned hits = 0;
bool reject(Kind requested, size_t bytes) {
    if (kind != requested || size != bytes) return false;
    kind = Kind::None;
    ++hits;
    return true;
}
}

// Fail only the selected production nothrow allocation. Successful allocations
// retain the system throwing-new/delete backend, including sanitizer tracking.
void* operator new(size_t size, const std::nothrow_t&) noexcept {
    if (allocation_fault::reject(allocation_fault::Kind::Scalar, size)) return nullptr;
    try { return ::operator new(size); } catch (const std::bad_alloc&) { return nullptr; }
}
void* operator new[](size_t size, const std::nothrow_t&) noexcept {
    if (allocation_fault::reject(allocation_fault::Kind::Array, size)) return nullptr;
    try { return ::operator new[](size); } catch (const std::bad_alloc&) { return nullptr; }
}
void operator delete(void* bytes, const std::nothrow_t&) noexcept { ::operator delete(bytes); }
void operator delete[](void* bytes, const std::nothrow_t&) noexcept { ::operator delete[](bytes); }

namespace {
// Fixed public headers are the contract. No private-member hooks or replacement
// transfer/decoder implementations; production MaintenanceExport must be a source.
static_assert(std::is_base_of<BoardExportSource, MaintenanceExport>::value, "production exporter implements source");
static_assert(kMotionExportWireMaxSize == 16384, "raw capture budget plus one NUL");
static_assert(BoardExportTransfer::kLifetimeMs == 5000, "total transfer deadline");
static_assert(BoardExportTransfer::kChunkTimeoutMs == 1000, "per-query deadline");
constexpr v4::Kind kind = static_cast<v4::Kind>(18);
static_assert(v4::Kind::MigrationRead == kind, "independent records-pull kind");
constexpr const char* device = "Babytech_01-test";
constexpr const char* physical = "012345abcdef";
constexpr const char* brainPhysical = "fedcba987654";
constexpr const char* challenge = "0123456789abcdef0123456789abcdef";
constexpr const char* nextChallenge = "1123456789abcdef0123456789abcdef";
constexpr const char* secret = "NEVER_EXPORT_CREDENTIAL_79f2";
constexpr uint64_t motionBoot = UINT64_C(0xfedcba9876543210);
constexpr uint64_t brainBoot = UINT64_C(0x0123456789abcdef);
constexpr uint32_t captured = 123;
unsigned scenarios = 0, failures = 0;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + \
    std::to_string(__LINE__) + ": " #x); } while (false)

void readOnly(const fake::Call& call) {
    CHECK(call.op == Op::OpenRO || call.op == Op::Query || call.op == Op::Read || call.op == Op::Close);
    CHECK(call.name == "productpair" || call.name == "productstate" ||
          call.name == "productctx" || call.name == "formulaevt");
    if (call.op == Op::Query || call.op == Op::Read) {
        CHECK(call.key == ((call.name == "productctx" || call.name == "formulaevt") ? "payload" : "record"));
        CHECK(call.name != "formulaevt" || call.op == Op::Query);
    }
}

fake::Value stringValue(const std::string& text) {
    Bytes bytes(text.begin(), text.end());
    bytes.push_back(0);
    return {bytes, fake::Type::String};
}

void scenario(const std::string& name, const std::function<void()>& body) {
    ++scenarios;
    fake::reset();
    fake_commissioning::reset();
    fake_product_crypto::reset();
    allocation_fault::kind = allocation_fault::Kind::None;
    allocation_fault::hits = 0;
    for (const char* space : {"wifi", "mqtt", "credentials", "productpair", "productstate", "productctx", "formulaevt"})
        for (const char* key : {"password", "token", "secret", "ssid", "private_key"})
            io.disk[space][key] = stringValue(secret);
    io.before = readOnly;
    try {
        body();
        CHECK(allocation_fault::kind == allocation_fault::Kind::None);
        CHECK(io.handles.empty());
        for (auto op : {Op::OpenRW, Op::Set, Op::Commit, Op::Erase, Op::Init}) CHECK(fake::count(op) == 0);
        for (const auto& call : io.calls) readOnly(call);
        fake::verifyFaults();
    } catch (const std::exception& error) {
        allocation_fault::kind = allocation_fault::Kind::None;
        ++failures;
        std::fprintf(stderr, "FAIL %s: %s\n", name.c_str(), error.what());
    }
}

template<typename T, size_t N>
Bytes encoded(const T& value, size_t (*encoder)(const T&, uint8_t*, size_t)) {
    std::array<uint8_t, N> bytes{};
    const auto n = encoder(value, bytes.data(), bytes.size());
    CHECK(n > 0 && n <= N);
    return Bytes(bytes.begin(), bytes.begin() + n);
}
Bytes pairBytes(const v4::Pairing& p) { return encoded<v4::Pairing, kPairingRecordMaxSize>(p, encodePairingRecord); }
Bytes stateBytes(const MotionState& s) { return encoded<MotionState, kMotionStateMaxSize>(s, encodeMotionState); }
Bytes contextBytes(const ProductContext& c) { return encoded<ProductContext, kContextIdentityMaxSize>(c, encodeContextIdentity); }

v4::Pairing pairing(const std::string& id = device) {
    v4::Pairing p;
    p.role = v4::Role::Motion;
    std::strcpy(p.deviceId, id.c_str());
    std::strcpy(p.epoch, challenge);
    std::strcpy(p.localPhysicalId, physical);
    std::strcpy(p.peerPhysicalId, brainPhysical);
    CHECK(v4::validPairing(p));
    return p;
}

ProductContext context(bool cleared = false, bool maximum = false, const std::string& id = device) {
    ProductContext c;
    std::strcpy(c.deviceId, id.c_str());
    c.profileVersion = INT32_MAX;
    c.cleared = cleared;
    if (!cleared) {
        std::strcpy(c.babyId, (maximum ? std::string(96, 'i') : "baby-1").c_str());
        std::strcpy(c.babyName, (maximum ? std::string(320, 'n') : "Full \"name\" \\ \xe5\xae\x9d").c_str());
        std::strcpy(c.formulaBrand, (maximum ? std::string(480, 'f') : "Full formula brand").c_str());
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
    std::string text;
    serializeJson(doc, text);
    return text;
}

ProductRequest productRequest(const v4::Pairing& p, const ProductContext& c, uint64_t sequence) {
    ProductRequest r;
    r.command = c.cleared ? ProductCommand::Clean : ProductCommand::Prepare;
    r.sequence = sequence;
    std::strcpy(r.deviceId, p.deviceId);
    auto brain = p;
    brain.role = v4::Role::Brain;
    std::swap(brain.localPhysicalId, brain.peerPhysicalId);
    CHECK(makeLocalCommandId(brain, sequence, r.commandId));
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

MotionExecutionSlot terminal(const v4::Pairing& p, const ProductContext& c, uint64_t sequence) {
    MotionExecutionSlot s;
    s.kind = MotionSlotKind::Terminal;
    s.request = productRequest(p, c, sequence);
    CHECK(requestDigest(s.request, s.digest));
    std::snprintf(s.executionId, sizeof(s.executionId), "%032llx", static_cast<unsigned long long>(sequence));
    CHECK(makeProductEventId(p, s.request.source, sequence, s.eventId));
    s.targetPowderG = productTargetPowderG(s.request);
    s.completed = sequence % 2 != 0;
    s.uptimeMs = UINT32_MAX;
    if (!s.completed) { std::strcpy(s.reason, "interrupted"); std::strcpy(s.errorCode, "E_POWER"); }
    return s;
}

MotionState motionState(const ProductContext& c, unsigned queueCount = 4, bool withSlot = false) {
    MotionState s;
    s.pairing = pairing(c.deviceId);
    CHECK(makeMotionContextBarrier(c, s.context));
    s.localSequence = s.cloudSequence = v4::kMaxSequence;
    s.cloudResult.kind = MotionResultKind::CloudStop;
    s.cloudResult.stopSequence = s.cloudSequence;
    std::strcpy(s.cloudResult.stopCommandId, std::string(128, 's').c_str());
    std::strcpy(s.cloudResult.stopExecutionId, challenge);
    std::strcpy(s.cloudResult.reason, "busy");
    s.localResult.kind = MotionResultKind::Ordinary;
    s.localResult.request = productRequest(s.pairing, c, s.localSequence);
    CHECK(requestDigest(s.localResult.request, s.localResult.digest));
    s.localResult.accepted = true;
    std::strcpy(s.localResult.reason, "accepted");
    if (c.cleared) {
        s.slot.kind = MotionSlotKind::Intent;
        s.slot.request = s.localResult.request;
        std::memcpy(s.slot.digest, s.localResult.digest, sizeof(s.slot.digest));
        std::strcpy(s.slot.executionId, challenge);
    } else {
        s.pendingResultCount = uint8_t(queueCount);
        for (unsigned i = 0; i < queueCount; ++i) s.pendingResults[i] = terminal(s.pairing, c, i + 1);
        if (withSlot) s.slot = terminal(s.pairing, c, s.localSequence);
    }
    CHECK(validMotionState(s));
    return s;
}

void seed(const ProductContext& c, unsigned queueCount = 4, bool withSlot = false) {
    io.disk["productpair"]["record"] = {pairBytes(pairing(c.deviceId)), fake::Type::Blob};
    io.disk["productstate"]["record"] = {stateBytes(motionState(c, queueCount, withSlot)), fake::Type::Blob};
    io.disk["productctx"]["payload"] = stringValue(legacyJson(c));
}

std::string capture(const std::string& id = device, uint32_t now = captured, const char* token = challenge) {
    const auto disk = io.disk;
    MaintenanceExport source;
    CHECK(source.begin(v4::Role::Motion, id.c_str(), token, motionBoot, now));
    std::string text;
    std::array<uint8_t, 155> bytes{};
    while (source.active()) {
        const auto n = source.peek(bytes.data(), bytes.size());
        CHECK(n > 0 && text.size() + n <= kMotionExportWireMaxSize);
        text.append(reinterpret_cast<const char*>(bytes.data()), n);
        source.consume(n);
    }
    CHECK(io.disk == disk && text.find(secret) == std::string::npos);
    return text;
}

bool decode(const std::string& text, MotionExportSnapshot& output, const std::string& id = device,
            uint32_t /*now*/ = captured, const char* token = challenge) {
    std::vector<char> buffer(text.begin(), text.end());
    buffer.push_back(0);
    return decodeMotionExport(buffer.data(), text.size(), id.c_str(), physical, motionBoot, token, output);
}

std::string change(const std::string& text, const std::string& from, const std::string& to) {
    const auto at = text.find(from);
    CHECK(at != std::string::npos);
    auto result = text;
    result.replace(at, from.size(), to);
    return result;
}

bool sameFrame(const v4::Frame& a, const v4::Frame& b) {
    return a.kind == b.kind && a.senderBoot == b.senderBoot && a.receiverBoot == b.receiverBoot &&
        a.messageId == b.messageId && a.total == b.total && a.offset == b.offset && a.length == b.length &&
        !std::memcmp(a.payload, b.payload, sizeof(a.payload));
}

Bytes wire(const v4::Frame& frame) {
    Bytes bytes(v4::kMaxFrame + 1, 0xa5);
    const auto n = v4::encode(frame, bytes.data(), v4::kMaxFrame);
    CHECK(n == v4::kHeaderSize + frame.length + 2 && bytes[n] == 0xa5);
    bytes.resize(n);
    return bytes;
}

v4::Frame throughCodec(const v4::Frame& frame, uint32_t now = captured) {
    v4::Parser parser;
    v4::Frame result;
    unsigned count = 0;
    for (uint8_t byte : wire(frame)) if (parser.push(byte, now, result)) ++count;
    CHECK(count == 1 && sameFrame(result, frame));
    return result;
}

DiscoveryResult discover(const std::string& id = device) {
    BoardDiscovery brain, motion;
    v4::Pairing pair;
    bool ready = false;
    const auto space = io.disk.find("productpair");
    if (space != io.disk.end()) {
        const auto record = space->second.find("record");
        if (record != space->second.end() && record->second.type == fake::Type::Blob) {
            const auto& bytes = record->second.bytes;
            ready = decodePairingRecord(bytes.data(), bytes.size(), pair) &&
                pair.role == v4::Role::Motion && id == pair.deviceId &&
                !std::strcmp(pair.localPhysicalId, physical) && !std::strcmp(pair.peerPhysicalId, brainPhysical);
        }
    }
    CHECK(brain.begin(v4::Role::Brain, brainPhysical, brainBoot, DiscoveryPairState::Missing));
    CHECK(motion.begin(v4::Role::Motion, physical, motionBoot,
                       ready ? DiscoveryPairState::Ready : DiscoveryPairState::Missing, ready ? &pair : nullptr));
    CHECK(brain.request(id.c_str(), 1) && brain.outgoing());
    const auto query = throughCodec(*brain.outgoing(), 1);
    brain.queued();
    motion.receive(query, 1);
    CHECK(motion.outgoing());
    brain.receive(throughCodec(*motion.outgoing(), 1), 1);
    motion.queued();
    CHECK(brain.result().state == DiscoveryState::Found && brain.result().peerBoot == motionBoot);
    return brain.result();
}

uint16_t le16(const uint8_t* bytes) { return uint16_t(unsigned(bytes[0]) | (unsigned(bytes[1]) << 8)); }

v4::Frame query(uint16_t offset = 0, uint32_t id = 101, const std::string& target = device,
                const char* token = challenge) {
    CHECK(!target.empty() && target.size() <= 64);
    v4::Frame f;
    f.kind = kind;
    f.senderBoot = brainBoot;
    f.receiverBoot = motionBoot;
    f.messageId = id;
    f.payload[0] = 1;
    f.payload[1] = uint8_t(target.size());
    std::memcpy(f.payload + 2, target.data(), target.size());
    std::memcpy(f.payload + 2 + target.size(), brainPhysical, 12);
    std::memcpy(f.payload + 14 + target.size(), token, 32);
    f.payload[46 + target.size()] = uint8_t(offset);
    f.payload[47 + target.size()] = uint8_t(offset >> 8);
    f.total = f.length = uint16_t(48 + target.size());
    return f;
}

v4::Frame reply(const v4::Frame& q, const std::string& bytes, uint16_t offset, bool done) {
    CHECK(bytes.size() <= 155);
    v4::Frame f;
    f.kind = kind;
    f.senderBoot = motionBoot;
    f.receiverBoot = q.senderBoot;
    f.messageId = q.messageId;
    f.payload[0] = 2;
    f.payload[1] = uint8_t(offset);
    f.payload[2] = uint8_t(offset >> 8);
    f.payload[3] = uint8_t(bytes.size());
    f.payload[4] = done ? 1 : 0;
    std::memcpy(f.payload + 5, bytes.data(), bytes.size());
    f.total = f.length = uint16_t(5 + bytes.size());
    return f;
}

void checkQuery(const v4::Frame& q, size_t offset, const std::string& id = device,
                const char* token = challenge) {
    CHECK(q.kind == kind && q.messageId && q.senderBoot == brainBoot && q.receiverBoot == motionBoot);
    CHECK(q.offset == 0 && q.total == q.length && q.length == 48 + id.size());
    CHECK(q.payload[0] == 1 && q.payload[1] == id.size());
    CHECK(!std::memcmp(q.payload + 2, id.data(), id.size()));
    CHECK(!std::memcmp(q.payload + 2 + id.size(), brainPhysical, 12));
    CHECK(!std::memcmp(q.payload + 14 + id.size(), token, 32));
    CHECK(le16(q.payload + 46 + id.size()) == offset);
}

void checkReply(const v4::Frame& r, const v4::Frame& q, size_t offset) {
    CHECK(r.kind == kind && r.senderBoot == motionBoot && r.receiverBoot == brainBoot && r.messageId == q.messageId);
    CHECK(r.offset == 0 && r.total == r.length && r.length >= 5 && r.length <= 160);
    CHECK(r.payload[0] == 2 && le16(r.payload + 1) == offset);
    CHECK(r.payload[3] <= 155 && r.length == 5 + r.payload[3] && r.payload[4] <= 1);
}

struct TrackedSource : BoardExportSource {
    MaintenanceExport production;
    unsigned begins = 0, consumes = 0, cancels = 0;
    size_t consumed = 0;
    bool begin(v4::Role role, const char* id, const char* token, uint64_t boot, uint32_t now) override {
        ++begins;
        return production.begin(role, id, token, boot, now);
    }
    size_t remaining() const override { return production.remaining(); }
    size_t peek(uint8_t* bytes, size_t capacity) const override { return production.peek(bytes, capacity); }
    void consume(size_t length) override { ++consumes; consumed += length; production.consume(length); }
    void cancel() override { ++cancels; production.cancel(); }
};

struct RawSource : BoardExportSource {
    std::string text;
    size_t position = 0;
    unsigned begins = 0, consumes = 0, cancels = 0;
    bool available = true, active = false;
    explicit RawSource(std::string bytes) : text(std::move(bytes)) {}
    bool begin(v4::Role role, const char*, const char*, uint64_t boot, uint32_t) override {
        CHECK(role == v4::Role::Motion && boot == motionBoot);
        ++begins;
        if (!available || active) return false;
        position = 0; active = true; return true;
    }
    size_t remaining() const override { return active ? text.size() - position : 0; }
    size_t peek(uint8_t* bytes, size_t capacity) const override {
        const auto n = std::min(capacity, remaining());
        if (n) std::memcpy(bytes, text.data() + position, n);
        return n;
    }
    void consume(size_t length) override {
        CHECK(active && length <= remaining());
        ++consumes; position += length;
        if (position == text.size()) active = false;
    }
    void cancel() override { ++cancels; active = false; }
};

struct Harness {
    BoardExportTransfer brain, motion;
    DiscoveryResult peer;
    std::string id;
    explicit Harness(BoardExportSource& source, const std::string& target = device) : peer(discover(target)), id(target) {
        CHECK(brain.begin(v4::Role::Brain, brainPhysical, brainBoot));
        CHECK(motion.begin(v4::Role::Motion, physical, motionBoot));
        CHECK(motion.setSource(&source));
    }
    void start(uint32_t now = captured, const char* token = challenge) {
        CHECK(brain.request(id.c_str(), peer, token, now));
        CHECK(brain.state() == ExportTransferState::Pending && !brain.snapshot() && brain.outgoing());
    }
    std::string run(uint32_t now = captured, const char* token = challenge, std::string raw = {}) {
        uint32_t lastId = 0;
        for (unsigned chunk = 0; brain.state() == ExportTransferState::Pending; ++chunk) {
            CHECK(chunk < 200 && brain.outgoing());
            const auto q = *brain.outgoing();
            checkQuery(q, raw.size(), id, token);
            CHECK(q.messageId != lastId);
            lastId = q.messageId;
            brain.queued();
            CHECK(!brain.outgoing());
            motion.receive(throughCodec(q, now), now);
            CHECK(motion.outgoing());
            const auto r = *motion.outgoing();
            checkReply(r, q, raw.size());
            CHECK(r.payload[3] > 0);
            raw.append(reinterpret_cast<const char*>(r.payload + 5), r.payload[3]);
            const bool done = r.payload[4] == 1;
            motion.queued();
            CHECK(!motion.outgoing());
            brain.receive(throughCodec(r, now), now);
            if (!done) CHECK(brain.state() == ExportTransferState::Pending && !brain.snapshot() && brain.outgoing());
            now += 1;
        }
        CHECK(raw.find(secret) == std::string::npos);
        return raw;
    }
};

void metadata(const MotionExportSnapshot& s, const std::string& id = device, uint32_t now = captured) {
    CHECK(id == s.deviceId && !std::strcmp(s.physicalId, physical) && !std::strcmp(s.challenge, challenge));
    CHECK(s.boot == motionBoot && s.capturedAtMs == now);
}

void records() {
    scenario("one-byte device and nonzero challenge at final nibble roundtrip", [] {
        const std::string id = "0";
        const char* token = "00000000000000000000000000000001";
        TrackedSource source; Harness h(source, id); h.start(captured, token); h.run(captured, token);
        CHECK(h.brain.state() == ExportTransferState::Complete && h.brain.snapshot());
        CHECK(id == h.brain.snapshot()->deviceId && !std::strcmp(h.brain.snapshot()->challenge, token));
        CHECK(source.begins == 1);
    });
    for (bool cleared : {false, true}) for (bool maximum : {false, true})
        scenario("unpaired full old context/tombstone is diagnostic only", [=] {
            const auto id = maximum ? std::string(64, 'D') : device;
            const auto c = context(cleared, maximum, id);
            io.disk["productctx"]["payload"] = stringValue(legacyJson(c));
            const auto disk = io.disk;
            TrackedSource source;
            Harness h(source, id);
            h.start(); h.run();
            CHECK(h.brain.state() == ExportTransferState::Complete && h.brain.snapshot());
            const auto& s = *h.brain.snapshot();
            metadata(s, id);
            CHECK(s.pairStatus == ExportRead::Missing && s.stateStatus == ExportRead::Missing);
            CHECK(s.legacyStatus == ExportRead::Ready && s.legacyEvent == ExportRead::Missing);
            CHECK(contextBytes(s.legacy) == contextBytes(c));
            CHECK(source.begins == 1 && source.remaining() == 0 && io.disk == disk);
            if (maximum && !cleared) CHECK(contextBytes(c).size() == kContextIdentityMaxSize);
        });
    for (bool cleared : {false, true}) for (bool withSlot : {false, true})
        scenario("pair, state, both maximum watermarks and persistent result queue", [=] {
            const auto c = context(cleared, true, std::string(64, 'D'));
            seed(c, withSlot ? 3 : 4, withSlot);
            const auto disk = io.disk;
            TrackedSource source;
            Harness h(source, c.deviceId);
            h.start(); const auto raw = h.run();
            CHECK(h.brain.state() == ExportTransferState::Complete && h.brain.snapshot());
            const auto& s = *h.brain.snapshot();
            metadata(s, c.deviceId);
            CHECK(s.pairStatus == ExportRead::Ready && s.stateStatus == ExportRead::Ready && s.legacyStatus == ExportRead::Ready);
            CHECK(pairBytes(s.pairing) == disk.at("productpair").at("record").bytes);
            CHECK(stateBytes(s.state) == disk.at("productstate").at("record").bytes);
            CHECK(sameMotionState(s.state, motionState(c, withSlot ? 3 : 4, withSlot)));
            CHECK(contextBytes(s.legacy) == contextBytes(c));
            CHECK(s.state.localSequence == v4::kMaxSequence && s.state.cloudSequence == v4::kMaxSequence);
            CHECK(source.begins == 1 && source.consumed == raw.size() && io.disk == disk);
        });
    for (unsigned variant = 0; variant < 12; ++variant)
        scenario("declared corrupt/io/identity/conflict/pending-legacy facts survive pull", [=] {
            seed(context());
            ExportRead expected = ExportRead::Corrupt;
            if (variant == 0) io.disk["productpair"]["record"].bytes[0] ^= 1;
            if (variant == 1) io.disk["productstate"]["record"].bytes.back() ^= 1;
            if (variant == 2) io.disk["productctx"]["payload"] = stringValue("{bad");
            if (variant == 3) { io.disk["productstate"]["record"].type = fake::Type::String; expected = ExportRead::IoError; }
            if (variant == 4) { io.disk["formulaevt"]["payload"] = stringValue(secret); expected = ExportRead::Present; }
            if (variant == 5) {
                auto p = pairing(); p.epoch[0] = '9';
                io.disk["productpair"]["record"].bytes = pairBytes(p); expected = ExportRead::Conflict;
            }
            if (variant == 6) {
                auto p = pairing("other-device");
                io.disk["productpair"]["record"].bytes = pairBytes(p); expected = ExportRead::IdentityMismatch;
            }
            if (variant == 7) { fake::fail(Op::OpenRO, 1); expected = ExportRead::IoError; }
            if (variant == 8) { io.disk["productctx"]["payload"].type = fake::Type::Blob; expected = ExportRead::IoError; }
            if (variant == 9) io.disk["formulaevt"]["payload"] = stringValue("");
            if (variant == 10) { io.disk["formulaevt"]["payload"] = {{1, 2}, fake::Type::Blob}; expected = ExportRead::IoError; }
            if (variant == 11) { io.disk["productpair"]["record"].type = fake::Type::String; expected = ExportRead::IoError; }
            const auto disk = io.disk;
            TrackedSource source; Harness h(source); h.start(); h.run();
            CHECK(h.brain.state() == ExportTransferState::Complete && h.brain.snapshot());
            const auto& s = *h.brain.snapshot();
            const auto actual = variant == 0 || variant == 6 || variant == 7 || variant == 11 ? s.pairStatus :
                variant == 2 || variant == 8 ? s.legacyStatus : variant == 4 || variant == 9 || variant == 10 ? s.legacyEvent : s.stateStatus;
            CHECK(actual == expected && io.disk == disk);
        });
    scenario("capture is immutable after offset zero and does not lazily reread", [] {
        const auto c = context(); seed(c);
        const auto originalState = io.disk.at("productstate").at("record").bytes;
        TrackedSource source; Harness h(source); h.start();
        const auto q = *h.brain.outgoing(); h.brain.queued();
        h.motion.receive(throughCodec(q), captured);
        const auto reads = io.calls.size();
        const auto first = *h.motion.outgoing();
        CHECK(source.consumed == 0 && source.begins == 1);
        io.disk["productstate"]["record"].bytes = {0};
        io.disk["productctx"]["payload"] = stringValue(legacyJson(context(true)));
        h.motion.queued(); h.brain.receive(throughCodec(first), captured);
        // run() expects offset zero, so finish manually with the first chunk retained.
        size_t offset = first.payload[3];
        for (unsigned chunk = 0; h.brain.state() == ExportTransferState::Pending; ++chunk) {
            CHECK(chunk < 200 && h.brain.outgoing());
            const auto next = *h.brain.outgoing(); checkQuery(next, offset); h.brain.queued();
            h.motion.receive(throughCodec(next), captured);
            CHECK(h.motion.outgoing()); const auto r = *h.motion.outgoing(); checkReply(r, next, offset);
            offset += r.payload[3]; h.motion.queued(); h.brain.receive(throughCodec(r), captured);
        }
        CHECK(h.brain.state() == ExportTransferState::Complete && h.brain.snapshot());
        CHECK(stateBytes(h.brain.snapshot()->state) == originalState);
        CHECK(contextBytes(h.brain.snapshot()->legacy) == contextBytes(c));
        CHECK(source.begins == 1 && io.calls.size() == reads);
    });
}

std::string hexDamage(const std::string& text, const char* field) {
    const auto start = text.find(std::string("\"") + field + "\":\"");
    CHECK(start != std::string::npos);
    const auto at = start + std::strlen(field) + 4;
    CHECK(text[at] != '"');
    auto result = text; result[at] = result[at] == '0' ? '1' : '0'; return result;
}

void decoder() {
    scenario("strict final decoder rejects metadata/schema/hex/CRC/length without changing output", [] {
        seed(context());
        const auto raw = capture();
        MotionExportSnapshot good;
        CHECK(decode(raw, good)); metadata(good);
        std::vector<std::string> invalid = {
            change(raw, "\"schema\":1", "\"schema\":2"),
            change(raw, "\"schema\":1", "\"schema\":\"1\""),
            change(raw, "\"role\":\"motion\"", "\"role\":\"brain\""),
            change(raw, device, "other-device"), change(raw, physical, brainPhysical),
            change(raw, "fedcba9876543210", "fedcba9876543211"), change(raw, challenge, nextChallenge),
            change(raw, "\"captured_ms\":123", "\"captured_ms\":-1"),
            change(raw, "\"captured_ms\":123", "\"captured_ms\":4294967296"),
            change(raw, "\"pair_status\":\"ready\"", "\"pair_status\":\"unknown\""),
            change(raw, "\"pair_status\":\"ready\"", "\"pair_status\":\"missing\""),
            change(raw, "\"state_status\":\"ready\"", "\"state_status\":\"corrupt\""),
            change(raw, "\"legacy_status\":\"ready\"", "\"legacy_status\":\"missing\""),
            change(raw, "\"legacy_event\":\"missing\"", "\"legacy_event\":\"ready\""),
            change(raw, "\"pair_hex\":\"", "\"pair_hex\":\"g"),
            change(raw, "\"state_hex\":\"", "\"state_hex\":\"0"),
            hexDamage(raw, "pair_hex"), hexDamage(raw, "state_hex"), hexDamage(raw, "legacy_hex"),
            change(raw, "\"pair_status\":\"ready\",", ""),
            change(raw, "\"schema\":1,", "\"schema\":1,\"unexpected\":true,"),
            raw.substr(0, raw.size() - 2), raw + "{}", raw + "garbage"
        };
        auto embedded = raw; embedded[embedded.size() / 2] = 0; invalid.push_back(embedded);
        for (const auto& text : invalid) {
            auto output = good;
            std::array<uint8_t, sizeof(MotionExportSnapshot)> before{};
            std::memcpy(before.data(), &output, sizeof(output));
            CHECK(!decode(text, output));
            CHECK(!std::memcmp(before.data(), &output, sizeof(output)));
        }
        auto output = good;
        CHECK(!decodeMotionExport(nullptr, 0, device, physical, motionBoot, challenge, output));
        CHECK(!decode("", output));
    });
    scenario("ready with missing bytes and internal record identities cannot pass", [] {
        seed(context()); const auto raw = capture();
        for (const char* field : {"pair_hex", "state_hex", "legacy_hex"}) {
            auto text = raw;
            const auto at = text.find(std::string("\"") + field + "\":\"") + std::strlen(field) + 4;
            const auto end = text.find('"', at);
            CHECK(end != std::string::npos); text.erase(at, end - at);
            MotionExportSnapshot s; CHECK(!decode(text, s));
        }
        // Produce valid-CRC records, but lie in the outer status/identity metadata.
        auto p = pairing("other-device");
        const auto bytes = pairBytes(p);
        constexpr char digits[] = "0123456789abcdef";
        std::string hex;
        for (auto byte : bytes) { hex += digits[byte >> 4]; hex += digits[byte & 15]; }
        auto text = raw;
        const auto at = text.find("\"pair_hex\":\"") + 12;
        text.replace(at, text.find('"', at) - at, hex);
        MotionExportSnapshot s; CHECK(!decode(text, s));
    });
    scenario("Brain exposes no partial snapshot and rejects corrupt export only at final chunk", [] {
        seed(context()); RawSource source(hexDamage(capture(), "state_hex"));
        Harness h(source); h.start(); h.run();
        CHECK(h.brain.state() == ExportTransferState::Invalid && !h.brain.snapshot() && !h.brain.outgoing());
    });
}

void ignoredReply(BoardExportTransfer& brain, const v4::Frame& frame, uint32_t now) {
    const auto state = brain.state();
    const bool pending = brain.outgoing() != nullptr;
    v4::Frame outgoing;
    if (pending) outgoing = *brain.outgoing();
    brain.receive(frame, now);
    CHECK(brain.state() == state && !brain.snapshot());
    CHECK((brain.outgoing() != nullptr) == pending);
    if (pending) CHECK(sameFrame(*brain.outgoing(), outgoing));
}

void frames() {
    scenario("wire Kind18 single frames, CRC damage, independent discovery IDs", [] {
        const auto q = query(0x1234, 77, std::string(64, 'D'));
        CHECK(v4::validFrame(q)); checkQuery(throughCodec(q), 0x1234, std::string(64, 'D'));
        auto r = reply(q, std::string(155, 'x'), 0x1234, false);
        checkReply(throughCodec(r), q, 0x1234);
        // The general codec may encode fragments. The transfer, not an unrelated
        // protocol-wide restriction, owns rejection of fragmented records pulls.
        RawSource rejectSource(capture()); Harness rejected(rejectSource);
        auto fragmented = q; fragmented.total += 1;
        rejected.motion.receive(fragmented, captured);
        fragmented = q; fragmented.offset = 1; fragmented.total += 1;
        rejected.motion.receive(fragmented, captured);
        CHECK(rejectSource.begins == 0 && !rejected.motion.outgoing());
        auto bytes = wire(r); bytes.back() ^= 1;
        v4::Parser parser; v4::Frame output; unsigned parsed = 0;
        for (auto byte : bytes) if (parser.push(byte, captured, output)) ++parsed;
        CHECK(parsed == 0);
        for (auto byte : wire(r)) if (parser.push(byte, captured, output)) ++parsed;
        CHECK(parsed == 1);
        BoardDiscovery discoveryBrain, discoveryMotion;
        CHECK(discoveryBrain.begin(v4::Role::Brain, brainPhysical, brainBoot, DiscoveryPairState::Missing));
        CHECK(discoveryMotion.begin(v4::Role::Motion, physical, motionBoot, DiscoveryPairState::Missing));
        const auto exchange = [&](uint32_t expectedId) {
            CHECK(discoveryBrain.request(device, captured));
            CHECK(discoveryBrain.outgoing() && discoveryBrain.outgoing()->messageId == expectedId);
            const auto ask = *discoveryBrain.outgoing(); discoveryBrain.queued();
            discoveryMotion.receive(throughCodec(ask), captured);
            CHECK(discoveryMotion.outgoing()); const auto answer = *discoveryMotion.outgoing(); discoveryMotion.queued();
            discoveryBrain.receive(throughCodec(answer), captured);
            CHECK(discoveryBrain.result().state == DiscoveryState::Found);
        };
        exchange(1);
        RawSource source(capture()); Harness h(source); h.start(); h.run();
        CHECK(h.brain.state() == ExportTransferState::Complete);
        exchange(2);
    });
    for (unsigned mutation = 0; mutation < 25; ++mutation)
        scenario("malformed/out-of-order/foreign query unchanged, mutation=" + std::to_string(mutation), [=] {
            seed(context()); TrackedSource source; Harness h(source);
            const auto first = query(); h.motion.receive(first, captured);
            CHECK(h.motion.outgoing()); const auto original = *h.motion.outgoing();
            auto bad = query(0, 102);
            const auto n = std::strlen(device);
            switch (mutation) {
                case 0: bad.kind = v4::Kind::Command; break;
                case 1: bad.senderBoot = 0; break;
                case 2: bad.receiverBoot = 0; break;
                case 3: bad.receiverBoot ^= 1; break;
                case 4: bad.messageId = 0; break;
                case 5: bad.offset = 1; bad.total += 1; break;
                case 6: bad.total += 1; break;
                case 7: --bad.length; --bad.total; break;
                case 8: ++bad.length; ++bad.total; break;
                case 9: bad.payload[0] = 2; break;
                case 10: bad.payload[1] = 0; break;
                case 11: bad.payload[1] = 65; break;
                case 12: bad.payload[2] = '_'; break;
                case 13: bad.payload[2] = 0; break;
                case 14: bad.payload[2 + n] = 'A'; break;
                case 15: std::memset(bad.payload + 2 + n, '0', 12); break;
                case 16: bad.payload[14 + n] = 'A'; break;
                case 17: std::memset(bad.payload + 14 + n, '0', 32); break;
                case 18: bad.payload[46 + n] = 1; break;
                case 19: bad.senderBoot ^= 1; break;
                case 20: bad.payload[2 + n] = '9'; break;
                case 21: bad.payload[14 + n] = '9'; bad.senderBoot ^= 1; break;
                case 22: bad.payload[2] = '9'; break;
                case 23: bad.payload[14 + n] = '9'; bad.payload[46 + n] = 1; break;
                case 24: bad.payload[14 + n] = '9'; bad.payload[46 + n] = 155; break;
            }
            const auto begins = source.begins, cancels = source.cancels, consumes = source.consumes;
            const auto remaining = source.remaining(), reads = io.calls.size();
            h.motion.receive(bad, captured + 1);
            CHECK(source.begins == begins && source.cancels == cancels && source.consumes == consumes);
            CHECK(source.remaining() == remaining && io.calls.size() == reads);
            CHECK(h.motion.outgoing() && sameFrame(*h.motion.outgoing(), original));
            // Commit the original response, then reject the same request after queueing.
            h.motion.queued();
            const auto consumed = source.consumed;
            h.motion.receive(bad, captured + 2);
            CHECK(source.begins == begins && source.cancels == cancels && source.consumed == consumed);
            if (h.motion.outgoing()) {
                const auto error = *h.motion.outgoing();
                CHECK(error.payload[0] == 3 && error.length == 1 && error.total == 1 && error.offset == 0);
                h.motion.queued(); CHECK(source.consumed == consumed);
            }
            auto next = query(uint16_t(consumed), 103);
            h.motion.receive(next, captured + 3);
            CHECK(h.motion.outgoing()); checkReply(*h.motion.outgoing(), next, consumed);
            CHECK(source.begins == 1);
        });
    for (unsigned mutation = 0; mutation < 18; ++mutation)
        scenario("Brain rejects wrong envelopes/malformed replies unchanged, mutation=" + std::to_string(mutation), [=] {
            RawSource source(capture()); Harness h(source); h.start();
            const auto q = *h.brain.outgoing(); h.brain.queued();
            auto r = reply(q, "x", 0, false);
            switch (mutation) {
                case 0: r.kind = v4::Kind::Discovery; break;
                case 1: ++r.messageId; break;
                case 2: r.senderBoot ^= 1; break;
                case 3: r.receiverBoot ^= 1; break;
                case 4: r.senderBoot = 0; break;
                case 5: r.receiverBoot = 0; break;
                case 6: r.messageId = 0; break;
                case 7: r.offset = 1; ++r.total; break;
                case 8: ++r.total; break;
                case 9: --r.length; --r.total; break;
                case 10: ++r.length; ++r.total; break;
                case 11: r.payload[0] = 9; break;
                case 12: r.payload[1] = 1; break;
                case 13: r.payload[3] = 156; break;
                case 14: r.payload[4] = 2; break;
                case 15: r = reply(q, "", 0, false); break;
                case 16: r.payload[0] = 3; break; // Error must be exactly one byte.
                case 17: r.length = r.total = 0; break;
            }
            ignoredReply(h.brain, r, captured + 1);
            h.brain.receive(reply(q, source.text.substr(0, 155), 0, false), captured + 2);
            CHECK(h.brain.outgoing()); checkQuery(*h.brain.outgoing(), 155);
        });
    scenario("foreign/replayed replies cannot advance or corrupt the next chunk", [] {
        seed(context()); const auto raw = capture(); RawSource source(raw); Harness h(source); h.start();
        const auto q = *h.brain.outgoing(); h.brain.queued();
        const auto first = reply(q, raw.substr(0, 155), 0, false);
        h.brain.receive(first, captured);
        CHECK(h.brain.outgoing()); const auto next = *h.brain.outgoing(); checkQuery(next, 155);
        ignoredReply(h.brain, first, captured + 1);
        h.brain.queued(); ignoredReply(h.brain, first, captured + 2);
        size_t offset = 155;
        auto current = next;
        while (offset < raw.size()) {
            const auto n = std::min(size_t(155), raw.size() - offset);
            const auto r = reply(current, raw.substr(offset, n), uint16_t(offset), offset + n == raw.size());
            h.brain.receive(throughCodec(r), captured + 3); offset += n;
            if (offset < raw.size()) { CHECK(h.brain.outgoing()); current = *h.brain.outgoing(); h.brain.queued(); }
        }
        CHECK(h.brain.state() == ExportTransferState::Complete && h.brain.snapshot());
        CHECK(stateBytes(h.brain.snapshot()->state) == io.disk.at("productstate").at("record").bytes);
    });
    scenario("source unavailable replies with op3 only, no snapshot or automatic retry", [] {
        RawSource source("unused"); source.available = false;
        Harness h(source); h.start(); const auto q = *h.brain.outgoing(); h.brain.queued();
        h.motion.receive(throughCodec(q), captured);
        CHECK(h.motion.outgoing()); const auto error = *h.motion.outgoing();
        CHECK(error.kind == kind && error.messageId == q.messageId && error.receiverBoot == brainBoot);
        CHECK(error.payload[0] == 3 && error.length == 1 && error.total == 1 && error.offset == 0);
        h.motion.queued(); h.brain.receive(throughCodec(error), captured);
        CHECK(h.brain.state() == ExportTransferState::Unavailable && !h.brain.snapshot());
        h.brain.poll(captured + 5000); CHECK(!h.brain.outgoing());
        CHECK(source.consumes == 0);
    });
    scenario("used query messageID at next offset must not advance Motion capture", [] {
        seed(context()); TrackedSource source; Harness h(source); h.start();
        const auto first = *h.brain.outgoing(); h.brain.queued();
        h.motion.receive(first, captured); CHECK(h.motion.outgoing());
        const auto firstReply = *h.motion.outgoing(); h.motion.queued();
        h.brain.receive(firstReply, captured);
        CHECK(h.brain.outgoing()); const auto next = *h.brain.outgoing();
        const auto consumed = source.consumed, remaining = source.remaining();
        auto staleId = next; staleId.messageId = first.messageId;
        h.motion.receive(staleId, captured + 1);
        if (h.motion.outgoing()) {
            const auto response = *h.motion.outgoing();
            h.motion.queued();
            h.brain.receive(response, captured + 1);
            CHECK(h.brain.outgoing() && sameFrame(*h.brain.outgoing(), next));
        }
        CHECK(source.consumed == consumed && source.remaining() == remaining && source.begins == 1);
        h.motion.receive(next, captured + 2);
        CHECK(h.motion.outgoing()); checkReply(*h.motion.outgoing(), next, consumed);
    });
}

// The existing queue owns a copied whole frame. Short driver writes below never
// call core.queued() again; source advancement belongs to acceptance, not drain.
struct OneTxQueue {
    Bytes bytes;
    size_t sent = 0;
    v4::Parser parser;
    bool accept(BoardExportTransfer& sender) {
        if (!bytes.empty() || !sender.outgoing()) return false;
        bytes = wire(*sender.outgoing()); sent = 0;
        sender.queued(); return true;
    }
    bool write(BoardExportTransfer& receiver, size_t limit, uint32_t now) {
        if (bytes.empty()) return false;
        const auto n = std::min(limit, bytes.size() - sent);
        bool delivered = false;
        for (size_t i = 0; i < n; ++i) {
            v4::Frame frame;
            if (parser.push(bytes[sent++], now, frame)) { receiver.receive(frame, now); delivered = true; }
        }
        if (sent == bytes.size()) { bytes.clear(); sent = 0; }
        return delivered;
    }
};

void timing() {
    for (uint32_t start : {captured, uint32_t(UINT32_MAX - 500)})
        scenario("lost reply: Brain times out, explicit new read completes before old five-second expiry", [=] {
            seed(context()); const auto disk = io.disk;
            TrackedSource source; Harness h(source); h.start(start);
            const auto firstQuery = *h.brain.outgoing(); h.brain.queued();
            h.motion.receive(throughCodec(firstQuery, start), start);
            CHECK(h.motion.outgoing()); const auto lostReply = *h.motion.outgoing();
            OneTxQueue tx; CHECK(tx.accept(h.motion));
            CHECK(source.consumed == lostReply.payload[3] && source.begins == 1);
            // Drop the queued frame without delivering even a byte to Brain.
            tx.bytes.clear();
            h.brain.poll(start + 999); CHECK(h.brain.state() == ExportTransferState::Pending);
            h.brain.poll(start + 1000); h.motion.poll(start + 1000);
            CHECK(h.brain.state() == ExportTransferState::TimedOut && !h.brain.outgoing() && !h.brain.snapshot());
            CHECK(source.remaining() > 0 && source.begins == 1);
            h.brain.poll(start + 1001); CHECK(!h.brain.outgoing());
            // A user action starts a new read. poll() never retries automatically.
            CHECK(h.brain.request(device, h.peer, nextChallenge, start + 1001));
            CHECK(h.brain.outgoing() && h.brain.outgoing()->messageId > firstQuery.messageId);
            ignoredReply(h.brain, lostReply, start + 1001);
            const auto raw = h.run(start + 1001, nextChallenge);
            CHECK(h.brain.state() == ExportTransferState::Complete && h.brain.snapshot());
            CHECK(source.begins == 2 && io.disk == disk);
            CHECK(!std::strcmp(h.brain.snapshot()->challenge, nextChallenge));
            CHECK(h.brain.snapshot()->capturedAtMs == uint32_t(start + 1001));
            CHECK(1001 + (raw.size() + 154) / 155 < BoardExportTransfer::kLifetimeMs);
        });
    scenario("explicit restart replaces unqueued outgoing, recaptures once, never consumes discarded reply", [] {
        seed(context()); TrackedSource source; Harness h(source); h.start();
        const auto oldQuery = *h.brain.outgoing(); h.brain.queued(); h.motion.receive(oldQuery, captured);
        CHECK(h.motion.outgoing()); const auto oldReply = *h.motion.outgoing();
        CHECK(source.consumes == 0 && source.begins == 1);
        const auto cancels = source.cancels;
        h.brain.poll(captured + 1000); CHECK(h.brain.state() == ExportTransferState::TimedOut && !h.brain.outgoing());
        CHECK(h.brain.request(device, h.peer, nextChallenge, captured + 1001));
        const auto freshQuery = *h.brain.outgoing(); h.brain.queued();
        io.disk["productctx"]["payload"] = stringValue(legacyJson(context(true)));
        h.motion.receive(throughCodec(freshQuery), captured + 1001);
        CHECK(source.begins == 2 && source.cancels == cancels + 1 && source.consumes == 0);
        CHECK(h.motion.outgoing()); const auto freshReply = *h.motion.outgoing();
        checkReply(freshReply, freshQuery, 0);
        ignoredReply(h.brain, oldReply, captured + 1002);
        h.motion.queued(); CHECK(source.consumes == 1 && source.consumed == freshReply.payload[3]);
        h.brain.receive(throughCodec(freshReply), captured + 1002);
        h.run(captured + 1003, nextChallenge,
              std::string(reinterpret_cast<const char*>(freshReply.payload + 5), freshReply.payload[3]));
        CHECK(h.brain.state() == ExportTransferState::Complete && h.brain.snapshot());
        CHECK(h.brain.snapshot()->legacy.cleared && source.begins == 2);
    });
    scenario("old queued short-write bytes remain immutable across explicit restart and are ignored by Brain", [] {
        seed(context()); TrackedSource source; Harness h(source); h.start();
        const auto oldQuery = *h.brain.outgoing(); h.brain.queued(); h.motion.receive(oldQuery, captured);
        CHECK(h.motion.outgoing()); OneTxQueue tx; CHECK(tx.accept(h.motion));
        const auto oldBytes = tx.bytes;
        CHECK(!tx.write(h.brain, 7, captured) && source.consumes == 1);
        const auto oldSent = tx.sent;
        h.brain.poll(captured + 1000); CHECK(h.brain.state() == ExportTransferState::TimedOut && !h.brain.outgoing());
        CHECK(h.brain.request(device, h.peer, nextChallenge, captured + 1001));
        const auto freshQuery = *h.brain.outgoing(); h.brain.queued();
        h.motion.receive(freshQuery, captured + 1001);
        CHECK(tx.bytes == oldBytes && tx.sent == oldSent && source.begins == 2 && source.consumes == 1);
        CHECK(h.motion.outgoing()); const auto freshReply = *h.motion.outgoing();
        CHECK(!tx.accept(h.motion) && source.consumes == 1);
        // After the 1s silence the parser may discard the old partial prefix;
        // either way these old-ID bytes must not expose or advance a snapshot.
        for (unsigned writes = 0; !tx.bytes.empty(); ++writes) {
            CHECK(writes < 200); tx.write(h.brain, 3, captured + 1002);
            CHECK(h.brain.state() == ExportTransferState::Pending && !h.brain.snapshot() && !h.brain.outgoing());
            CHECK(source.consumes == 1 && sameFrame(*h.motion.outgoing(), freshReply));
        }
        CHECK(tx.accept(h.motion) && source.consumes == 2);
        for (unsigned writes = 0; !tx.bytes.empty(); ++writes) {
            CHECK(writes < 200); tx.write(h.brain, 3, captured + 1002);
        }
        CHECK(h.brain.outgoing()); checkQuery(*h.brain.outgoing(), freshReply.payload[3], device, nextChallenge);
        h.run(captured + 1003, nextChallenge,
              std::string(reinterpret_cast<const char*>(freshReply.payload + 5), freshReply.payload[3]));
        CHECK(h.brain.state() == ExportTransferState::Complete && source.begins == 2);
    });
    for (unsigned foreign = 0; foreign < 5; ++foreign)
        scenario("foreign owner, old ID or same challenge cannot restart pending outgoing", [=] {
            seed(context()); TrackedSource source; Harness h(source);
            const auto first = query(0, 101); h.motion.receive(first, captured);
            CHECK(h.motion.outgoing()); const auto oldReply = *h.motion.outgoing();
            auto restart = query(0, 102, device, nextChallenge);
            if (foreign == 0) restart.senderBoot ^= 1;
            if (foreign == 1) restart.payload[2 + std::strlen(device)] = '9';
            if (foreign == 2) restart.payload[2] = '9';
            if (foreign == 3) restart.messageId = 101;
            if (foreign == 4) restart = query(0, 102);
            const auto remaining = source.remaining(), calls = io.calls.size();
            const auto cancels = source.cancels;
            h.motion.receive(restart, captured + 1001);
            CHECK(source.begins == 1 && source.cancels == cancels && source.consumes == 0);
            CHECK(source.remaining() == remaining && io.calls.size() == calls);
            CHECK(h.motion.outgoing() && sameFrame(*h.motion.outgoing(), oldReply));
            h.motion.queued(); CHECK(source.consumed == oldReply.payload[3]);
            const auto good = query(uint16_t(source.consumed), 102);
            h.motion.receive(good, captured + 1002);
            CHECK(h.motion.outgoing()); checkReply(*h.motion.outgoing(), good, source.consumed);
            CHECK(source.begins == 1);
        });
    scenario("rejected high-ID error does not poison same-owner watermark", [] {
        TrackedSource source; Harness h(source); const auto first = query();
        h.motion.receive(first, captured); CHECK(h.motion.outgoing());
        h.motion.queued(); const auto position = source.consumed;
        h.motion.receive(query(0, UINT32_MAX), captured + 1); // Same challenge cannot restart.
        CHECK(h.motion.outgoing() && h.motion.outgoing()->payload[0] == 3);
        h.motion.queued(); CHECK(source.consumed == position && source.begins == 1);
        const auto next = query(uint16_t(position), 102);
        h.motion.receive(next, captured + 2);
        CHECK(h.motion.outgoing()); checkReply(*h.motion.outgoing(), next, position);
    });
    for (bool finished : {false, true})
        scenario("same-owner watermark survives completion/expiry, new owner may begin at ID one", [=] {
            TrackedSource source; Harness h(source); uint32_t oldId = 101;
            if (finished) {
                h.start(); const auto raw = h.run(); oldId = uint32_t((raw.size() + 154) / 155);
            } else {
                h.motion.receive(query(0, oldId), captured);
                h.motion.poll(captured + 5000);
            }
            const auto begins = source.begins, consumes = source.consumes;
            const uint32_t now = captured + 5001;
            h.motion.receive(query(0, oldId, device, nextChallenge), now);
            CHECK(source.begins == begins && source.consumes == consumes);
            if (h.motion.outgoing()) { CHECK(h.motion.outgoing()->payload[0] == 3); h.motion.queued(); }
            auto newOwner = query(0, 1, device, nextChallenge); newOwner.senderBoot ^= 1;
            h.motion.receive(newOwner, now + 1);
            CHECK(source.begins == begins + 1 && h.motion.outgoing());
            CHECK(h.motion.outgoing()->payload[0] == 2 && h.motion.outgoing()->messageId == 1);
            CHECK(h.motion.outgoing()->receiverBoot == newOwner.senderBoot);
        });
    scenario("queue full and zero/short TX preserve reply until accepted; consume exactly once", [] {
        seed(context()); TrackedSource source; Harness h(source); h.start();
        const auto q = *h.brain.outgoing(); h.brain.queued();
        h.motion.receive(q, captured); CHECK(h.motion.outgoing()); const auto r = *h.motion.outgoing();
        OneTxQueue tx; tx.bytes = {0};
        CHECK(!tx.accept(h.motion) && source.consumes == 0 && sameFrame(*h.motion.outgoing(), r));
        h.motion.poll(captured + 1); CHECK(source.consumes == 0 && sameFrame(*h.motion.outgoing(), r));
        tx.bytes.clear(); CHECK(tx.accept(h.motion));
        CHECK(source.consumes == 1 && source.consumed == r.payload[3] && !h.motion.outgoing());
        CHECK(!tx.write(h.brain, 0, captured + 1));
        for (unsigned writes = 0; !tx.bytes.empty(); ++writes) {
            CHECK(writes < 200);
            const bool done = tx.write(h.brain, writes % 3 ? 3 : 1, captured + 2);
            if (!done) CHECK(!h.brain.outgoing() && !h.brain.snapshot());
            CHECK(source.consumes == 1);
        }
        CHECK(h.brain.outgoing()); checkQuery(*h.brain.outgoing(), r.payload[3]);
        h.motion.queued(); CHECK(source.consumes == 1);
    });
    for (uint32_t start : {uint32_t(0), uint32_t(UINT32_MAX - 500)})
        scenario("chunk timeout exact boundary, wrapsafe, late callbacks/replies and explicit retry", [=] {
            TrackedSource source; Harness h(source); h.start(start);
            const auto old = *h.brain.outgoing();
            h.brain.poll(start + 999); CHECK(h.brain.state() == ExportTransferState::Pending);
            h.brain.poll(start + 1000);
            CHECK(h.brain.state() == ExportTransferState::TimedOut && !h.brain.outgoing() && !h.brain.snapshot());
            h.brain.queued(); h.brain.receive(reply(old, "x", 0, false), start + 1000);
            h.brain.poll(start + 1001); CHECK(h.brain.state() == ExportTransferState::TimedOut && !h.brain.outgoing());
            CHECK(h.brain.request(device, h.peer, nextChallenge, start + 1001));
            const auto fresh = *h.brain.outgoing(); CHECK(fresh.messageId != old.messageId);
            ignoredReply(h.brain, reply(old, "x", 0, false), start + 1002);
            checkQuery(*h.brain.outgoing(), 0, device, nextChallenge);
        });
    scenario("reply arriving exactly at chunk expiry cannot revive transfer without poll", [] {
        RawSource source(capture()); Harness h(source); h.start();
        const auto q = *h.brain.outgoing(); h.brain.queued();
        h.brain.receive(reply(q, source.text.substr(0, 155), 0, false), captured + 1000);
        CHECK(h.brain.state() == ExportTransferState::TimedOut && !h.brain.outgoing() && !h.brain.snapshot());
    });
    for (uint32_t start : {uint32_t(100), uint32_t(UINT32_MAX - 2000)})
        scenario("whole transfer expires at five seconds despite fresh chunks", [=] {
            RawSource source(capture()); Harness h(source); h.start(start);
            for (unsigned i = 0; i < 5; ++i) {
                CHECK(h.brain.outgoing()); const auto q = *h.brain.outgoing(); h.brain.queued();
                const uint32_t now = start + (i + 1) * 900;
                h.brain.receive(reply(q, "x", uint16_t(i), false), now);
                CHECK(h.brain.state() == ExportTransferState::Pending);
            }
            h.brain.poll(start + 4999); CHECK(h.brain.state() == ExportTransferState::Pending);
            h.brain.poll(start + 5000);
            CHECK(h.brain.state() == ExportTransferState::TimedOut && !h.brain.snapshot() && !h.brain.outgoing());
        });
    scenario("Motion expiry cancels capture, late queue callback never consumes, explicit recapture works", [] {
        seed(context()); TrackedSource source; Harness h(source);
        const uint32_t start = UINT32_MAX - 100;
        h.motion.receive(query(), start); CHECK(h.motion.outgoing());
        h.motion.poll(start + 4999); CHECK(h.motion.outgoing() && source.consumes == 0);
        h.motion.poll(start + 5000); CHECK(!h.motion.outgoing() && source.remaining() == 0);
        const auto consumes = source.consumes, begins = source.begins;
        h.motion.queued(); CHECK(source.consumes == consumes);
        h.motion.receive(query(155, 102), start + 5001);
        CHECK(source.begins == begins && source.consumes == consumes);
        if (h.motion.outgoing()) h.motion.queued();
        h.motion.receive(query(0, 103, device, nextChallenge), start + 5002);
        CHECK(h.motion.outgoing() && source.begins == begins + 1);
        checkReply(*h.motion.outgoing(), query(0, 103, device, nextChallenge), 0);
    });
    scenario("reset cancels pending capture and Brain buffer; old frames cannot enter new pull", [] {
        seed(context()); TrackedSource source; Harness h(source); h.start();
        const auto old = *h.brain.outgoing(); h.brain.queued(); h.motion.receive(old, captured);
        CHECK(h.motion.outgoing()); const auto replyOld = *h.motion.outgoing();
        h.motion.reset(); h.brain.reset();
        CHECK(!h.motion.outgoing() && !h.brain.outgoing() && !h.brain.snapshot() && source.remaining() == 0);
        const auto consumed = source.consumed; h.motion.queued(); CHECK(source.consumed == consumed);
        CHECK(h.brain.begin(v4::Role::Brain, brainPhysical, brainBoot + 1));
        CHECK(h.brain.request(device, h.peer, nextChallenge, captured + 1));
        ignoredReply(h.brain, replyOld, captured + 2);
        CHECK(h.motion.begin(v4::Role::Motion, physical, motionBoot)); CHECK(h.motion.setSource(&source));
    });
    scenario("begin/request validation is local and busy Brain requests leave pending query intact", [] {
        BoardExportTransfer core;
        CHECK(core.state() == ExportTransferState::Idle && !core.outgoing() && !core.snapshot());
        auto peer = discover();
        CHECK(!core.request(device, peer, challenge, captured));
        for (const char* id : {static_cast<const char*>(nullptr), "", "000000000000", "012345ABCDEf", "bad"})
            CHECK(!core.begin(v4::Role::Brain, id, brainBoot));
        CHECK(!core.begin(v4::Role::Brain, brainPhysical, 0));
        CHECK(!core.begin(static_cast<v4::Role>(3), brainPhysical, brainBoot));
        CHECK(core.begin(v4::Role::Brain, brainPhysical, brainBoot));
        for (const char* id : {static_cast<const char*>(nullptr), "", "_bad", "bad/id"}) CHECK(!core.request(id, peer, challenge, captured));
        for (const char* token : {static_cast<const char*>(nullptr), "", "bad", "00000000000000000000000000000000", "0123456789ABCDEF0123456789abcdef"})
            CHECK(!core.request(device, peer, token, captured));
        for (auto state : {DiscoveryState::Idle, DiscoveryState::Pending, DiscoveryState::Conflict, DiscoveryState::Unavailable, DiscoveryState::TimedOut}) {
            auto p = peer; p.state = state; CHECK(!core.request(device, p, challenge, captured));
        }
        auto p = peer; p.peerBoot = 0; CHECK(!core.request(device, p, challenge, captured));
        p = peer; std::strcpy(p.physicalId, "000000000000"); CHECK(!core.request(device, p, challenge, captured));
        CHECK(io.calls.empty());
        CHECK(core.request(device, peer, challenge, captured)); const auto q = *core.outgoing();
        CHECK(!core.request("other-device", peer, nextChallenge, captured + 1)); CHECK(sameFrame(q, *core.outgoing()));
    });
}

void bounds() {
    scenario("Brain checked 16384+NUL allocation fails without issuing query or I/O", [] {
        RawSource source("unused"); Harness h(source);
        allocation_fault::kind = allocation_fault::Kind::Array;
        allocation_fault::size = kMotionExportWireMaxSize + 1;
        CHECK(!h.brain.request(device, h.peer, challenge, captured));
        CHECK(allocation_fault::hits == 1 && h.brain.state() == ExportTransferState::Idle);
        CHECK(!h.brain.outgoing() && !h.brain.snapshot() && io.calls.empty());
        h.start(); CHECK(h.brain.outgoing());
    });
    scenario("final snapshot allocation failure is unavailable with no partial exposure", [] {
        RawSource source(capture()); Harness h(source); h.start();
        size_t position = 0;
        while (position < source.text.size()) {
            CHECK(h.brain.outgoing()); const auto q = *h.brain.outgoing(); h.brain.queued();
            const auto n = std::min(size_t(155), source.text.size() - position);
            const bool done = position + n == source.text.size();
            if (done) {
                allocation_fault::kind = allocation_fault::Kind::Scalar;
                allocation_fault::size = sizeof(MotionExportSnapshot);
            }
            h.brain.receive(reply(q, source.text.substr(position, n), uint16_t(position), done), captured);
            position += n;
        }
        CHECK(allocation_fault::hits == 1 && h.brain.state() == ExportTransferState::Unavailable);
        CHECK(!h.brain.snapshot() && !h.brain.outgoing());
    });
    scenario("strict decoder allocation failure leaves previous snapshot byte-for-byte unchanged", [] {
        const auto raw = capture(); MotionExportSnapshot output;
        CHECK(decode(raw, output));
        std::array<uint8_t, sizeof(output)> before{}; std::memcpy(before.data(), &output, sizeof(output));
        allocation_fault::kind = allocation_fault::Kind::Scalar;
        allocation_fault::size = sizeof(MotionExportSnapshot);
        CHECK(!decode(raw, output)); CHECK(allocation_fault::hits == 1);
        CHECK(!std::memcmp(before.data(), &output, sizeof(output)));
    });
    for (size_t length : {size_t(16383), size_t(16384), size_t(16385)})
        scenario("Brain raw bound including checked final NUL", [=] {
            RawSource source("unused"); Harness h(source); h.start();
            size_t position = 0;
            while (position < length && h.brain.state() == ExportTransferState::Pending) {
                CHECK(h.brain.outgoing()); const auto q = *h.brain.outgoing(); checkQuery(q, position); h.brain.queued();
                const auto n = std::min(size_t(155), length - position);
                h.brain.receive(reply(q, std::string(n, 'x'), uint16_t(position), position + n == length), captured);
                position += n;
                if (position < length && position <= 16384) CHECK(h.brain.state() == ExportTransferState::Pending);
            }
            CHECK(position == length && h.brain.state() == ExportTransferState::Invalid);
            CHECK(!h.brain.snapshot() && !h.brain.outgoing());
        });
    scenario("exact 16KiB valid schema decodes and terminates safely", [] {
        seed(context()); auto raw = capture();
        const auto object = raw.find('{'); CHECK(object != std::string::npos && raw.size() < 16384);
        // JSON whitespace changes no schema/records and tests a valid full buffer.
        raw.insert(object + 1, 16384 - raw.size(), ' ');
        MotionExportSnapshot s; CHECK(decode(raw, s));
        RawSource source(raw); Harness h(source); h.start(); h.run();
        CHECK(h.brain.state() == ExportTransferState::Complete && h.brain.snapshot());
        CHECK(stateBytes(h.brain.snapshot()->state) == io.disk.at("productstate").at("record").bytes);
        CHECK(source.position == 16384);
    });
    scenario("final decoder rejects over-bound/null/zero and Motion refuses over-bound source", [] {
        seed(context()); auto raw = capture(); const auto object = raw.find('{');
        raw.insert(object + 1, 16385 - raw.size(), ' ');
        MotionExportSnapshot s; CHECK(!decode(raw, s));
        RawSource source(raw); Harness h(source); h.start();
        const auto q = *h.brain.outgoing(); h.brain.queued(); h.motion.receive(q, captured);
        CHECK(h.motion.outgoing()); const auto error = *h.motion.outgoing();
        CHECK(error.payload[0] == 3 && error.length == 1 && error.total == 1);
        h.motion.queued(); CHECK(source.consumes == 0);
    });
}
} // namespace

int main(int argc, char** argv) {
    const std::string group = argc > 1 ? argv[1] : "all";
    if (argc > 2 || (group != "all" && group != "records" && group != "decoder" &&
                    group != "frames" && group != "timing" && group != "bounds")) {
        std::fprintf(stderr, "Unknown test group\n"); return 2;
    }
    if (group == "all" || group == "records") records();
    if (group == "all" || group == "decoder") decoder();
    if (group == "all" || group == "frames") frames();
    if (group == "all" || group == "timing") timing();
    if (group == "all" || group == "bounds") bounds();
    std::printf("BoardExportTransfer: %u scenarios, %u failures (%s)\n", scenarios, failures, group.c_str());
    return failures ? 1 : 0;
}
