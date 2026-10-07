#include "brain_installer.h"
#include "brain_install_console.h"
#include "MotionInstallTarget.h"
#include "MaintenanceExport.h"
#include "FakeCommissioning.h"
#include "FakeProductCrypto.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace babytech::boardlink;
namespace v4 = babytech::v4;
namespace fake = fake_brain;
namespace extra = fake_commissioning;
using Stage = babytech::brain::BrainInstallStage;
using fake::io;
using fake::Op;

namespace {
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + \
    std::to_string(__LINE__) + ": " #x); } while (false)
constexpr char kBrain[] = "fedcba987654", kMotion[] = "012345abcdef";
constexpr char kDevice[] = "Babytech_01-test", kForeign[] = "a123456789ab";
constexpr char kEpoch[] = "0123456789abcdef0123456789abcdef";
constexpr char kOtherEpoch[] = "fedcba9876543210fedcba9876543210";
constexpr char kNonce[] = "123456789abcdef0123456789abcdef0";
constexpr char kFresh[] = "fedcba9876543210fedcba9876543210";
constexpr char kExecution[] = "a123456789abcdefa123456789abcdef";
constexpr uint64_t kBrainBoot = UINT64_C(0xfedcba9876543210);
constexpr uint64_t kMotionBoot = UINT64_C(0x0123456789abcdef);
constexpr std::array<uint8_t, 6> kBrainMac{{0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54}};
constexpr std::array<uint8_t, 6> kMotionMac{{0x01, 0x23, 0x45, 0xab, 0xcd, 0xef}};
unsigned scenarios = 0, failures = 0, wireFrames = 0, epochCalls = 0;
uint32_t currentNow = 100;
bool localMaintenance = true, stationary = true, epochAvailable = true;
std::vector<std::string> epochs;
std::function<void()> clockHook;
bool localSafe() { return localMaintenance; }
bool motionSafe() { return stationary; }
uint32_t nowMs() { if (clockHook) clockHook(); return currentNow; }
bool newEpoch(char (&output)[33]) {
    ++epochCalls;
    if (!epochAvailable) return false;
    // Exercise propagation of fresh entropy, not a fake constant success epoch.
    std::random_device random;
    for (unsigned i = 0; i < 4; ++i)
        std::snprintf(output + i * 8, 9, "%08x", static_cast<unsigned>(random()));
    epochs.emplace_back(output);
    return true;
}

v4::Pairing pair(bool brain, const char* epoch = kEpoch) {
    v4::Pairing p;
    p.role = brain ? v4::Role::Brain : v4::Role::Motion;
    std::strcpy(p.deviceId, kDevice);
    std::strcpy(p.epoch, epoch);
    std::strcpy(p.localPhysicalId, brain ? kBrain : kMotion);
    std::strcpy(p.peerPhysicalId, brain ? kMotion : kBrain);
    CHECK(v4::validPairing(p));
    return p;
}

std::string legacyJson(int kind) {
    const std::string prefix = std::string("{\"type\":\"feeding_context\",\"device_id\":\"") +
        kDevice + "\",\"profile_version\":2147483647";
    if (kind == 2) return prefix + ",\"cleared\":true}";
    std::string name, brand;
    for (unsigned i = 0; i < 106; ++i) name += "\xe5\xae\x9d";
    for (unsigned i = 0; i < 160; ++i) brand += "\xe5\xa5\xb6";
    return prefix + ",\"baby_id\":\"" + std::string(96, 'b') + "\",\"baby_name\":\"" + name +
        "AB\",\"formula_brand\":\"" + brand +
        "\",\"water_ml\":180,\"temp\":45,\"powder_g_per_100ml\":25.125}";
}

ProductContext context(int kind) {
    ProductContext c;
    const auto json = legacyJson(kind);
    CHECK(decodeProductContext(reinterpret_cast<const uint8_t*>(json.data()), json.size(), kDevice, c));
    return c;
}

fake::Value textValue(const std::string& value) {
    fake::Bytes bytes(value.begin(), value.end());
    bytes.push_back(0);
    return {bytes, fake::Type::String};
}

bool present(const fake::Database& db, const char* name) {
    const auto found = db.find(name);
    return found != db.end() && found->second.count("record");
}

struct Trace { bool motion; fake::Call call; };

// The shared fake SDK represents exactly one MCU at a time. Swap its entire
// state (disk, faults, call history), never just the identity or the record map.
class MotionScope {
public:
    explicit MotionScope(fake::State& other) : other_(other), previous_(extra::mac) {
        CHECK(io.handles.empty() && other_.handles.empty());
        std::swap(io, other_);
        extra::mac = kMotionMac;
    }
    ~MotionScope() noexcept(false) {
        CHECK(io.handles.empty() && other_.handles.empty());
        std::swap(io, other_);
        extra::mac = previous_;
    }
private:
    fake::State& other_;
    std::array<uint8_t, 6> previous_;
};

v4::Frame wire(const v4::Frame& frame) {
    std::array<uint8_t, v4::kMaxFrame> bytes{};
    const size_t size = v4::encode(frame, bytes.data(), bytes.size());
    CHECK(size);
    v4::Parser parser;
    v4::Frame parsed;
    unsigned completed = 0;
    for (size_t i = 0; i < size; ++i) if (parser.push(bytes[i], currentNow, parsed)) ++completed;
    CHECK(completed == 1 && parsed.kind == frame.kind && parsed.senderBoot == frame.senderBoot &&
          parsed.receiverBoot == frame.receiverBoot && parsed.messageId == frame.messageId &&
          parsed.offset == frame.offset && parsed.total == frame.total && parsed.length == frame.length);
    CHECK(!std::memcmp(parsed.payload, frame.payload, frame.length));
    ++wireFrames;
    return parsed;
}

void deliver(BoardInstall& receiver, const v4::Message& message) {
    for (size_t offset = 0; offset < message.length;) {
        v4::Frame frame;
        CHECK(v4::fragment(message, offset, frame));
        receiver.receive(wire(frame), currentNow);
        offset += frame.length;
    }
}

struct Evidence final : BoardMaintenanceTarget {
    bool safeToAcquire() const override { return stationary; }
    bool safeToRelease() const override { return stationary; }
};

struct FakeLink {
    fake::State& motionDb;
    BoardDiscovery discoveryBrain, discoveryMotion;
    BoardMaintenance leaseBrain, leaseMotion;
    Evidence evidence;
    v4::Assembler brainRx{}, motionRx{};
    v4::Message brainScratch{}, motionScratch{};
    BoardInstall installBrain, installMotion;
    MotionStateStore motionStore;
    MotionInstallTarget motionTarget;
    MaintenanceExport exporter;
    BoardExportTransfer recordsBrain, recordsMotion;
    unsigned discoveries = 0, reservations = 0, reads = 0, installs = 0, releases = 0;
    unsigned installedReads = 0;
    unsigned recoveryReads = 0;
    std::vector<CommissioningImport> submitted;
    std::vector<std::string> readChallenges;
    unsigned snapshotCaptures = 0, installPackets = 0;
    bool dropDiscovery = false, dropMaintenance = false, dropRecords = false;
    bool dropInstallRequest = false, dropInstallReply = false;
    bool refuseRecords = false, refuseInstall = false, nullSnapshot = false;
    std::function<void()> beforeCapture;
    std::function<void()> afterSnapshot;

    static DiscoveryPairState discoveryState(PairingLoad loaded) {
        return loaded == PairingLoad::Ready ? DiscoveryPairState::Ready :
            loaded == PairingLoad::Missing ? DiscoveryPairState::Missing :
            loaded == PairingLoad::Corrupt ? DiscoveryPairState::Corrupt :
            loaded == PairingLoad::IdentityMismatch ? DiscoveryPairState::IdentityMismatch : DiscoveryPairState::IoError;
    }

    explicit FakeLink(fake::State& db) : motionDb(db),
        motionTarget(motionStore, leaseMotion, motionSafe, nowMs) {
        CHECK(discoveryBrain.begin(v4::Role::Brain, kBrain, kBrainBoot, DiscoveryPairState::Missing));
        CHECK(discoveryMotion.begin(v4::Role::Motion, kMotion, kMotionBoot, DiscoveryPairState::Missing));
        CHECK(leaseBrain.begin(v4::Role::Brain, kBrain, kBrainBoot));
        CHECK(leaseMotion.begin(v4::Role::Motion, kMotion, kMotionBoot));
        CHECK(leaseMotion.setTarget(&evidence));
        CHECK(installBrain.bindReceiveBuffers(brainRx, brainScratch));
        CHECK(installMotion.bindReceiveBuffers(motionRx, motionScratch));
        CHECK(installBrain.begin(v4::Role::Brain, kBrain, kBrainBoot));
        CHECK(installMotion.begin(v4::Role::Motion, kMotion, kMotionBoot));
        CHECK(installMotion.setTarget(&motionTarget));
        CHECK(recordsBrain.begin(v4::Role::Brain, kBrain, kBrainBoot));
        CHECK(recordsMotion.begin(v4::Role::Motion, kMotion, kMotionBoot));
        CHECK(recordsMotion.setSource(&exporter));
    }
    bool requestDiscovery(const char* device, uint32_t now) {
        ++discoveries;
        if (discoveries > 1) return discoveryBrain.request(device, now);
        // Populate both real channel identities from their own boot-time NVS.
        // A newly persisted installation does NOT activate this running channel.
        v4::Pairing brainPair;
        const auto brainLoad = loadBoardPairing(v4::Role::Brain, brainPair);
        discoveryBrain = BoardDiscovery{};
        leaseBrain = BoardMaintenance{};
        CHECK(discoveryBrain.begin(v4::Role::Brain, kBrain, kBrainBoot, discoveryState(brainLoad),
                                   brainLoad == PairingLoad::Ready ? &brainPair : nullptr));
        CHECK(leaseBrain.begin(v4::Role::Brain, kBrain, kBrainBoot,
                               brainLoad == PairingLoad::Ready ? &brainPair : nullptr));
        {
            MotionScope scope(motionDb);
            v4::Pairing motionPair;
            const auto motionLoad = loadBoardPairing(v4::Role::Motion, motionPair);
            discoveryMotion = BoardDiscovery{};
            leaseMotion = BoardMaintenance{};
            CHECK(discoveryMotion.begin(v4::Role::Motion, kMotion, kMotionBoot, discoveryState(motionLoad),
                                        motionLoad == PairingLoad::Ready ? &motionPair : nullptr));
            CHECK(leaseMotion.begin(v4::Role::Motion, kMotion, kMotionBoot,
                                    motionLoad == PairingLoad::Ready ? &motionPair : nullptr));
            CHECK(leaseMotion.setTarget(&evidence));
        }
        return discoveryBrain.request(device, now);
    }
    const DiscoveryResult& discoveryResult() const { return discoveryBrain.result(); }
    bool requestMaintenance(const char* device, uint32_t now) {
        ++reservations;
        char nonce[33]{};
        if (reservations <= 2) std::strcpy(nonce, reservations == 1 ? kNonce : kFresh);
        else std::snprintf(nonce, sizeof(nonce), "%032x", reservations);
        return leaseBrain.request(device, discoveryResult(), nonce, now);
    }
    BoardMaintenanceState maintenanceState() const { return leaseBrain.state(); }
    const char* installationNonce() const { return leaseBrain.nonce(); }
    const char* installationPhysicalId() const { return installBrain.physicalId(); }
    bool installationLeaseValid(uint32_t now) {
        leaseBrain.poll(now);
        return leaseBrain.state() == BoardMaintenanceState::Active;
    }
    bool requestRecords(const char* device, uint32_t now) {
        ++reads;
        if (refuseRecords) return false;
        char challenge[33]{};
        nextChallenge(challenge);
        return recordsBrain.request(device, discoveryResult(), challenge, now);
    }
    bool requestInstalledRecords(const char* device, const v4::Pairing& expected, uint32_t now) {
        ++reads; ++installedReads;
        if (refuseRecords) return false;
        char challenge[33]{};
        nextChallenge(challenge);
        return installationLeaseValid(now) && installBrain.state() == BoardInstallState::Complete &&
            (installBrain.result() == CommissioningResult::Installed ||
             installBrain.result() == CommissioningResult::AlreadyInstalled) &&
            recordsBrain.requestInstalled(device, discoveryResult(), challenge, expected, now);
    }
    bool requestRecoveryRecords(const char* device, const v4::Pairing& expected, uint32_t now) {
        ++reads; ++recoveryReads;
        if (refuseRecords) return false;
        char challenge[33]{};
        nextChallenge(challenge);
        return installationLeaseValid(now) && installBrain.matchesRequestedPair(expected) &&
            recordsBrain.requestRecovery(device, discoveryResult(), challenge, expected, now);
    }
    void nextChallenge(char (&challenge)[33]) {
        if (reads <= 2) std::strcpy(challenge, reads == 1 ? kNonce : kFresh);
        else std::snprintf(challenge, sizeof(challenge), "%032x", reads);
        CHECK(std::find(readChallenges.begin(), readChallenges.end(), challenge) == readChallenges.end());
        readChallenges.emplace_back(challenge);
    }
    ExportTransferState recordsState() const { return recordsBrain.state(); }
    const MotionExportSnapshot* recordsSnapshot() const {
        return !nullSnapshot ? recordsBrain.snapshot() : nullptr;
    }
    bool requestInstallation(const CommissioningImport& request, uint32_t now) {
        ++installs;
        const bool requested = !refuseInstall && installationLeaseValid(now) &&
            installBrain.request(request, installationNonce(), discoveryResult().peerBoot, now);
        if (requested) submitted.push_back(request);
        return requested;
    }
    BoardInstallState installationState() const { return installBrain.state(); }
    CommissioningResult installationResult() const { return installBrain.result(); }
    bool releaseMaintenance(uint32_t now) { ++releases; return leaseBrain.release(now); }

    // Only transport/snapshot adaptation. No fake Installed response or fake
    // initial state: the Motion target invokes its actual commissioning/NVS.
    void pump() {
        CHECK(extra::mac == kBrainMac && io.handles.empty() && motionDb.handles.empty());
        discoveryBrain.poll(currentNow);
        leaseBrain.poll(currentNow);
        installBrain.poll(currentNow);
        recordsBrain.poll(currentNow);
        {
            MotionScope scope(motionDb);
            discoveryMotion.poll(currentNow);
            leaseMotion.poll(currentNow);
            installMotion.poll(currentNow);
            recordsMotion.poll(currentNow);
        }
        if (discoveryBrain.outgoing()) {
            const auto frame = wire(*discoveryBrain.outgoing());
            discoveryBrain.queued();
            if (!dropDiscovery) {
                MotionScope scope(motionDb);
                discoveryMotion.receive(frame, currentNow);
            }
        }
        if (discoveryMotion.outgoing()) {
            const auto frame = wire(*discoveryMotion.outgoing());
            discoveryMotion.queued();
            discoveryBrain.receive(frame, currentNow);
        }
        if (leaseBrain.outgoing()) {
            const auto frame = wire(*leaseBrain.outgoing());
            leaseBrain.queued();
            if (!dropMaintenance) {
                MotionScope scope(motionDb);
                leaseMotion.receive(frame, currentNow);
            }
        }
        if (leaseMotion.outgoing()) {
            const auto frame = wire(*leaseMotion.outgoing());
            leaseMotion.queued();
            if (!dropMaintenance) leaseBrain.receive(frame, currentNow);
        }
        if (installBrain.outgoing()) {
            const auto message = *installBrain.outgoing();
            installBrain.queued();
            ++installPackets;
            if (!dropInstallRequest) {
                MotionScope scope(motionDb);
                deliver(installMotion, message);
            }
        }
        if (installMotion.outgoing()) {
            const auto message = *installMotion.outgoing();
            installMotion.queued();
            if (!dropInstallReply) deliver(installBrain, message);
        }
        // Drain real fragmented read requests/responses, as the sole UART
        // owner would. All capture, parsing and identity checks stay production.
        for (unsigned n = 0; recordsBrain.outgoing() || recordsMotion.outgoing(); ++n) {
            CHECK(n < 256);
            if (recordsBrain.outgoing()) {
                const auto frame = wire(*recordsBrain.outgoing());
                recordsBrain.queued();
                if (!dropRecords) {
                    MotionScope scope(motionDb);
                    const unsigned sets = fake::count(Op::Set), commits = fake::count(Op::Commit);
                    const size_t offset = frame.payload[frame.length - 2] |
                                          (size_t(frame.payload[frame.length - 1]) << 8);
                    if (!offset && beforeCapture) beforeCapture();
                    recordsMotion.receive(frame, currentNow);
                    CHECK(fake::count(Op::Set) == sets && fake::count(Op::Commit) == commits);
                }
            }
            if (recordsMotion.outgoing()) {
                const auto frame = wire(*recordsMotion.outgoing());
                recordsMotion.queued();
                if (!dropRecords) {
                    recordsBrain.receive(frame, currentNow);
                    if (recordsBrain.state() == ExportTransferState::Complete) {
                        ++snapshotCaptures;
                        if (afterSnapshot) afterSnapshot();
                    }
                }
            }
        }
    }
};

struct Fixture {
    fake::State motionDb;
    std::vector<Trace> trace;
    std::function<void(bool, const fake::Call&)> before;
    FakeLink link;
    BrainStateStore brainStore;
    babytech::brain::BrainInstaller<FakeLink> installer;
    fake::Database protectedBrain, protectedMotion;

    Fixture() : link(motionDb), installer(link, brainStore, localSafe, nowMs, newEpoch) {
        seedSentinels(io.disk, 0x11);
        seedSentinels(motionDb.disk, 0x77);
        protectedBrain = io.disk;
        protectedMotion = motionDb.disk;
        io.before = [this](const fake::Call& c) { observed(false, c); };
        motionDb.before = [this](const fake::Call& c) { observed(true, c); };
    }
    ~Fixture() { io.before = {}; }
    static void seedSentinels(fake::Database& db, uint8_t tag) {
        for (const char* space : {"wifi-cfg", "cloudcfg", "actuatorcfg", "sensorcfg", "outbox"})
            db[space]["payload"] = {{tag, 0, 0xff}, fake::Type::Blob};
        for (const char* space : {"brainstate", "productstate", "productpair", "productctx", "formulaevt"})
            db[space]["unrelated"] = {{tag, 1, 2}, fake::Type::U32};
    }
    void observed(bool motion, const fake::Call& c) {
        trace.push_back({motion, c});
        if (before) before(motion, c);
    }
    void seedLegacy(int kind) {
        if (kind) motionDb.disk["productctx"]["payload"] = textValue(legacyJson(kind));
    }
    void resetTrace() {
        CHECK(io.handles.empty() && motionDb.handles.empty());
        trace.clear();
        io.calls.clear();
        motionDb.calls.clear();
    }
    void begin() { CHECK(installer.start(kDevice, true, currentNow)); }
    void step() { link.pump(); installer.poll(currentNow); }
    void until(Stage wanted) {
        for (unsigned i = 0; i < 12 && installer.stage() != wanted && installer.busy(); ++i) step();
        CHECK(installer.stage() == wanted);
    }
    void run() {
        begin();
        for (unsigned i = 0; i < 12 && installer.busy(); ++i) step();
        CHECK(!installer.busy());
    }
    void noWrites() const {
        for (const auto& e : trace)
            CHECK(e.call.op != Op::OpenRW && e.call.op != Op::Set && e.call.op != Op::Commit);
    }
    void audit() const {
        CHECK(io.handles.empty() && motionDb.handles.empty() && extra::mac == kBrainMac);
        for (const auto& e : trace) {
            CHECK(e.call.op != Op::Erase && e.call.op != Op::Init);
            if (e.call.op == Op::OpenRW || e.call.op == Op::Set || e.call.op == Op::Commit) {
                CHECK(e.call.name == "productpair" || e.call.name == (e.motion ? "productstate" : "brainstate"));
                if (e.call.op == Op::Set) CHECK(e.call.key == "record");
            }
        }
        for (const auto* expected : {&protectedBrain, &protectedMotion}) {
            const auto& actual = expected == &protectedBrain ? io.disk : motionDb.disk;
            for (const auto& space : *expected) for (const auto& entry : space.second)
                CHECK(actual.at(space.first).at(entry.first) == entry.second);
        }
        fake::verifyFaults();
        for (const auto& fault : motionDb.faults) CHECK(fault.hit);
    }
    void failed(const char* reason = nullptr) {
        CHECK(installer.stage() == Stage::Failed && !installer.busy());
        CHECK(std::strcmp(installer.reason(), "activation_pending"));
        if (reason) CHECK(!std::strcmp(installer.reason(), reason));
        const auto brain = io.disk, motion = motionDb.disk;
        const unsigned installs = link.installs, packets = link.installPackets, releases = link.releases;
        const bool mayHaveWritten = installer.mayHaveWritten();
        if (packets) CHECK(mayHaveWritten);
        const size_t calls = trace.size();
        for (unsigned i = 0; i < 4; ++i) {
            currentNow += 1000;
            installer.poll(currentNow);
            installer.cancel(currentNow);
        }
        CHECK(io.disk == brain && motionDb.disk == motion && trace.size() == calls);
        CHECK(link.installs == installs && link.installPackets == packets && link.releases == releases);
        CHECK(installer.mayHaveWritten() == mayHaveWritten);
    }
    void verifyPersisted(int kind, const char* expectedEpoch = nullptr, unsigned attempts = 1) {
        CHECK(installer.stage() == Stage::Persisted && !installer.busy());
        CHECK(!std::strcmp(installer.reason(), "activation_pending"));
        CHECK(installer.mayHaveWritten());
        CHECK(!std::strcmp(installer.status(), "persisted_restart_required"));
        CHECK(link.reads == attempts + 1 && link.installedReads == 1 && link.recoveryReads == attempts - 1 &&
              link.snapshotCaptures == attempts + 1 && link.installs == attempts && link.installPackets == attempts);
        CHECK(link.installationState() == BoardInstallState::Complete);
        CHECK(link.installationResult() == CommissioningResult::Installed ||
              link.installationResult() == CommissioningResult::AlreadyInstalled);
        BrainState brain;
        const auto& brainBytes = io.disk.at("brainstate").at("record").bytes;
        CHECK(decodeBrainState(brainBytes.data(), brainBytes.size(), brain));
        CHECK(brain.hasContext == (kind != 0) && brain.localSequence == 0 && !brain.pending);
        if (kind) CHECK(sameProductContext(brain.context, context(kind)));
        CHECK(!std::strcmp(brain.pairing.localPhysicalId, kBrain));
        CHECK(!std::strcmp(brain.pairing.peerPhysicalId, kMotion));
        CHECK(!std::strcmp(brain.pairing.deviceId, kDevice));
        if (expectedEpoch) CHECK(!std::strcmp(brain.pairing.epoch, expectedEpoch));
        else {
            CHECK(epochCalls == 1 && epochs.size() == 1);
            CHECK(!std::strcmp(brain.pairing.epoch, epochs.back().c_str()));
            CHECK(std::strcmp(brain.pairing.epoch, kEpoch));
        }
        CHECK(!present(io.disk, "productstate") && !present(motionDb.disk, "brainstate"));
        MotionState motion;
        const auto& motionBytes = motionDb.disk.at("productstate").at("record").bytes;
        CHECK(decodeMotionState(motionBytes.data(), motionBytes.size(), motion));
        MotionState expected;
        expected.pairing = pair(false, brain.pairing.epoch);
        if (kind) CHECK(makeMotionContextBarrier(context(kind), expected.context));
        CHECK(sameMotionState(motion, expected));
        for (bool board : {false, true}) {
            const auto& db = board ? motionDb.disk : io.disk;
            const auto& bytes = db.at("productpair").at("record").bytes;
            v4::Pairing saved;
            CHECK(decodePairingRecord(bytes.data(), bytes.size(), saved));
            std::array<uint8_t, kPairingRecordMaxSize> encoded{};
            const size_t n = encodePairingRecord(pair(!board, brain.pairing.epoch), encoded.data(), encoded.size());
            CHECK(n == bytes.size() && !std::memcmp(encoded.data(), bytes.data(), n));
        }
        const auto brainDb = io.disk, motionDbBefore = motionDb.disk;
        const unsigned installs = link.installs, discovery = link.discoveries;
        for (unsigned i = 0; i < 4; ++i) {
            currentNow += 100;
            installer.poll(currentNow);
            installer.cancel(currentNow);
            CHECK(!installer.start(kDevice, true, currentNow));
            link.pump();
        }
        CHECK(io.disk == brainDb && motionDb.disk == motionDbBefore);
        CHECK(link.installs == installs && link.discoveries == discovery);
        CHECK(!link.leaseMotion.active() && link.releases == attempts);
        CHECK(motionStoreIdle());
    }
    bool motionStoreIdle() const {
        const auto& state = link.motionStore.state();
        return state.slot.kind == MotionSlotKind::Empty && state.pendingResultCount == 0 &&
               state.cloudSequence == 0 && state.localSequence == 0;
    }
};

void scenario(const std::string& name, const std::function<void(Fixture&)>& body) {
    fake::reset();
    extra::reset();
    extra::mac = kBrainMac;
    fake_product_crypto::reset();
    currentNow = 100;
    localMaintenance = stationary = epochAvailable = true;
    epochCalls = 0;
    epochs.clear();
    clockHook = {};
    ++scenarios;
    try {
        Fixture f;
        body(f);
        f.audit();
    } catch (const std::exception& e) {
        ++failures;
        std::fprintf(stderr, "FAIL %s: %s\n", name.c_str(), e.what());
    }
    clockHook = {};
}

void seedState(Fixture& f, bool brain, int kind, bool paired = false) {
    const auto c = context(kind ? kind : 1);
    if (brain) {
        BrainStateStore store;
        CHECK(store.installInitial(pair(true), kind ? &c : nullptr) == BrainWrite::Stored);
        if (paired) {
            PairingInstaller installer;
            CHECK(installer.installFirst(v4::Role::Brain, pair(true)) == PairingInstall::Installed);
        }
    } else {
        MotionScope scope(f.motionDb);
        MotionStateStore store;
        CHECK(store.installInitial(pair(false), kind ? &c : nullptr) == MotionWrite::Stored);
        if (paired) {
            PairingInstaller installer;
            CHECK(installer.installFirst(v4::Role::Motion, pair(false)) == PairingInstall::Installed);
        }
    }
}

ProductRequest command(bool cloud, bool prepare, uint64_t sequence = 1) {
    ProductRequest q;
    q.source = cloud ? v4::Source::CloudCommand : v4::Source::LocalTouch;
    q.command = prepare ? ProductCommand::Prepare : ProductCommand::Clean;
    q.sequence = sequence;
    std::strcpy(q.deviceId, kDevice);
    if (cloud) std::strcpy(q.commandId, ("cloud-test-" + std::to_string(sequence)).c_str());
    else CHECK(makeLocalCommandId(pair(true), sequence, q.commandId));
    if (prepare) {
        const auto c = context(1);
        std::strcpy(q.babyId, c.babyId);
        q.profileVersion = c.profileVersion;
        q.waterMl = c.waterMl;
        q.temperatureC = c.temperatureC;
        q.powderGPer100Ml = c.powderGPer100Ml;
    }
    CHECK(validProductRequest(q));
    return q;
}

void success() {
    std::vector<std::string> fresh;
    for (int kind : {0, 1, 2}) scenario("fresh context=" + std::to_string(kind), [&](Fixture& f) {
        f.seedLegacy(kind);
        const auto legacy = f.motionDb.disk.at("productctx");
        bool motionReadBack = false, verifiedSnapshot = false;
        f.link.afterSnapshot = [&] { if (f.link.reads == 2) verifiedSnapshot = true; };
        f.before = [&](bool motion, const fake::Call& c) {
            if (motion && c.op == Op::Read && c.name == "productpair" && present(io.disk, "productpair"))
                motionReadBack = true;
            if (!motion && c.op == Op::OpenRW) {
                CHECK(motionReadBack && verifiedSnapshot && f.link.snapshotCaptures == 2);
                CHECK(f.link.installationState() == BoardInstallState::Complete);
                CHECK(present(f.motionDb.disk, "productstate") && present(f.motionDb.disk, "productpair"));
            }
            if (c.op == Op::Set && c.name == "productpair") {
                CHECK(present(io.disk, motion ? "productstate" : "brainstate"));
                const auto first = std::find_if(f.trace.begin(), f.trace.end(), [&](const Trace& t) {
                    return t.motion == motion && t.call.op == Op::Read &&
                           t.call.name == (motion ? "productstate" : "brainstate");
                });
                CHECK(first != f.trace.end());
            }
        };
        f.run();
        f.verifyPersisted(kind);
        CHECK(f.motionDb.disk.at("productctx") == legacy);
        CHECK(fake::count(Op::Set) == 2);
        {
            MotionScope scope(f.motionDb);
            CHECK(fake::count(Op::Set) == 2);
        }
        fresh.push_back(epochs.back());
    });
    CHECK(fresh.size() == 3);
    CHECK(fresh[0] != fresh[1] && fresh[0] != fresh[2] && fresh[1] != fresh[2]);
}

void resume() {
    for (int kind : {0, 1, 2}) for (unsigned topology = 0; topology < 6; ++topology)
        scenario("partial resume context/topology=" + std::to_string(kind) + "/" + std::to_string(topology),
                 [=](Fixture& f) {
            f.seedLegacy(kind);
            const bool brain = topology == 1 || topology >= 3;
            const bool motion = topology != 1;
            if (brain) seedState(f, true, kind, topology == 4 || topology == 5);
            if (motion) seedState(f, false, kind, topology == 2 || topology == 5);
            const auto brainDb = io.disk, motionDb = f.motionDb.disk;
            f.resetTrace();
            f.run();
            if (topology == 4) {
                // A running paired Brain correctly refuses discovery of a
                // missing Motion pair. This is not the unpaired orphan-state
                // resume path; do not fake an unpaired boot for this topology.
                f.failed("discovery_unavailable"); f.noWrites();
                CHECK(io.disk == brainDb && f.motionDb.disk == motionDb);
                return;
            }
            f.verifyPersisted(kind, kEpoch);
            CHECK(epochCalls == 0);
            if (brain) CHECK(io.disk.at("brainstate") == brainDb.at("brainstate"));
            if (motion) CHECK(f.motionDb.disk.at("productstate") == motionDb.at("productstate"));
            CHECK(fake::count(Op::Set) == unsigned(!brain) + unsigned(topology != 4 && topology != 5));
            MotionScope scope(f.motionDb);
            CHECK(fake::count(Op::Set) == unsigned(!motion) + unsigned(topology != 2 && topology != 5));
        });
}

void used() {
    for (bool paired : {false, true}) for (unsigned kind = 0; kind < 3; ++kind)
        scenario("used Brain pending/watermark=" + std::to_string(kind) + "/" + std::to_string(paired),
                 [=](Fixture& f) {
            f.seedLegacy(1);
            seedState(f, true, 1, paired);
            if (paired) seedState(f, false, 1, true);
            BrainStateStore store;
            CHECK(store.load(pair(true)) == BrainLoad::Ready);
            const auto q = command(false, kind == 2);
            CHECK(store.reserveLocal(q) == BrainWrite::Stored);
            if (kind == 0) CHECK(store.clearPending(q) == BrainWrite::Stored);
            const auto brain = io.disk, motion = f.motionDb.disk;
            f.resetTrace();
            f.run();
            f.failed("existing_records_conflict");
            f.noWrites();
            CHECK(io.disk == brain && f.motionDb.disk == motion && f.link.installs == 0);
        });
    for (bool paired : {false, true}) for (unsigned kind = 0; kind < 8; ++kind)
        scenario("used Motion results/watermarks=" + std::to_string(kind) + "/" + std::to_string(paired),
                 [=](Fixture& f) {
            f.seedLegacy(1);
            seedState(f, false, 1, paired);
            {
                MotionScope scope(f.motionDb);
                MotionStateStore store;
                CHECK(store.load(pair(false)) == MotionLoad::Ready);
                if (kind < 3) {
                    const auto q = command(kind == 1, false);
                    CHECK(store.recordDecision(q, kind == 2, kind == 2 ? "accepted" : "busy",
                                               kind == 2 ? kExecution : nullptr) == MotionWrite::Stored);
                } else if (kind == 7) {
                    CHECK(store.recordCloudStop(3, "cloud-stop-test", nullptr, true, "accepted", true) == MotionWrite::Stored);
                } else {
                    const unsigned count = kind == 6 ? kMotionResultQueueCapacity : 1;
                    for (unsigned n = 1; n <= count; ++n) {
                        std::string execution(kExecution);
                        execution.back() = char('0' + n);
                        CHECK(store.recordDecision(command(true, true, n), true, "accepted", execution.c_str()) == MotionWrite::Stored);
                        if (kind >= 4) CHECK(store.finishFeeding(execution.c_str(), true, "", "", 123) == MotionWrite::Stored);
                        if (kind >= 5) CHECK(store.archiveFeeding(true) == MotionWrite::Stored);
                    }
                }
            }
            const auto brain = io.disk, motion = f.motionDb.disk;
            f.resetTrace();
            f.run();
            f.failed("existing_records_conflict");
            f.noWrites();
            CHECK(io.disk == brain && f.motionDb.disk == motion && f.link.installs == 0);
        });
}

void writePair(fake::Database& db, const v4::Pairing& p) {
    std::array<uint8_t, kPairingRecordMaxSize> bytes{};
    const size_t n = encodePairingRecord(p, bytes.data(), bytes.size());
    CHECK(n);
    db["productpair"]["record"] = {{bytes.begin(), bytes.begin() + n}, fake::Type::Blob};
}

void conflicts() {
    for (bool brain : {false, true}) for (unsigned kind = 0; kind < 7; ++kind)
        scenario("identity/corruption board/kind=" + std::to_string(brain) + "/" + std::to_string(kind),
                 [=](Fixture& f) {
            auto& db = brain ? io.disk : f.motionDb.disk;
            auto p = pair(brain);
            if (kind == 0) std::strcpy(p.localPhysicalId, kForeign);
            if (kind == 1) std::strcpy(p.peerPhysicalId, kForeign);
            if (kind == 2) std::strcpy(p.deviceId, "Foreign_device");
            if (kind <= 3) writePair(db, p); // kind 3: valid pair, missing state.
            if (kind == 4) db["productpair"]["record"] = {{1, 2, 3}, fake::Type::Blob};
            if (kind == 5) db[brain ? "brainstate" : "productstate"]["record"] = {{1, 2, 3}, fake::Type::Blob};
            if (kind == 6) {
                seedState(f, brain, 0);
                writePair(db, pair(brain, kOtherEpoch));
            }
            const auto oldBrain = io.disk, oldMotion = f.motionDb.disk;
            f.resetTrace();
            if (brain && kind == 2) {
                CHECK(!f.installer.start(kDevice, true, currentNow));
                CHECK(f.installer.stage() == Stage::Idle);
            } else { f.run(); f.failed(); }
            f.noWrites();
            CHECK(io.disk == oldBrain && f.motionDb.disk == oldMotion && f.link.installs == 0);
        });
    scenario("two valid orphan states disagree on epoch", [](Fixture& f) {
        seedState(f, true, 0);
        {
            MotionScope scope(f.motionDb);
            MotionStateStore store;
            CHECK(store.installInitial(pair(false, kOtherEpoch)) == MotionWrite::Stored);
        }
        const auto brain = io.disk, motion = f.motionDb.disk;
        f.resetTrace(); f.run(); f.failed(); f.noWrites();
        CHECK(io.disk == brain && f.motionDb.disk == motion);
    });
    for (int kind : {0, 1, 2, 3}) scenario("legacy mismatch/pending=" + std::to_string(kind), [=](Fixture& f) {
        if (kind == 0) {
            f.seedLegacy(2); seedState(f, true, 1);
        } else if (kind == 1) seedState(f, true, 1);
        else if (kind == 2) f.motionDb.disk["productctx"]["payload"] = textValue("{broken");
        else f.motionDb.disk["formulaevt"]["payload"] = textValue("{pending-event}");
        const auto brain = io.disk, motion = f.motionDb.disk;
        f.resetTrace(); f.run(); f.failed(); f.noWrites();
        CHECK(io.disk == brain && f.motionDb.disk == motion && f.link.installs == 0);
    });
}

void gates() {
    for (unsigned kind = 0; kind < 7; ++kind)
        scenario("start rejects missing consent/maintenance/bad device=" + std::to_string(kind), [=](Fixture& f) {
            const char* device = kDevice;
            if (kind == 2) device = nullptr;
            if (kind == 3) device = "";
            if (kind == 4) device = "_bad";
            if (kind == 5) device = "bad/device";
            const std::string longDevice(65, 'a');
            if (kind == 6) device = longDevice.c_str();
            if (kind == 1) localMaintenance = false;
            CHECK(!f.installer.start(device, kind != 0, currentNow));
            CHECK(f.installer.stage() == Stage::Idle && f.link.discoveries == 0);
            f.noWrites();
        });
    scenario("busy does not start a second discovery or import", [](Fixture& f) {
        f.begin(); CHECK(!f.installer.start(kDevice, true, currentNow));
        CHECK(f.link.discoveries == 1); f.until(Stage::Persisted); f.verifyPersisted(0);
    });
    scenario("fresh epoch entropy failure writes nothing", [](Fixture& f) {
        epochAvailable = false; f.run(); f.failed(); f.noWrites(); CHECK(epochCalls == 1 && f.link.installs == 0);
    });
    scenario("Motion stationary gate denies reservation", [](Fixture& f) {
        stationary = false; f.run(); f.failed("reservation_unavailable"); f.noWrites();
    });
}

void interruptions() {
    for (Stage stage : {Stage::Discovering, Stage::Reserving, Stage::Reading, Stage::Installing, Stage::Verifying})
        for (bool cancel : {false, true})
            scenario("cancel/local end stage=" + std::to_string(int(stage)) + "/" + std::to_string(cancel),
                     [=](Fixture& f) {
                f.seedLegacy(1); f.begin(); f.until(stage);
                if (stage == Stage::Installing) { f.link.dropInstallReply = true; f.link.pump(); }
                if (stage == Stage::Verifying) f.link.pump();
                const auto brain = io.disk, motion = f.motionDb.disk;
                if (cancel) f.installer.cancel(currentNow);
                else { localMaintenance = false; f.installer.poll(currentNow); }
                f.failed(cancel ? "cancelled_outcome_unknown" : "maintenance_ended");
                CHECK(f.installer.mayHaveWritten() == (stage == Stage::Installing || stage == Stage::Verifying));
                CHECK(io.disk == brain && f.motionDb.disk == motion && !present(io.disk, "brainstate"));
            });
    for (Stage stage : {Stage::Reading, Stage::Installing, Stage::Verifying})
        scenario("lease loss preserves unknown outcome stage=" + std::to_string(int(stage)), [=](Fixture& f) {
            f.begin(); f.until(stage);
            if (stage == Stage::Installing) { f.link.dropInstallReply = true; f.link.pump(); }
            const auto brain = io.disk, motion = f.motionDb.disk;
            f.link.dropMaintenance = true;
            currentNow += BoardMaintenance::kRenewMs;
            f.link.pump();
            currentNow += BoardMaintenance::kResponseMs;
            f.link.pump(); f.installer.poll(currentNow);
            f.failed("reservation_lost_outcome_unknown");
            CHECK(io.disk == brain && f.motionDb.disk == motion);
        });
    for (unsigned kind = 0; kind < 6; ++kind)
        scenario("response timeout no replay kind=" + std::to_string(kind), [=](Fixture& f) {
            f.seedLegacy(1);
            if (kind == 0) f.link.dropDiscovery = true;
            if (kind == 1) f.link.dropMaintenance = true;
            if (kind == 2) f.link.dropRecords = true;
            if (kind == 3) f.link.dropInstallRequest = true;
            if (kind == 4) f.link.dropInstallReply = true;
            f.begin();
            const Stage target = kind == 0 ? Stage::Discovering : kind == 1 ? Stage::Reserving :
                                 kind == 2 ? Stage::Reading : kind == 5 ? Stage::Verifying : Stage::Installing;
            f.until(target);
            if (kind == 5) f.link.dropRecords = true;
            f.link.pump();
            const auto brain = io.disk, motion = f.motionDb.disk;
            for (unsigned i = 0; i < 40 && f.installer.busy(); ++i) {
                currentNow += 100; f.step();
            }
            f.failed(kind == 0 ? "discovery_unavailable" : kind == 1 ? "reservation_unavailable" :
                     kind == 2 || kind == 5 ? "records_invalid" : "motion_write_outcome_unknown");
            CHECK(io.disk == brain && f.motionDb.disk == motion && !present(io.disk, "brainstate"));
            CHECK(f.link.installPackets == unsigned(kind >= 3));
            CHECK(present(f.motionDb.disk, "productstate") == (kind == 4 || kind == 5));
        });
}

void postRead() {
    for (unsigned kind = 0; kind < 11; ++kind)
        scenario("actual post-write export rejects changed records=" + std::to_string(kind), [=](Fixture& f) {
            f.seedLegacy(1);
            bool hit = false;
            f.link.beforeCapture = [&] {
                if (f.link.reads != 2) return;
                CHECK(!hit); hit = true;
                CHECK(extra::mac == kMotionMac && io.handles.empty());
                if (kind == 0) io.disk["productpair"].erase("record");
                if (kind == 1) io.disk["productstate"].erase("record");
                if (kind == 2) writePair(io.disk, pair(false, kOtherEpoch));
                if (kind == 3) io.disk["productctx"]["payload"] = textValue(legacyJson(2));
                if (kind == 4) io.disk["formulaevt"]["payload"] = textValue("{pending-event}");
                if (kind == 5) io.disk["productstate"]["record"] = {{1, 2, 3}, fake::Type::Blob};
                if (kind == 6 || kind == 10) {
                    auto state = f.link.motionStore.state();
                    if (kind == 6) std::strcpy(state.pairing.deviceId, "Foreign_device");
                    else CHECK(makeMotionContextBarrier(context(2), state.context));
                    std::array<uint8_t, kMotionStateMaxSize> bytes{};
                    const size_t n = encodeMotionState(state, bytes.data(), bytes.size());
                    CHECK(n);
                    io.disk["productstate"]["record"] = {{bytes.begin(), bytes.begin() + n}, fake::Type::Blob};
                }
                if (kind == 7) extra::mac = {{0xa1, 0x23, 0x45, 0x67, 0x89, 0xab}};
                if (kind == 8) f.link.nullSnapshot = true;
                if (kind == 9) io.disk["productctx"]["payload"] = textValue(
                    "{\"type\":\"feeding_context\",\"device_id\":\"Foreign_device\",\"profile_version\":1,\"cleared\":true}");
            };
            f.run(); f.failed(); CHECK(hit);
            CHECK(!present(io.disk, "brainstate") && !present(io.disk, "productpair"));
            CHECK(fake::count(Op::Set) == 0 && f.link.installs == 1 && f.installer.mayHaveWritten());
        });
    for (bool brain : {false, true}) scenario("actual NVS changes between initial read and import board=" +
        std::to_string(brain), [=](Fixture& f) {
        f.seedLegacy(1); f.begin(); f.until(Stage::Installing);
        if (brain) seedState(f, true, 2);
        else f.motionDb.disk["productctx"]["payload"] = textValue(legacyJson(2));
        const auto old = brain ? io.disk : f.motionDb.disk;
        f.until(Stage::Failed); f.failed();
        CHECK((brain ? io.disk : f.motionDb.disk) == old);
        CHECK(!present(io.disk, "productpair"));
    });
    for (unsigned kind = 0; kind < 4; ++kind)
        scenario("actual Motion used after ACK before verification=" + std::to_string(kind), [=](Fixture& f) {
            f.seedLegacy(1); f.begin(); f.until(Stage::Verifying);
            {
                MotionScope scope(f.motionDb);
                auto p = f.link.motionStore.state().pairing;
                auto q = command(kind != 0, kind >= 2);
                if (kind == 0) {
                    p.role = v4::Role::Brain;
                    std::swap(p.localPhysicalId, p.peerPhysicalId);
                    CHECK(makeLocalCommandId(p, 1, q.commandId));
                }
                CHECK(f.link.motionStore.recordDecision(q, kind >= 2, kind >= 2 ? "accepted" : "busy",
                                                       kind >= 2 ? kExecution : nullptr) == MotionWrite::Stored);
                if (kind == 3) {
                    CHECK(f.link.motionStore.finishFeeding(kExecution, true, "", "", 123) == MotionWrite::Stored);
                    CHECK(f.link.motionStore.archiveFeeding(true) == MotionWrite::Stored);
                }
            }
            const auto motion = f.motionDb.disk;
            f.resetTrace(); f.until(Stage::Failed); f.failed("motion_verification_failed");
            CHECK(f.motionDb.disk == motion); f.noWrites();
        });
    for (unsigned kind = 0; kind < 3; ++kind)
        scenario("guard rechecks lease/local after matching post-read=" + std::to_string(kind), [=](Fixture& f) {
            f.begin(); f.until(Stage::Verifying); f.link.pump();
            CHECK(f.link.recordsState() == ExportTransferState::Complete);
            f.resetTrace();
            bool hit = false;
            clockHook = [&] {
                if (hit) return;
                hit = true;
                if (kind == 0) { currentNow += BoardMaintenance::kLeaseMs; f.link.leaseMotion.poll(currentNow); }
                if (kind == 1) localMaintenance = false;
                if (kind == 2) {
                    CHECK(f.link.leaseBrain.release(currentNow));
                }
            };
            f.installer.poll(currentNow);
            CHECK(hit); clockHook = {};
            f.failed("import_unsafe"); f.noWrites();
            CHECK(present(f.motionDb.disk, "productpair") && !present(io.disk, "brainstate"));
        });
}

bool console(Fixture& f, const char* line, std::string& output) {
    std::array<char, 192> bytes{};
    const bool handled = babytech::brain::BrainInstallConsole::handle(
        line, f.installer, currentNow, bytes.data(), bytes.size());
    output = bytes.data();
    return handled;
}

void consoleTests() {
    const std::vector<std::string> invalid = {
        "PAIR INSTALL", "PAIR INSTALL ", "PAIR INSTALL " + std::string(kDevice),
        "PAIR INSTALL " + std::string(kDevice) + " HANDOFF_CONFIRM",
        "PAIR INSTALL " + std::string(kDevice) + " handoff_confirmed",
        "PAIR INSTALL " + std::string(kDevice) + " HANDOFF_CONFIRMEDx",
        "PAIR INSTALL " + std::string(kDevice) + " HANDOFF_CONFIRMED ",
        "PAIR INSTALL " + std::string(kDevice) + " HANDOFF_CONFIRMED extra",
        "PAIR INSTALL  " + std::string(kDevice) + " HANDOFF_CONFIRMED",
        "PAIR INSTALL " + std::string(kDevice) + "  HANDOFF_CONFIRMED",
        "PAIR INSTALL\t" + std::string(kDevice) + " HANDOFF_CONFIRMED",
        "PAIR INSTALL " + std::string(kDevice) + "\tHANDOFF_CONFIRMED",
        "PAIR INSTALL " + std::string(kDevice) + " HANDOFF_CONFIRMED\r",
        "PAIR INSTALL _bad HANDOFF_CONFIRMED", "PAIR INSTALL bad/device HANDOFF_CONFIRMED",
        "PAIR INSTALL " + std::string(65, 'a') + " HANDOFF_CONFIRMED",
        "PAIR INSTALL " + std::string(4096, 'a') + " HANDOFF_CONFIRMED",
        "PAIR INSTALL STATUS extra", "PAIR INSTALL CANCEL extra",
        "PAIR INSTALLX " + std::string(kDevice) + " HANDOFF_CONFIRMED"
    };
    for (const auto& line : invalid) scenario("console rejects " + line.substr(0, 100), [&](Fixture& f) {
        std::string output;
        CHECK(console(f, line.c_str(), output));
        CHECK(output == "[install] not_started\n");
        CHECK(f.installer.stage() == Stage::Idle && f.link.discoveries == 0 && f.link.releases == 0);
        CHECK(epochCalls == 0); f.noWrites();
    });
    for (const char* line : {static_cast<const char*>(nullptr), "PAIR INSTAL", "pair install", "PAIR INSTALLATION", "unrelated command"})
        scenario("console unrelated prefix", [=](Fixture& f) {
            std::string output;
            const bool handled = console(f, line, output);
            // INSTALLATION shares the parser's command prefix but cannot start.
            CHECK(handled == (line && !std::strcmp(line, "PAIR INSTALLATION")));
            CHECK(f.link.discoveries == 0 && f.installer.stage() == Stage::Idle); f.noWrites();
        });
    for (bool safe : {false, true}) scenario("console consent does not replace local MAINT=" + std::to_string(safe),
        [=](Fixture& f) {
            localMaintenance = safe;
            f.seedLegacy(1);
            std::string output;
            const std::string line = std::string("PAIR INSTALL ") + kDevice + " HANDOFF_CONFIRMED";
            CHECK(console(f, line.c_str(), output));
            CHECK(output == (safe ? "[install] started\n" : "[install] not_started\n"));
            if (safe) { f.until(Stage::Persisted); f.verifyPersisted(1); }
            else { CHECK(f.link.discoveries == 0); f.noWrites(); }
        });
    scenario("console accepts full 64-character device ID without truncating", [](Fixture& f) {
        const std::string device(64, 'a');
        std::string output;
        const std::string line = "PAIR INSTALL " + device + " HANDOFF_CONFIRMED";
        CHECK(console(f, line.c_str(), output) && output == "[install] started\n");
        f.until(Stage::Persisted);
        CHECK(!std::strcmp(f.installer.reason(), "activation_pending"));
        BrainState brain;
        const auto& bytes = io.disk.at("brainstate").at("record").bytes;
        CHECK(decodeBrainState(bytes.data(), bytes.size(), brain));
        CHECK(device == brain.pairing.deviceId && !brain.pending && brain.localSequence == 0);
        CHECK(device == f.link.motionStore.state().pairing.deviceId);
        CHECK(f.motionStoreIdle());
    });
    for (Stage stage : {Stage::Idle, Stage::Discovering, Stage::Reserving, Stage::Reading,
                        Stage::Installing, Stage::Verifying, Stage::Persisted, Stage::Failed})
        scenario("console STATUS/CANCEL stage=" + std::to_string(int(stage)), [=](Fixture& f) {
            if (stage == Stage::Failed) { epochAvailable = false; f.run(); }
            else if (stage != Stage::Idle) { f.begin(); f.until(stage); }
            if (stage == Stage::Installing) { f.link.dropInstallReply = true; f.link.pump(); }
            const auto brain = io.disk, motion = f.motionDb.disk;
            const size_t calls = f.trace.size();
            const unsigned starts = f.link.discoveries, releases = f.link.releases;
            std::string output;
            CHECK(console(f, "PAIR INSTALL STATUS", output));
            CHECK(output == std::string("[install] ") + f.installer.status() + " reason=" + f.installer.reason() +
                  " writes_may_have_persisted=" + (f.installer.mayHaveWritten() ? "1\n" : "0\n"));
            CHECK(f.installer.stage() == stage && f.link.releases == releases && f.trace.size() == calls);
            CHECK(console(f, "PAIR INSTALL CANCEL", output));
            const bool wasBusy = stage != Stage::Idle && stage != Stage::Persisted && stage != Stage::Failed;
            CHECK(f.link.releases == releases + unsigned(wasBusy));
            if (wasBusy) {
                CHECK(output == std::string("[install] failed reason=cancelled_outcome_unknown writes_may_have_persisted=") +
                      (f.installer.mayHaveWritten() ? "1\n" : "0\n"));
                f.failed("cancelled_outcome_unknown");
            } else CHECK(f.installer.stage() == stage);
            CHECK(io.disk == brain && f.motionDb.disk == motion && f.trace.size() == calls);
            CHECK(f.link.discoveries == starts);
            // CANCEL queues the real reservation release. Complete that I/O
            // without running any queued unknown installation a second time.
            if (stage == Stage::Reserving || stage == Stage::Reading || stage == Stage::Installing ||
                stage == Stage::Verifying || stage == Stage::Persisted) {
                f.link.pump();
                CHECK(!f.link.leaseMotion.active());
                CHECK(io.disk == brain && f.motionDb.disk == motion);
            }
        });
}

void storageFaults() {
    for (bool brain : {false, true}) for (const char* space : {"state", "pair"})
        for (Op op : {Op::OpenRW, Op::Set, Op::Commit, Op::Read}) for (bool apply : {false, true})
            scenario("storage fault board/space/op/apply=" + std::to_string(brain) + "/" + space + "/" +
                     std::to_string(int(op)) + "/" + std::to_string(apply), [=](Fixture& f) {
                f.seedLegacy(1);
                bool injected = false;
                const std::string name = std::string(space) == "pair" ? "productpair" : brain ? "brainstate" : "productstate";
                f.before = [&](bool motion, const fake::Call& c) {
                    // Read failures here target durable post-commit readback,
                    // not a pre-install Missing query or initial export.
                    if (!injected && motion != brain && c.name == name && c.op == op &&
                        (op != Op::Read || fake::count(Op::Commit) > 0)) {
                        injected = true; fake::fail(op, c.occurrence, ESP_FAIL, apply);
                    }
                };
                f.run();
                CHECK(injected);
                f.failed("import_storage_fault");
                CHECK(!present(io.disk, "productpair") || present(io.disk, "brainstate"));
                if (brain) {
                    CHECK(present(f.motionDb.disk, "productstate") && present(f.motionDb.disk, "productpair"));
                    CHECK(f.link.installationResult() == CommissioningResult::Installed);
                } else CHECK(!present(io.disk, "brainstate") && !present(io.disk, "productpair"));
            });
}

void transfer() {
    scenario("ordinary post-read still rejects discovery Missing to persisted Ready", [](Fixture& f) {
        f.begin(); f.until(Stage::Installing); f.link.pump();
        CHECK(f.link.installationState() == BoardInstallState::Complete);
        CHECK(f.link.discoveryResult().pairingState == DiscoveryPairState::Missing);
        const auto brain = io.disk, motion = f.motionDb.disk;
        f.resetTrace();
        CHECK(f.link.requestRecords(kDevice, currentNow));
        f.link.pump();
        CHECK(f.link.recordsState() == ExportTransferState::Invalid && !f.link.recordsSnapshot());
        CHECK(io.disk == brain && f.motionDb.disk == motion); f.noWrites();
    });
    scenario("installed read requires matching expected epoch despite same hardware", [](Fixture& f) {
        f.begin(); f.until(Stage::Installing); f.link.pump();
        const auto motion = f.motionDb.disk;
        auto wrong = f.link.motionStore.state().pairing;
        std::strcpy(wrong.epoch, kOtherEpoch);
        f.resetTrace();
        CHECK(f.link.requestInstalledRecords(kDevice, wrong, currentNow));
        f.link.pump();
        CHECK(f.link.recordsState() == ExportTransferState::Invalid && !f.link.recordsSnapshot());
        CHECK(f.motionDb.disk == motion && !present(io.disk, "brainstate")); f.noWrites();
    });
    scenario("installed read cannot start before Motion installation completed", [](Fixture& f) {
        f.begin(); f.until(Stage::Installing);
        CHECK(!f.link.requestInstalledRecords(kDevice, pair(false), currentNow));
        CHECK(!present(io.disk, "brainstate") && !present(f.motionDb.disk, "productstate"));
    });
}

void leaseDuringNvs() {
    for (const char* space : {"brainstate", "productpair"}) for (bool localExit : {false, true})
        scenario("synchronous Brain NVS loses proof space/local=" + std::string(space) + "/" +
                 std::to_string(localExit), [=](Fixture& f) {
            f.seedLegacy(1); f.begin(); f.until(Stage::Verifying); f.link.pump();
            CHECK(f.link.recordsState() == ExportTransferState::Complete);
            const auto motion = f.motionDb.disk;
            f.resetTrace();
            bool hit = false;
            f.before = [&](bool motionBoard, const fake::Call& c) {
                if (!hit && !motionBoard && c.op == Op::Commit && c.name == space) {
                    hit = true;
                    if (localExit) localMaintenance = false;
                    else {
                        currentNow += BoardMaintenance::kLeaseMs;
                        // Pure RAM on the other MCU keeps ticking while Brain's
                        // synchronous NVS call holds a handle. No SDK switch.
                        f.link.leaseMotion.poll(currentNow);
                        CHECK(!f.link.leaseMotion.active());
                    }
                }
            };
            f.installer.poll(currentNow);
            CHECK(hit && f.installer.mayHaveWritten());
            f.failed(!std::strcmp(space, "brainstate") ? "import_unsafe" : "reservation_lost_after_write");
            CHECK(f.motionDb.disk == motion && present(io.disk, "brainstate"));
            if (!std::strcmp(space, "brainstate")) CHECK(!present(io.disk, "productpair"));
            if (!localExit) {
                CHECK(f.link.leaseBrain.state() == BoardMaintenanceState::TimedOut);
                CHECK(!f.link.leaseBrain.outgoing());
            }
        });
    for (bool delivered : {false, true})
        scenario("local MAINT exits with pending Motion result remains unknown=" + std::to_string(delivered),
                 [=](Fixture& f) {
            f.seedLegacy(1); f.begin(); f.until(Stage::Installing);
            f.link.dropInstallRequest = !delivered;
            f.link.dropInstallReply = true;
            f.link.pump();
            CHECK(f.link.installationState() == BoardInstallState::Pending);
            const auto brain = io.disk, motion = f.motionDb.disk;
            localMaintenance = false;
            f.installer.poll(currentNow); f.failed("maintenance_ended");
            CHECK(f.installer.mayHaveWritten() && !present(io.disk, "brainstate"));
            CHECK(present(f.motionDb.disk, "productstate") == delivered);
            std::string output;
            CHECK(console(f, "PAIR INSTALL STATUS", output));
            CHECK(output == "[install] failed reason=maintenance_ended writes_may_have_persisted=1\n");
            CHECK(io.disk == brain && f.motionDb.disk == motion && f.link.installPackets == 1);
        });
}

void warnings() {
    for (bool delivered : {false, true})
        scenario("same-boot retry retains uncertain writes warning=" + std::to_string(delivered), [=](Fixture& f) {
            f.begin(); f.until(Stage::Installing);
            CHECK(f.installer.mayHaveWritten());
            f.link.dropInstallRequest = !delivered;
            f.link.dropInstallReply = true;
            f.link.pump();
            f.installer.cancel(currentNow); f.failed("cancelled_outcome_unknown");
            f.link.pump();
            CHECK(!f.link.leaseMotion.active());
            const auto brain = io.disk, motion = f.motionDb.disk;
            CHECK(f.installer.start(kDevice, true, currentNow));
            CHECK(f.installer.stage() == Stage::Discovering && f.installer.mayHaveWritten());
            CHECK(io.disk == brain && f.motionDb.disk == motion);
            std::string output;
            CHECK(console(f, "PAIR INSTALL STATUS", output));
            CHECK(output == "[install] discovering reason=none writes_may_have_persisted=1\n");
            f.installer.cancel(currentNow);
            CHECK(f.installer.mayHaveWritten());
            BrainStateStore rebootedStore;
            babytech::brain::BrainInstaller<FakeLink> newObject(
                f.link, rebootedStore, localSafe, nowMs, newEpoch);
            CHECK(!newObject.mayHaveWritten() && newObject.stage() == Stage::Idle);
            CHECK(io.disk == brain && f.motionDb.disk == motion);
        });
    scenario("Unsafe Motion reply retains writes warning without inventing NVS", [](Fixture& f) {
        f.begin(); f.until(Stage::Installing);
        stationary = false;
        f.until(Stage::Failed); f.failed("import_unsafe");
        CHECK(f.link.installationResult() == CommissioningResult::Unsafe);
        CHECK(f.installer.mayHaveWritten()); f.noWrites();
        CHECK(!present(io.disk, "brainstate") && !present(f.motionDb.disk, "productstate"));
    });
    for (bool brain : {false, true}) for (Op op : {Op::OpenRO, Op::Query, Op::Read})
        scenario("initial evidence SDK error is not Missing board/op=" + std::to_string(brain) + "/" +
                 std::to_string(int(op)), [=](Fixture& f) {
            seedState(f, brain, 0);
            const auto oldBrain = io.disk, oldMotion = f.motionDb.disk;
            f.resetTrace();
            bool injected = false;
            f.before = [&](bool motion, const fake::Call& c) {
                if (!injected && motion != brain && c.name == (brain ? "brainstate" : "productstate") && c.op == op) {
                    injected = true; fake::fail(op, c.occurrence, ESP_FAIL);
                }
            };
            f.run(); f.failed(); CHECK(injected); f.noWrites();
            CHECK(io.disk == oldBrain && f.motionDb.disk == oldMotion && !f.installer.mayHaveWritten());
        });
}

void loseInitialInstallation(Fixture& f, int kind, bool delivered) {
    f.seedLegacy(kind);
    f.begin(); f.until(Stage::Installing);
    CHECK(f.link.discoveryResult().pairingState == DiscoveryPairState::Missing);
    CHECK(f.link.submitted.size() == 1 && f.installer.mayHaveWritten());
    f.link.dropInstallRequest = !delivered;
    f.link.dropInstallReply = true;
    f.link.pump();
    CHECK(f.link.installationState() == BoardInstallState::Pending);
    for (unsigned i = 0; i < 40 && f.installer.busy(); ++i) { currentNow += 100; f.step(); }
    CHECK(f.installer.stage() == Stage::Failed);
    CHECK(!std::strcmp(f.installer.reason(), "motion_write_outcome_unknown"));
    CHECK(f.link.installationState() == BoardInstallState::TimedOut);
    CHECK(present(f.motionDb.disk, "productpair") == delivered);
    CHECK(present(f.motionDb.disk, "productstate") == delivered);
    CHECK(!present(io.disk, "brainstate") && !present(io.disk, "productpair"));
    CHECK(f.link.installs == 1 && f.link.installPackets == 1 && f.link.reads == 1);
    // Complete the actual END handshake before advancing the idle clock.
    f.link.pump();
    CHECK(f.link.maintenanceState() == BoardMaintenanceState::Released && !f.link.leaseMotion.active());
    f.failed("motion_write_outcome_unknown");
    f.link.dropInstallRequest = f.link.dropInstallReply = false;
    f.resetTrace();
}

void recovery() {
    for (int kind : {0, 1, 2}) for (bool delivered : {false, true})
        scenario("same-boot explicit retry request/ACK lost context/delivered=" + std::to_string(kind) + "/" +
                 std::to_string(delivered), [=](Fixture& f) {
            loseInitialInstallation(f, kind, delivered);
            const auto original = f.link.submitted.front();
            const auto beforeBrain = io.disk, beforeMotion = f.motionDb.disk;
            CHECK(epochCalls == 1);
            // start itself must not refresh discovery from the now-written NVS.
            const size_t calls = f.trace.size();
            f.begin();
            CHECK(f.trace.size() == calls && f.installer.mayHaveWritten());
            f.until(Stage::Reading);
            CHECK(f.link.discoveries == 2 && f.link.reservations == 2);
            CHECK(f.link.discoveryResult().pairingState == DiscoveryPairState::Missing);
            CHECK(f.link.discoveryResult().peerBoot == kMotionBoot);
            CHECK(f.link.recoveryReads == 1 && f.link.installedReads == 0);
            CHECK(!std::strcmp(f.link.installationNonce(), kFresh));
            f.link.pump();
            CHECK(f.link.recordsState() == ExportTransferState::Complete);
            const auto* records = f.link.recordsSnapshot();
            CHECK(records && records->pairStatus == (delivered ? ExportRead::Ready : ExportRead::Missing));
            CHECK(records->stateStatus == (delivered ? ExportRead::Ready : ExportRead::Missing));
            CHECK(io.disk == beforeBrain && f.motionDb.disk == beforeMotion);
            f.installer.poll(currentNow);
            CHECK(f.installer.stage() == Stage::Installing && f.link.submitted.size() == 2);
            const auto& retry = f.link.submitted.back();
            CHECK(f.link.installBrain.matchesRequestedPair(original.pairing));
            CHECK(!std::strcmp(retry.pairing.epoch, original.pairing.epoch));
            CHECK(retry.hasContext == original.hasContext);
            if (retry.hasContext) CHECK(sameProductContext(retry.context, original.context));
            CHECK(epochCalls == 1 && epochs.size() == 1);
            f.until(Stage::Persisted);
            f.verifyPersisted(kind, original.pairing.epoch, 2);
            CHECK(f.link.installationResult() == (delivered ? CommissioningResult::AlreadyInstalled :
                                                            CommissioningResult::Installed));
            CHECK(f.link.readChallenges.size() == 3);
            CHECK(f.link.readChallenges[0] == kNonce && f.link.readChallenges[1] == kFresh);
            CHECK(f.link.discoveryResult().pairingState == DiscoveryPairState::Missing);
            MotionScope scope(f.motionDb);
            CHECK(fake::count(Op::Set) == (delivered ? 0u : 2u));
            CHECK(fake::count(Op::Commit) == (delivered ? 0u : 2u));
            if (delivered) CHECK(io.disk == beforeMotion);
        });
    for (int kind : {0, 1, 2}) for (bool paired : {false, true})
        scenario("recovery refuses old foreign epoch even after original request lost=" + std::to_string(kind) + "/" +
                 std::to_string(paired), [=](Fixture& f) {
            loseInitialInstallation(f, kind, false);
            const auto original = f.link.submitted.front();
            // Another installation's legitimate records appear after boot. Use
            // real commissioning primitives, not a fake successful recovery.
            seedState(f, false, kind, paired);
            CHECK(std::strcmp(original.pairing.epoch, kEpoch));
            const auto brain = io.disk, motion = f.motionDb.disk;
            f.resetTrace(); f.run(); f.failed(paired ? "records_invalid" : "existing_records_conflict");
            CHECK(f.link.recoveryReads == 1 && f.link.installs == 1 && f.link.installPackets == 1);
            CHECK(f.link.discoveryResult().pairingState == DiscoveryPairState::Missing);
            CHECK(io.disk == brain && f.motionDb.disk == motion && epochCalls == 1);
            CHECK(f.installer.mayHaveWritten()); f.noWrites();
        });
    for (int kind : {0, 1, 2}) for (bool delivered : {false, true})
        scenario("recovery cannot adopt changed legacy content context/delivered=" + std::to_string(kind) + "/" +
                 std::to_string(delivered), [=](Fixture& f) {
            loseInitialInstallation(f, kind, delivered);
            f.motionDb.disk["productctx"]["payload"] = textValue(legacyJson(kind == 1 ? 2 : 1));
            const auto brain = io.disk, motion = f.motionDb.disk;
            f.run(); f.failed("existing_records_conflict");
            CHECK(f.link.recordsState() == ExportTransferState::Complete);
            CHECK(f.link.recoveryReads == 1 && f.link.installs == 1 && epochCalls == 1);
            CHECK(io.disk == brain && f.motionDb.disk == motion); f.noWrites();
        });
    for (int kind : {0, 1, 2}) scenario("recovery cannot adopt changed Brain orphan content=" + std::to_string(kind),
        [=](Fixture& f) {
            loseInitialInstallation(f, kind, true);
            auto brainPair = f.link.submitted.front().pairing;
            brainPair.role = v4::Role::Brain;
            std::swap(brainPair.localPhysicalId, brainPair.peerPhysicalId);
            const auto changed = context(kind == 1 ? 2 : 1);
            BrainStateStore otherWriter;
            CHECK(otherWriter.installInitial(brainPair, &changed) == BrainWrite::Stored);
            const auto brain = io.disk, motion = f.motionDb.disk;
            f.resetTrace(); f.run(); f.failed("existing_records_conflict");
            CHECK(f.link.recordsState() == ExportTransferState::Complete);
            CHECK(f.link.recoveryReads == 1 && f.link.installs == 1 && epochCalls == 1);
            CHECK(io.disk == brain && f.motionDb.disk == motion); f.noWrites();
        });
    scenario("uncertain same-boot retry cannot change Device ID", [](Fixture& f) {
        loseInitialInstallation(f, 1, true);
        const auto brain = io.disk, motion = f.motionDb.disk;
        CHECK(!f.installer.start("Foreign_device", true, currentNow));
        CHECK(f.installer.stage() == Stage::Failed && f.installer.mayHaveWritten());
        CHECK(f.link.discoveries == 1 && f.link.recoveryReads == 0);
        CHECK(io.disk == brain && f.motionDb.disk == motion); f.noWrites();
    });
    scenario("recovery adapter refuses unsubmitted pair and inactive reservation", [](Fixture& f) {
        f.begin(); f.until(Stage::Reading);
        CHECK(!f.link.requestRecoveryRecords(kDevice, pair(false), currentNow));
        f.until(Stage::Installing);
        const auto expected = f.link.submitted.front().pairing;
        auto wrong = expected;
        std::strcpy(wrong.epoch, kOtherEpoch);
        CHECK(!f.link.requestRecoveryRecords(kDevice, wrong, currentNow));
        f.installer.cancel(currentNow);
        CHECK(!f.link.requestRecoveryRecords(kDevice, expected, currentNow));
        CHECK(f.link.recordsState() == ExportTransferState::Complete);
        CHECK(!f.link.recordsBrain.outgoing()); f.noWrites();
    });
}
}

int main(int argc, char** argv) {
    const std::pair<const char*, void (*)()> groups[] = {
        {"success", success}, {"resume", resume}, {"used", used}, {"conflicts", conflicts},
        {"gates", gates}, {"interruptions", interruptions}, {"post-read", postRead}, {"faults", storageFaults},
        {"console", consoleTests}, {"transfer", transfer}, {"lease-nvs", leaseDuringNvs}, {"warnings", warnings},
        {"recovery", recovery}
    };
    bool selected = false;
    for (const auto& group : groups) if (argc == 1 || !std::strcmp(argv[1], group.first)) {
        selected = true;
        try { group.second(); }
        catch (const std::exception& e) { ++failures; std::fprintf(stderr, "FAIL group %s: %s\n", group.first, e.what()); }
    }
    if (argc > 2 || !selected) { std::fprintf(stderr, "Unknown test group\n"); return 2; }
    std::printf("BrainInstaller: %u scenarios, %u failures, %u real UART frames; isolated Brain/Motion NVS; "
                "Persisted is activation_pending, not runtime/network activation\n", scenarios, failures, wireFrames);
    return failures ? 1 : 0;
}
