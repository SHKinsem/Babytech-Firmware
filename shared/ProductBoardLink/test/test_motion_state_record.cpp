#include "MotionStateRecord.h"
#include "BoardPairingRecord.h"
#include "RecordBytes.h"
#include "FakeProductCrypto.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

using namespace babytech;
using namespace babytech::boardlink;
using Bytes = std::vector<uint8_t>;
static unsigned checks = 0;
static const char* groupName = "setup";
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    std::fprintf(stderr, "%s:%d [%s]: %s\n", __FILE__, __LINE__, groupName, #condition); \
    std::exit(1); } } while (false)

static_assert(kMotionStateMaxSize <= 4096, "Motion NVS budget");
static_assert(kMotionResultMaxSize == 419 && kMotionSlotMaxSize == 586,
              "Review schema-1 branch capacity changes");
static_assert(kMotionStateV1MaxSize == 1728 && kMotionStateMaxSize == 4073, "Review record capacity changes");
template<class T> Bytes snapshot(const T& value) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    return Bytes(bytes, bytes + sizeof(value));
}
template<size_t N> void fill(char (&value)[N], char c) {
    std::memset(value, c, N - 1); value[N - 1] = 0;
}
template<size_t N> void unicode(char (&value)[N]) {
    // Valid four-byte UTF-8 at the exact byte capacity, not a shortened UI label.
    static_assert((N - 1) % 4 == 0, "UTF-8 fixture capacity");
    for (size_t i = 0; i < N - 1; i += 4) std::memcpy(value + i, "\xf0\x9f\x8d\xbc", 4);
    value[N - 1] = 0;
}
v4::Pairing pairing() {
    v4::Pairing p; p.role = v4::Role::Motion;
    std::strcpy(p.deviceId, "dev-1");
    std::strcpy(p.epoch, "0123456789abcdef0123456789abcdef");
    std::strcpy(p.localPhysicalId, "abcdef123456");
    std::strcpy(p.peerPhysicalId, "123456789abc");
    return p;
}
ProductContext context() {
    ProductContext c; std::strcpy(c.deviceId, "dev-1"); c.profileVersion = 7;
    std::strcpy(c.babyId, "baby-1"); std::strcpy(c.babyName, "Mia");
    std::strcpy(c.formulaBrand, "Friso");
    c.waterMl = 180; c.temperatureC = 40; c.powderGPer100Ml = 13.1f;
    return c;
}
void localId(const v4::Pairing& p, ProductRequest& r) {
    auto brain = p; brain.role = v4::Role::Brain;
    std::strcpy(brain.localPhysicalId, p.peerPhysicalId);
    std::strcpy(brain.peerPhysicalId, p.localPhysicalId);
    CHECK(makeLocalCommandId(brain, r.sequence, r.commandId));
}
ProductRequest request(v4::Source source = v4::Source::CloudCommand,
                       ProductCommand command = ProductCommand::Prepare, uint64_t seq = 42) {
    ProductRequest r; r.source = source; r.command = command; r.sequence = seq;
    std::strcpy(r.deviceId, "dev-1"); std::strcpy(r.commandId, "cmd-A");
    if (source == v4::Source::LocalTouch) localId(pairing(), r);
    if (command == ProductCommand::Prepare) {
        std::strcpy(r.babyId, "baby-1"); r.profileVersion = 7;
        r.waterMl = 180; r.temperatureC = 40; r.powderGPer100Ml = 13.1f;
    }
    if (command == ProductCommand::SetTargetTemp) r.temperatureC = 40;
    CHECK(validProductRequest(r)); return r;
}
MotionResult result(const ProductRequest& r, bool accepted = true) {
    MotionResult value; value.kind = MotionResultKind::Ordinary; value.request = r;
    CHECK(requestDigest(r, value.digest)); value.accepted = accepted;
    std::strcpy(value.reason, !accepted ? "busy" :
        r.command == ProductCommand::ResetError ? "already_clear" : "accepted"); return value;
}
MotionState emptyState() { MotionState s; s.pairing = pairing(); return s; }
MotionState state(v4::Source source = v4::Source::CloudCommand,
                  ProductCommand command = ProductCommand::Prepare,
                  MotionSlotKind kind = MotionSlotKind::Intent) {
    auto s = emptyState(); CHECK(makeMotionContextBarrier(context(), s.context));
    const auto r = request(source, command);
    const bool accepted = command != ProductCommand::CheckFirmwareUpdate;
    if (source == v4::Source::CloudCommand) { s.cloudSequence = r.sequence; s.cloudResult = result(r, accepted); }
    else { s.localSequence = r.sequence; s.localResult = result(r, accepted); }
    if (kind != MotionSlotKind::Empty) {
        s.slot.kind = kind; s.slot.request = r; CHECK(requestDigest(r, s.slot.digest));
        std::strcpy(s.slot.executionId, "fedcba9876543210fedcba9876543210");
        if (command == ProductCommand::Prepare) {
            CHECK(makeProductEventId(s.pairing, source, r.sequence, s.slot.eventId));
            s.slot.targetPowderG = std::round(r.waterMl * r.powderGPer100Ml / 10.f) / 10.f;
        }
        if (kind == MotionSlotKind::Terminal) {
            s.slot.uptimeMs = UINT32_MAX; std::strcpy(s.slot.reason, "reboot_during_feed");
            std::strcpy(s.slot.errorCode, "motor_fault");
        }
    }
    return s;
}

// The outer layout is assembled separately from MotionStateRecord; only nested
// codecs/digests and the shared record envelope are production implementations.
struct Wire {
    Bytes bytes = Bytes(12, 0);
    std::map<std::string, size_t> offsets;
    void mark(const std::string& name) { offsets[name] = bytes.size(); }
    void integer(uint64_t value, size_t width) {
        for (size_t i = 0; i < width; ++i) bytes.push_back(uint8_t(value >> (8 * i)));
    }
    void raw(const uint8_t* value, size_t length) { bytes.insert(bytes.end(), value, value + length); }
    void text(const std::string& name, const char* value) {
        mark(name + ".length"); integer(std::strlen(value), 2); mark(name); raw(
            reinterpret_cast<const uint8_t*>(value), std::strlen(value));
    }
    void floating(const std::string& name, float value) {
        mark(name); uint32_t bits; std::memcpy(&bits, &value, 4); integer(bits, 4);
    }
    void req(const std::string& name, const ProductRequest& r, const uint8_t* digest) {
        uint8_t encoded[kRequestIdentityMaxSize];
        const size_t n = encodeRequestIdentity(r, encoded, sizeof(encoded)); CHECK(n);
        mark(name + ".length"); integer(n, 2); mark(name + ".request"); raw(encoded, n);
        mark(name + ".digest"); raw(digest, 32);
    }
    void res(const std::string& name, const MotionResult& r) {
        mark(name + ".kind"); integer(uint8_t(r.kind), 1);
        if (r.kind == MotionResultKind::Ordinary) req(name, r.request, r.digest);
        if (r.kind == MotionResultKind::CloudStop) {
            mark(name + ".sequence"); integer(r.stopSequence, 8);
            text(name + ".command", r.stopCommandId); text(name + ".execution", r.stopExecutionId);
        }
        if (r.kind != MotionResultKind::None) {
            mark(name + ".accepted"); integer(r.accepted, 1); text(name + ".reason", r.reason);
            mark(name + ".outcome"); integer(uint8_t(r.outcome), 1);
        }
    }
};
Wire wire(const MotionState& s) {
    Wire w; uint8_t pair[kPairingRecordMaxSize];
    const size_t n = encodePairingRecord(s.pairing, pair, sizeof(pair)); CHECK(n);
    w.mark("pair.length"); w.integer(n, 2); w.mark("pair"); w.raw(pair, n);
    w.mark("context.present"); w.integer(s.context.present, 1);
    if (s.context.present) {
        w.mark("context.version"); w.integer(s.context.profileVersion, 4);
        w.mark("context.cleared"); w.integer(s.context.cleared, 1);
        w.mark("context.digest"); w.raw(s.context.digest, 32);
        w.text("context.baby", s.context.babyId); w.floating("context.powder", s.context.powderGPer100Ml);
    }
    w.mark("cloud.sequence"); w.integer(s.cloudSequence, 8);
    w.mark("local.sequence"); w.integer(s.localSequence, 8);
    w.res("cloud", s.cloudResult); w.res("local", s.localResult);
    w.mark("slot.kind"); w.integer(uint8_t(s.slot.kind), 1);
    if (s.slot.kind != MotionSlotKind::Empty) {
        w.req("slot", s.slot.request, s.slot.digest);
        w.text("slot.execution", s.slot.executionId); w.text("slot.event", s.slot.eventId);
        w.floating("slot.target", s.slot.targetPowderG);
        if (s.slot.kind == MotionSlotKind::Terminal) {
            w.mark("slot.completed"); w.integer(s.slot.completed, 1);
            w.mark("slot.uptime"); w.integer(s.slot.uptimeMs, 4);
            w.text("slot.reason", s.slot.reason); w.text("slot.error", s.slot.errorCode);
        }
    }
    CHECK(s.pendingResultCount == 0);  // Multi-result fixtures have a dedicated suite.
    w.integer(0, 1);
    detail::finishRecord(w.bytes.data(), w.bytes.size(), "BMS2"); return w;
}
void put(Bytes& bytes, size_t at, uint64_t value, size_t width) {
    CHECK(at + width <= bytes.size());
    for (size_t i = 0; i < width; ++i) bytes[at + i] = uint8_t(value >> (8 * i));
}
void repair(Bytes& bytes) { put(bytes, 8, detail::recordCrc(bytes.data(), bytes.size()), 4); }
Bytes encode(const MotionState& s) {
    Bytes bytes(kMotionStateMaxSize, 0xa5);
    const size_t n = encodeMotionState(s, bytes.data(), bytes.size());
    CHECK(n && n <= kMotionStateMaxSize); bytes.resize(n); return bytes;
}
void rejectDecode(const Bytes& bytes) {
    // The destination is deliberately populated and unlike most input fixtures.
    auto out = state(v4::Source::LocalTouch, ProductCommand::Prepare, MotionSlotKind::Terminal);
    const auto before = snapshot(out);
    CHECK(!decodeMotionState(bytes.data(), bytes.size(), out)); CHECK(snapshot(out) == before);
}
void rejectEncode(const MotionState& s, size_t capacity = kMotionStateMaxSize) {
    Bytes out(kMotionStateMaxSize + 16, 0xa5); const auto before = out;
    CHECK(!encodeMotionState(s, out.data(), capacity)); CHECK(out == before);
}
void invalid(const MotionState& s, bool serialized = true) {
    CHECK(!validMotionState(s)); rejectEncode(s);
    if (serialized) rejectDecode(wire(s).bytes);
}
void roundTrip(const MotionState& s, bool exhaustive = false) {
    CHECK(validMotionState(s)); const auto bytes = encode(s); CHECK(bytes == wire(s).bytes);
    auto out = state(v4::Source::LocalTouch, ProductCommand::Clean);
    CHECK(decodeMotionState(bytes.data(), bytes.size(), out)); CHECK(sameMotionState(s, out));
    CHECK(encode(out) == bytes);
    Bytes exact(bytes.size() + 16, 0xa5);
    CHECK(encodeMotionState(s, exact.data(), bytes.size()) == bytes.size());
    CHECK(std::equal(bytes.begin(), bytes.end(), exact.begin()));
    CHECK(std::all_of(exact.begin() + bytes.size(), exact.end(), [](uint8_t v) { return v == 0xa5; }));
    CHECK(!encodeMotionState(s, nullptr, kMotionStateMaxSize));
    const auto before = snapshot(out);
    CHECK(!decodeMotionState(nullptr, bytes.size(), out)); CHECK(snapshot(out) == before);
    auto extra = bytes; extra.push_back(0); rejectDecode(extra);
    detail::finishRecord(extra.data(), extra.size(), "BMS2"); rejectDecode(extra);
    extra.resize(kMotionStateMaxSize + 1); rejectDecode(extra);
    if (!exhaustive) return;
    for (size_t n = 0; n < bytes.size(); ++n) {
        rejectEncode(s, n); rejectDecode(Bytes(bytes.begin(), bytes.begin() + n));
        for (unsigned bit = 0; bit < 8; ++bit) {
            auto corrupt = bytes; corrupt[n] ^= uint8_t(1u << bit); rejectDecode(corrupt);
        }
    }
}

Bytes unhex(const char* hex) {
    Bytes bytes;
    while (*hex) {
        unsigned value = 0; CHECK(std::sscanf(hex, "%2x", &value) == 1);
        bytes.push_back(uint8_t(value)); hex += 2;
    }
    return bytes;
}
void golden() {
    // Independently generated with Python struct.pack, hashlib.sha256 and zlib:
    // envelope = magic + <HH(schema,payload size)> + CRC32(header[0:8]+payload).
    const auto empty = unhex(
        "424d5331010061004190b9ca4b004254503101003f0089b0ab91020530313233343536373839616263646566"
        "303132333435363738396162636465666162636465663132333435363132333435363738396162636465762d31"
        "0000000000000000000000000000000000000000");
    CHECK(empty.size() == 109);
    auto emptyV2 = unhex("424d533201006200e112df54");
    emptyV2.insert(emptyV2.end(), empty.begin() + 12, empty.end());
    emptyV2.push_back(0);
    CHECK(encode(emptyState()) == emptyV2);
    const auto full = unhex(
        "424d533101008b01a004eb214b004254503101003f0089b0ab91020530313233343536373839616263646566"
        "303132333435363738396162636465666162636465663132333435363132333435363738396162636465762d31"
        "0107000000001ddbcc3f5b0d76f63dc1379471c30f9f30cd736586757cfc617c688436170be1"
        "0600626162792d319a9951412a000000000000000000000000000000012c00"
        "0101022a0000000000000005006465762d310500636d642d410600626162792d3107000000b400289a995141"
        "3b066eda8fb23571235b64e708155275db41df1f4a360b0a008bb6c6ae87cdb9"
        "01080061636365707465640000012c00"
        "0101022a0000000000000005006465762d310500636d642d410600626162792d3107000000b400289a995141"
        "3b066eda8fb23571235b64e708155275db41df1f4a360b0a008bb6c6ae87cdb9"
        "20006665646362613938373635343332313066656463626139383736353433323130"
        "29006576742d30313233343536373839616263646566303132333435363738396162636465662d632d3432cdccbc41");
    CHECK(full.size() == 407);
    auto fullV2 = unhex("424d533201008c01c9d2afbe");
    fullV2.insert(fullV2.end(), full.begin() + 12, full.end());
    fullV2.push_back(0);
    CHECK(encode(state()) == fullV2);
    auto output = emptyState(); CHECK(decodeMotionState(full.data(), full.size(), output));
    CHECK(sameMotionState(state(), output));
    CHECK(decodeMotionState(empty.data(), empty.size(), output)); CHECK(sameMotionState(emptyState(), output));
}

void helpers() {
    MotionContextBarrier barrier; CHECK(makeMotionContextBarrier(context(), barrier));
    CHECK(barrier.present && !barrier.cleared && barrier.profileVersion == 7);
    CHECK(!std::strcmp(barrier.babyId, "baby-1") && barrier.powderGPer100Ml == 13.1f);
    uint8_t digest[32]; CHECK(contextDigest(context(), digest));
    CHECK(!std::memcmp(digest, barrier.digest, 32));
    CHECK(sameMotionContextBarrier(barrier, barrier));
    auto c = context(); unicode(c.babyName); unicode(c.formulaBrand);
    MotionContextBarrier changed; CHECK(makeMotionContextBarrier(c, changed));
    CHECK(!sameMotionContextBarrier(barrier, changed));
    auto tombstone = ProductContext{}; std::strcpy(tombstone.deviceId, "dev-1");
    tombstone.profileVersion = INT32_MAX; tombstone.cleared = true;
    CHECK(makeMotionContextBarrier(tombstone, changed));
    CHECK(changed.present && changed.cleared && changed.profileVersion == INT32_MAX);
    CHECK(!changed.babyId[0] && changed.powderGPer100Ml == 0);
    CHECK(contextDigest(tombstone, digest)); CHECK(!std::memcmp(digest, changed.digest, 32));
    auto s = emptyState(); s.context = changed; roundTrip(s);
    for (unsigned field = 0; field < 6; ++field) {
        c = context();
        if (field == 0) c.profileVersion = 0;
        if (field == 1) c.profileVersion = uint32_t(INT32_MAX) + 1;
        if (field == 2) c.cleared = true;
        if (field == 3) c.powderGPer100Ml = std::numeric_limits<float>::quiet_NaN();
        if (field == 4) c.deviceId[0] = '-';
        if (field == 5) std::memset(c.babyId, 'x', sizeof(c.babyId));
        const auto before = snapshot(barrier);
        CHECK(!makeMotionContextBarrier(c, barrier)); CHECK(snapshot(barrier) == before);
    }
    char id[59]{};
    for (auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch}) {
        for (uint64_t seq : {uint64_t(1), uint64_t(INT64_MAX)}) {
            CHECK(makeProductEventId(pairing(), source, seq, id));
            CHECK(std::string(id) == std::string("evt-") + pairing().epoch +
                (source == v4::Source::CloudCommand ? "-c-" : "-l-") + std::to_string(seq));
        }
    }
    CHECK(std::strlen(id) == 58);
    for (uint64_t seq : {uint64_t(0), uint64_t(INT64_MAX) + 1, UINT64_MAX}) {
        const auto before = snapshot(id);
        CHECK(!makeProductEventId(pairing(), v4::Source::CloudCommand, seq, id)); CHECK(snapshot(id) == before);
    }
    for (unsigned source = 0; source < 256; ++source) {
        if (source == 1 || source == 2) continue;
        const auto before = snapshot(id);
        CHECK(!makeProductEventId(pairing(), v4::Source(source), 1, id)); CHECK(snapshot(id) == before);
    }
    auto p = pairing(); p.epoch[0] = 'G'; const auto before = snapshot(id);
    CHECK(!makeProductEventId(p, v4::Source::LocalTouch, 1, id)); CHECK(snapshot(id) == before);
    char execution[33]; std::strcpy(execution, "0123456789abcdef0123456789abcdef");
    CHECK(validExecutionId(execution));
    for (const char* bad : {"", "0", "00000000000000000000000000000000",
                           "0123456789ABCDEF0123456789abcdef", "g123456789abcdef0123456789abcdef"}) {
        std::strcpy(execution, bad); CHECK(!validExecutionId(execution));
    }
    std::memset(execution, 'a', sizeof(execution)); CHECK(!validExecutionId(execution));
}

void records() {
    roundTrip(emptyState(), true);
    for (auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch}) {
        for (auto kind : {MotionSlotKind::Empty, MotionSlotKind::Intent, MotionSlotKind::Terminal})
            roundTrip(state(source, ProductCommand::Prepare, kind), true);
        auto complete = state(source, ProductCommand::Prepare, MotionSlotKind::Terminal);
        complete.slot.completed = true; complete.slot.reason[0] = 0; complete.slot.errorCode[0] = 0;
        complete.slot.uptimeMs = 0; roundTrip(complete, true);
        auto failed = state(source, ProductCommand::Prepare, MotionSlotKind::Terminal);
        failed.slot.errorCode[0] = 0; roundTrip(failed);
    }
    for (auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch}) {
        for (auto command : {ProductCommand::Initialize, ProductCommand::Clean,
                            ProductCommand::SetTargetTemp, ProductCommand::ResetError,
                            ProductCommand::CheckFirmwareUpdate}) {
            if (command == ProductCommand::Initialize && source == v4::Source::CloudCommand) continue;
            auto s = state(source, command, MotionSlotKind::Empty); s.context = {};
            if (command == ProductCommand::Initialize || command == ProductCommand::Clean)
                (source == v4::Source::CloudCommand ? s.cloudResult : s.localResult).outcome = MotionOutcome::Succeeded;
            roundTrip(s);
            if (command != ProductCommand::Initialize && command != ProductCommand::Clean) continue;
            s = state(source, command); s.context = {}; roundTrip(s, true);
            s.slot = {};
            auto& r = source == v4::Source::CloudCommand ? s.cloudResult : s.localResult;
            for (auto outcome : {MotionOutcome::Succeeded, MotionOutcome::Interrupted, MotionOutcome::Failed}) {
                r.outcome = outcome; roundTrip(s);
            }
        }
    }
    auto s = state(); s.localSequence = 43;
    s.localResult = result(request(v4::Source::LocalTouch, ProductCommand::Clean, 43), false);
    roundTrip(s, true);
    CHECK(!sameMotionState(s, state()));
}

void requiredIntent() {
    for (auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch}) {
        for (auto command : {ProductCommand::Initialize, ProductCommand::Clean}) {
            if (command == ProductCommand::Initialize && source == v4::Source::CloudCommand) continue;
            const auto original = state(source, command); roundTrip(original);
            auto s = original; s.slot = {}; invalid(s);
            s = original; s.slot.kind = MotionSlotKind::Terminal; s.slot.completed = true; invalid(s);
            // A structurally valid older intent is not the current accepted intent.
            s = original; --s.slot.request.sequence;
            if (source == v4::Source::LocalTouch) localId(s.pairing, s.slot.request);
            CHECK(requestDigest(s.slot.request, s.slot.digest)); invalid(s);
            s = original; std::strcpy(s.slot.request.commandId, "wrong-intent");
            CHECK(requestDigest(s.slot.request, s.slot.digest)); invalid(s);
            const auto other = source == v4::Source::CloudCommand ? v4::Source::LocalTouch : v4::Source::CloudCommand;
            s = state(other); // Valid prepare slot/result for the other source.
            if (source == v4::Source::CloudCommand) {
                s.cloudSequence = original.cloudSequence; s.cloudResult = original.cloudResult;
            } else {
                s.localSequence = original.localSequence; s.localResult = original.localResult;
            }
            invalid(s);
            s.slot.kind = MotionSlotKind::Terminal; s.slot.completed = true; invalid(s);
            // Repaired CRC must not let a completed, cleared result become an
            // accepted-but-unaccounted-for action by changing only its outcome.
            for (auto outcome : {MotionOutcome::Succeeded, MotionOutcome::Interrupted, MotionOutcome::Failed}) {
                s = original; s.slot = {};
                auto& recent = source == v4::Source::CloudCommand ? s.cloudResult : s.localResult;
                recent.outcome = outcome; roundTrip(s);
                auto w = wire(s);
                w.bytes[w.offsets.at(source == v4::Source::CloudCommand ? "cloud.outcome" : "local.outcome")] = 0;
                repair(w.bytes); rejectDecode(w.bytes);
            }
        }
    }
}

void frozen() {
    const auto original = state(); const auto frozenBytes = snapshot(original.slot);
    CHECK(productTargetPowderG(original.slot.request) == 23.6f);
    CHECK(original.slot.targetPowderG == 23.6f);
    auto raw = original; raw.slot.targetPowderG = 180.f * 13.1f / 100.f; invalid(raw);
    auto s = original;
    auto newer = context(); ++newer.profileVersion; std::strcpy(newer.babyId, "new-baby");
    newer.powderGPer100Ml = 20; CHECK(makeMotionContextBarrier(newer, s.context));
    roundTrip(s); CHECK(snapshot(s.slot) == frozenBytes);
    ProductContext cleared; std::strcpy(cleared.deviceId, "dev-1");
    cleared.profileVersion = 8; cleared.cleared = true;
    CHECK(makeMotionContextBarrier(cleared, s.context)); roundTrip(s);
    CHECK(snapshot(s.slot) == frozenBytes);
    for (auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch}) {
        s = state(source); const auto slot = snapshot(s.slot);
        auto& watermark = source == v4::Source::CloudCommand ? s.cloudSequence : s.localSequence;
        auto& recent = source == v4::Source::CloudCommand ? s.cloudResult : s.localResult;
        ++watermark; recent = result(request(source, ProductCommand::Prepare, watermark), false);
        // A rejected newer request need not match the active context or old slot.
        recent.request.profileVersion = 99; std::strcpy(recent.request.babyId, "rejected-baby");
        CHECK(requestDigest(recent.request, recent.digest)); roundTrip(s, true);
        CHECK(snapshot(s.slot) == slot);
        s.slot.kind = MotionSlotKind::Terminal; s.slot.completed = true; roundTrip(s);
        s.slot = {}; roundTrip(s);
        CHECK(watermark == 43 && !recent.accepted && recent.request.profileVersion == 99);
    }
    s = original; s.slot = {}; s.context = {}; roundTrip(s);
    s = original; --s.context.profileVersion; invalid(s);
    s = original; s.context = {}; invalid(s);
    s = original; std::strcpy(s.context.babyId, "other"); invalid(s);
    s = original; s.context.powderGPer100Ml = std::nextafter(13.1f, 50.f); invalid(s);
    s = original; cleared.profileVersion = 7;
    CHECK(makeMotionContextBarrier(cleared, s.context)); invalid(s);
}

void limits() {
    struct Dose { uint16_t water; float ratio; float expected; };
    // Exact binary half steps plus nonintegral ratios prevent a raw /100 regression.
    const Dose doses[] = {{30, 1.f, .3f}, {500, 50.f, 250.f}, {180, 13.1f, 23.6f},
        {125, 13.f, 16.3f}, {31, 12.5f, 3.9f}, {180, 13.333f, 24.f}};
    for (const auto& dose : doses) {
        auto s = state(); auto& r = s.slot.request;
        r.waterMl = dose.water; r.powderGPer100Ml = dose.ratio;
        s.context.powderGPer100Ml = dose.ratio; s.cloudResult = result(r);
        CHECK(requestDigest(r, s.slot.digest)); s.slot.targetPowderG = dose.expected;
        CHECK(productTargetPowderG(r) == dose.expected); roundTrip(s);
        s.slot.targetPowderG = std::nextafter(dose.expected, 300.f); invalid(s);
    }
    for (unsigned field = 0; field < 10; ++field) {
        auto s = state(v4::Source::CloudCommand, ProductCommand::Prepare, MotionSlotKind::Terminal);
        if (field == 0) std::memset(s.context.babyId, 'b', sizeof(s.context.babyId));
        if (field == 1) std::memset(s.cloudResult.reason, 'r', sizeof(s.cloudResult.reason));
        if (field == 2) std::memset(s.slot.executionId, 'a', sizeof(s.slot.executionId));
        if (field == 3) std::memset(s.slot.eventId, 'e', sizeof(s.slot.eventId));
        if (field == 4) std::memset(s.slot.reason, 'r', sizeof(s.slot.reason));
        if (field == 5) std::memset(s.slot.errorCode, 'e', sizeof(s.slot.errorCode));
        if (field == 6) std::memset(s.slot.request.commandId, 'c', sizeof(s.slot.request.commandId));
        if (field == 7) std::memset(s.slot.request.babyId, 'b', sizeof(s.slot.request.babyId));
        if (field == 8) std::memset(s.cloudResult.request.commandId, 'c', sizeof(s.cloudResult.request.commandId));
        if (field == 9) std::memset(s.pairing.deviceId, 'd', sizeof(s.pairing.deviceId));
        invalid(s, false);
    }
    for (const char* malformed : {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82", "\x80"}) {
        auto s = state(); std::strcpy(s.context.babyId, malformed); invalid(s);
        s = state(); std::strcpy(s.cloudResult.request.commandId, malformed); invalid(s, false);
    }
    auto s = state(); std::memset(s.context.digest, 0, sizeof(s.context.digest)); invalid(s);
    ProductContext c; std::strcpy(c.deviceId, "dev-1"); c.profileVersion = 8; c.cleared = true;
    s = state(); CHECK(makeMotionContextBarrier(c, s.context));
    s.context.babyId[0] = 'b'; invalid(s);
    s.context.babyId[0] = 0; s.context.powderGPer100Ml = -0.f; invalid(s);
}

void semantics() {
    for (unsigned which = 0; which < 23; ++which) {
        auto s = state();
        if (which == 0) s.pairing.role = v4::Role::Brain;
        if (which == 1) s.cloudSequence = 0;
        if (which == 2) --s.cloudSequence;
        if (which == 3) ++s.cloudSequence;
        if (which == 4) s.cloudSequence = uint64_t(INT64_MAX) + 1;
        if (which == 5) s.localSequence = UINT64_MAX;
        if (which == 6) s.cloudResult = {};
        if (which == 7) s.cloudResult.digest[0] ^= 1;
        if (which == 8) s.slot.digest[0] ^= 1;
        if (which == 9) s.slot.targetPowderG = std::nextafter(s.slot.targetPowderG, 100.f);
        if (which == 10) s.slot.targetPowderG = std::numeric_limits<float>::quiet_NaN();
        if (which == 11) s.slot.targetPowderG = std::numeric_limits<float>::infinity();
        if (which == 12) s.slot.executionId[0] = 'A';
        if (which == 13) fill(s.slot.executionId, '0');
        if (which == 14) s.slot.eventId[37] = 'l';
        if (which == 15) s.slot.eventId[4] = '9';
        if (which == 16) s.slot.eventId[0] = 0;
        if (which == 17) s.context.profileVersion = 0;
        if (which == 18) s.context.profileVersion = uint32_t(INT32_MAX) + 1;
        if (which == 19) s.context.powderGPer100Ml = 0;
        if (which == 20) s.cloudResult.accepted = false;
        if (which == 21) {
            s.cloudResult = result(s.slot.request, false);
        }
        if (which == 22) {
            ++s.slot.request.sequence; CHECK(requestDigest(s.slot.request, s.slot.digest));
            CHECK(makeProductEventId(s.pairing, s.slot.request.source, s.slot.request.sequence, s.slot.eventId));
        }
        invalid(s);
    }
    for (bool slot : {false, true}) {
        for (unsigned field = 0; field < 5; ++field) {
            auto s = state(); auto& r = slot ? s.slot.request : s.cloudResult.request;
            auto& digest = slot ? s.slot.digest : s.cloudResult.digest;
            if (field == 0) std::strcpy(r.deviceId, "other");
            if (field == 1) r.source = v4::Source::LocalTouch;
            if (field == 2) std::strcpy(r.commandId, "other");
            if (field == 3) ++r.waterMl;
            if (field == 4) --r.sequence;
            CHECK(requestDigest(r, digest)); invalid(s);
        }
    }
    auto local = state(v4::Source::LocalTouch);
    std::strcpy(local.localResult.request.commandId, "local-wrong-42");
    CHECK(requestDigest(local.localResult.request, local.localResult.digest)); invalid(local);
    local = state(v4::Source::LocalTouch); local.pairing.epoch[0] = '9'; invalid(local);
    for (unsigned value = 0; value < 256; ++value) {
        if (value > 2) {
            auto s = state(); s.cloudResult.kind = MotionResultKind(value); invalid(s);
            s = state(); s.slot.kind = MotionSlotKind(value); invalid(s);
        }
        if (value > 3) {
            auto s = state(v4::Source::LocalTouch, ProductCommand::Clean, MotionSlotKind::Empty);
            s.localResult.outcome = MotionOutcome(value); invalid(s);
        }
    }
    for (const char* reason : {"", "bad-reason", "bad reason", "bad.reason", "bad\nreason", "\xc3\xa9"}) {
        auto s = state(); s.slot = {}; s.cloudResult.accepted = false;
        std::strcpy(s.cloudResult.reason, reason); invalid(s);
    }
    for (auto command : {ProductCommand::Prepare, ProductCommand::Clean,
                        ProductCommand::SetTargetTemp, ProductCommand::ResetError,
                        ProductCommand::CheckFirmwareUpdate}) {
        for (const char* reason : {"accepted", "already_idle", "already_clear"}) {
            auto s = state(v4::Source::CloudCommand, command, MotionSlotKind::Empty);
            s.cloudResult.accepted = true; std::strcpy(s.cloudResult.reason, reason);
            if (command == ProductCommand::Clean) s.cloudResult.outcome = MotionOutcome::Succeeded;
            const bool expected = command == ProductCommand::ResetError ? !std::strcmp(reason, "already_clear") :
                command != ProductCommand::CheckFirmwareUpdate && !std::strcmp(reason, "accepted");
            if (expected) roundTrip(s); else invalid(s);
            s.cloudResult.accepted = false; s.cloudResult.outcome = MotionOutcome::None; invalid(s);
        }
    }
    auto s = state(); s.slot = {}; std::strcpy(s.cloudResult.reason, "busy"); invalid(s);
    s.cloudResult.accepted = false; fill(s.cloudResult.reason, 'R'); roundTrip(s);
    s.cloudResult.reason[64] = 'R'; invalid(s, false);
    for (auto outcome : {MotionOutcome::Succeeded, MotionOutcome::Interrupted, MotionOutcome::Failed}) {
        s = state(); s.cloudResult.outcome = outcome; invalid(s);
        s.slot = {}; invalid(s); // Prepare never acquires an execution outcome.
        s = state(v4::Source::LocalTouch, ProductCommand::Clean);
        s.localResult.outcome = outcome; invalid(s); // Slot has not been cleared.
        s.slot = {}; s.localResult.accepted = false; std::strcpy(s.localResult.reason, "busy"); invalid(s);
    }
    s = state(v4::Source::LocalTouch, ProductCommand::Clean, MotionSlotKind::Terminal); invalid(s);
    s = state(v4::Source::CloudCommand, ProductCommand::SetTargetTemp); invalid(s);
    for (unsigned field = 0; field < 3; ++field) {
        s = state(v4::Source::CloudCommand, ProductCommand::Prepare, MotionSlotKind::Terminal);
        if (field == 0) s.slot.reason[0] = 0;
        if (field == 1) s.slot.completed = true;
        if (field == 2) { s.slot.completed = true; s.slot.reason[0] = 0; }
        invalid(s);
    }
}

void omitted() {
    // Fields not serialized in a discriminated branch may not hide live state.
    for (unsigned field = 0; field < 6; ++field) {
        auto s = emptyState();
        if (field == 0) s.context.profileVersion = 1;
        if (field == 1) s.context.cleared = true;
        if (field == 2) s.context.digest[0] = 1;
        if (field == 3) s.context.babyId[0] = 'b';
        if (field == 4) s.context.powderGPer100Ml = 1;
        if (field == 5) s.context.powderGPer100Ml = -0.f;
        invalid(s, false);
    }
    for (unsigned field = 0; field < 10; ++field) {
        auto s = emptyState(); auto& r = s.cloudResult;
        if (field == 0) r.request = request();
        if (field == 1) r.digest[0] = 1;
        if (field == 2) r.stopSequence = 1;
        if (field == 3) r.stopCommandId[0] = 'c';
        if (field == 4) r.stopExecutionId[0] = 'e';
        if (field == 5) r.accepted = true;
        if (field == 6) r.reason[0] = 'r';
        if (field == 7) r.outcome = MotionOutcome::Succeeded;
        if (field == 8) r.request.source = v4::Source::CloudCommand;
        if (field == 9) r.request.powderGPer100Ml = -0.f;
        invalid(s, false); s.localResult = r; s.cloudResult = {}; invalid(s, false);
    }
    for (unsigned field = 0; field < 11; ++field) {
        auto s = emptyState(); auto& slot = s.slot;
        if (field == 0) slot.request = request();
        if (field == 1) slot.digest[0] = 1;
        if (field == 2) slot.executionId[0] = 'e';
        if (field == 3) slot.eventId[0] = 'e';
        if (field == 4) slot.targetPowderG = 1;
        if (field == 5) slot.completed = true;
        if (field == 6) slot.uptimeMs = 1;
        if (field == 7) slot.reason[0] = 'r';
        if (field == 8) slot.errorCode[0] = 'e';
        if (field == 9) slot.request.source = v4::Source::CloudCommand;
        if (field == 10) slot.targetPowderG = -0.f;
        invalid(s, false);
    }
    for (unsigned field = 0; field < 7; ++field) {
        auto s = state();
        if (field == 0) s.cloudResult.stopSequence = 1;
        if (field == 1) s.cloudResult.stopCommandId[0] = 'c';
        if (field == 2) s.cloudResult.stopExecutionId[0] = 'e';
        if (field == 3) s.slot.completed = true;
        if (field == 4) s.slot.uptimeMs = 1;
        if (field == 5) s.slot.reason[0] = 'r';
        if (field == 6) s.slot.errorCode[0] = 'e';
        invalid(s, false);
    }
    for (unsigned field = 0; field < 3; ++field) {
        auto s = state(v4::Source::LocalTouch, ProductCommand::Clean);
        if (field == 0) s.slot.eventId[0] = 'e';
        if (field == 1) s.slot.targetPowderG = 1;
        if (field == 2) s.slot.targetPowderG = -0.f;
        invalid(s);
    }
}

MotionResult stop(uint64_t seq = 43) {
    MotionResult r; r.kind = MotionResultKind::CloudStop; r.stopSequence = seq;
    std::strcpy(r.stopCommandId, "stop-command");
    std::strcpy(r.stopExecutionId, "fedcba9876543210fedcba9876543210");
    r.accepted = true; std::strcpy(r.reason, "accepted"); return r;
}
void stops() {
    auto s = emptyState(); s.cloudSequence = 43; s.cloudResult = stop(); roundTrip(s, true);
    s.cloudResult.stopExecutionId[0] = 0; std::strcpy(s.cloudResult.reason, "already_idle");
    roundTrip(s, true); // Observed idle, never a wildcard active-execution target.
    std::strcpy(s.cloudResult.reason, "already_clear"); invalid(s);
    s = state(); s.cloudSequence = 43; s.cloudResult = stop(); roundTrip(s, true);
    s.slot.kind = MotionSlotKind::Terminal; s.slot.completed = true; roundTrip(s);
    s.cloudResult.accepted = false; std::strcpy(s.cloudResult.reason, "stale_execution"); roundTrip(s);
    s.slot = {}; s.cloudSequence = INT64_MAX; s.cloudResult.stopSequence = INT64_MAX; roundTrip(s);
    for (unsigned field = 0; field < 11; ++field) {
        s = emptyState(); s.cloudSequence = 43; s.cloudResult = stop();
        if (field == 0) s.cloudResult.stopSequence = 0;
        if (field == 1) s.cloudResult.stopSequence = 42;
        if (field == 2) s.cloudResult.stopSequence = UINT64_MAX;
        if (field == 3) s.cloudResult.stopCommandId[0] = 0;
        if (field == 4) std::strcpy(s.cloudResult.stopExecutionId, "short");
        if (field == 5) fill(s.cloudResult.stopExecutionId, '0');
        if (field == 6) s.cloudResult.stopExecutionId[0] = 'F';
        if (field == 7) s.cloudResult.outcome = MotionOutcome::Succeeded;
        if (field == 8) s.cloudResult.request = request();
        if (field == 9) s.cloudResult.digest[0] = 1;
        if (field == 10) {
            s.localSequence = 43; s.localResult = s.cloudResult; s.cloudResult = {}; s.cloudSequence = 0;
        }
        invalid(s, field != 8 && field != 9);
    }
    s = state(); s.cloudResult = stop(42); invalid(s); // Same watermark cannot replace slot's accepted request.
    for (unsigned field = 0; field < 2; ++field) {
        s = emptyState(); s.cloudSequence = 43; s.cloudResult = stop();
        if (field == 0) std::memset(s.cloudResult.stopCommandId, 'c', sizeof(s.cloudResult.stopCommandId));
        else std::memset(s.cloudResult.stopExecutionId, 'a', sizeof(s.cloudResult.stopExecutionId));
        invalid(s, false);
    }
    s = emptyState(); s.cloudSequence = 43; s.cloudResult = stop(); const auto w = wire(s);
    for (auto name : {"cloud.command.length", "cloud.execution.length"}) {
        auto bad = w.bytes; put(bad, w.offsets.at(name), 0xffff, 2); repair(bad); rejectDecode(bad);
    }
    for (auto name : {"cloud.command", "cloud.execution"}) {
        auto bad = w.bytes; bad[w.offsets.at(name)] = 0xff; repair(bad); rejectDecode(bad);
    }
}

void repairedWire() {
    const auto s = state(v4::Source::CloudCommand, ProductCommand::Prepare, MotionSlotKind::Terminal);
    const auto w = wire(s);
    const auto mutate = [&](const char* name, uint64_t value, size_t width = 1) {
        auto bytes = w.bytes; put(bytes, w.offsets.at(name), value, width); repair(bytes); rejectDecode(bytes);
    };
    for (auto name : {"context.present", "context.cleared", "cloud.accepted", "slot.completed"})
        for (unsigned value = 2; value < 256; ++value) mutate(name, value);
    for (auto name : {"pair.length", "context.baby.length", "cloud.length", "cloud.reason.length",
                      "slot.length", "slot.execution.length", "slot.event.length",
                      "slot.reason.length", "slot.error.length"}) {
        mutate(name, 0xffff, 2); mutate(name, 0, 2);
    }
    for (auto name : {"context.baby", "cloud.reason", "slot.execution", "slot.event", "slot.reason", "slot.error"})
        for (uint8_t value : {uint8_t(0), uint8_t(0xff), uint8_t(0xc0), uint8_t(0x80)}) mutate(name, value);
    mutate("cloud.digest", w.bytes[w.offsets.at("cloud.digest")] ^ 1);
    mutate("slot.digest", w.bytes[w.offsets.at("slot.digest")] ^ 1);
    mutate("context.version", 0, 4); mutate("context.version", UINT32_MAX, 4);
    mutate("context.powder", 0x7fc00000, 4); mutate("slot.target", 0x7f800000, 4);
    mutate("cloud.sequence", 0, 8); mutate("local.sequence", UINT64_MAX, 8);
    for (size_t offset : {size_t(0), size_t(4), size_t(5), size_t(6), size_t(7)}) {
        auto bytes = w.bytes; bytes[offset] ^= 1; repair(bytes); rejectDecode(bytes);
    }
    auto nested = w.bytes; const size_t pairAt = w.offsets.at("pair");
    nested[pairAt + 12] = uint8_t(v4::Role::Brain);
    repair(nested); rejectDecode(nested); // Valid outer CRC, invalid nested CRC.
    const size_t pairLength = w.offsets.at("context.present") - pairAt;
    put(nested, pairAt + 8, detail::recordCrc(nested.data() + pairAt, pairLength), 4);
    repair(nested); rejectDecode(nested); // Both CRCs valid, wrong role.
    for (auto name : {"cloud.request", "slot.request"}) {
        const size_t at = w.offsets.at(name);
        for (size_t field : {size_t(0), size_t(1), size_t(2)}) {
            auto bytes = w.bytes; bytes[at + field] = 255; repair(bytes); rejectDecode(bytes);
        }
    }
    // Repair header length and CRC at every truncation point, not just raw cuts.
    for (size_t n = 12; n < w.bytes.size(); ++n) {
        Bytes bytes(w.bytes.begin(), w.bytes.begin() + n);
        detail::finishRecord(bytes.data(), bytes.size(), "BMS2"); rejectDecode(bytes);
    }
}

void maximum() {
    auto s = state(v4::Source::CloudCommand, ProductCommand::Prepare, MotionSlotKind::Terminal);
    fill(s.pairing.deviceId, 'd');
    auto c = context(); std::strcpy(c.deviceId, s.pairing.deviceId);
    c.profileVersion = INT32_MAX; unicode(c.babyId); unicode(c.babyName); unicode(c.formulaBrand);
    c.waterMl = 500; c.temperatureC = 60; c.powderGPer100Ml = 50;
    CHECK(makeMotionContextBarrier(c, s.context));
    auto r = request(); std::strcpy(r.deviceId, c.deviceId); unicode(r.commandId);
    std::strcpy(r.babyId, c.babyId); r.profileVersion = c.profileVersion;
    r.waterMl = c.waterMl; r.temperatureC = c.temperatureC; r.powderGPer100Ml = c.powderGPer100Ml;
    r.sequence = INT64_MAX - 1; s.slot.request = r; CHECK(requestDigest(r, s.slot.digest));
    CHECK(makeProductEventId(s.pairing, r.source, r.sequence, s.slot.eventId));
    s.slot.targetPowderG = 250; fill(s.slot.reason, 'R'); fill(s.slot.errorCode, 'E');
    r.sequence = INT64_MAX; s.cloudSequence = r.sequence; s.cloudResult = result(r, false);
    fill(s.cloudResult.reason, 'R');
    r.source = v4::Source::LocalTouch; localId(s.pairing, r);
    s.localSequence = r.sequence; s.localResult = result(r, false); fill(s.localResult.reason, 'R');
    const auto bytes = encode(s);
    // Cloud request 316; local request 246 (58-byte generated ID, not 128).
    CHECK(bytes.size() == 1659);
    std::printf("Maximum legal record: %zu bytes (bound %zu, NVS cap 4096)\n", bytes.size(), kMotionStateMaxSize);
    roundTrip(s, true);
    s.slot = {}; s.cloudResult = stop(INT64_MAX); unicode(s.cloudResult.stopCommandId); roundTrip(s, true);
    for (uint64_t seq : {uint64_t(INT64_MAX) + 1, UINT64_MAX}) {
        s.cloudSequence = seq; invalid(s);
    }
}

void crypto() {
    auto s = state(); const auto bytes = encode(s);
    auto out = state(v4::Source::LocalTouch, ProductCommand::Clean);
    const auto before = snapshot(out);
    MotionContextBarrier barrier; CHECK(makeMotionContextBarrier(context(), barrier));
    const auto barrierBefore = snapshot(barrier);
    fake_product_crypto::reset(); fake_product_crypto::fail = true;
    CHECK(!makeMotionContextBarrier(context(), barrier)); CHECK(snapshot(barrier) == barrierBefore);
    CHECK(!validMotionState(s)); rejectEncode(s);
    CHECK(!decodeMotionState(bytes.data(), bytes.size(), out)); CHECK(snapshot(out) == before);
    CHECK(fake_product_crypto::calls >= 4);
    fake_product_crypto::reset();
    // Isolate each SHA-bearing branch, including slot verification after a Stop.
    for (unsigned branch = 0; branch < 3; ++branch) {
        s = state(branch == 1 ? v4::Source::LocalTouch : v4::Source::CloudCommand);
        if (branch < 2) s.slot = {};
        else { s.cloudSequence = 43; s.cloudResult = stop(); }
        const auto encoded = encode(s); const auto original = snapshot(out);
        fake_product_crypto::reset(); fake_product_crypto::fail = true;
        CHECK(!validMotionState(s)); rejectEncode(s);
        CHECK(!decodeMotionState(encoded.data(), encoded.size(), out)); CHECK(snapshot(out) == original);
        CHECK(fake_product_crypto::calls >= 3); fake_product_crypto::reset();
    }
    roundTrip(s);
}

int main(int argc, char** argv) {
    struct Group { const char* name; void (*run)(); };
    const Group groups[] = {{"golden", golden}, {"helpers", helpers}, {"records", records}, {"frozen", frozen},
        {"limits", limits}, {"required-intent", requiredIntent},
        {"semantics", semantics}, {"omitted", omitted}, {"stop", stops},
        {"repaired-wire", repairedWire}, {"maximum", maximum}, {"crypto", crypto}};
    CHECK(argc <= 2); bool selected = false;
    for (const auto& group : groups) {
        if (argc == 2 && std::strcmp(argv[1], group.name)) continue;
        selected = true; groupName = group.name; fake_product_crypto::reset(); group.run();
        std::printf("PASS %s\n", group.name);
    }
    CHECK(selected); std::printf("PASS MotionStateRecord: %u checks\n", checks);
}
