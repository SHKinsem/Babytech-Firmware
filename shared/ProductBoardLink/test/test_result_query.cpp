#include "ProductResultQuery.h"
#include "FakeBrainNvs.h"
#include "FakeProductCrypto.h"
#include "RecordBytes.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace babytech::boardlink;
namespace v4 = babytech::v4;
namespace fake = fake_brain;
using fake::Bytes;
using fake::Op;
using fake::io;

namespace {
unsigned scenarios = 0, failures = 0, lookups = 0, codecRejections = 0;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + \
    std::to_string(__LINE__) + ": " #x); } while (false)
constexpr const char* execution = "123456789abcdef0123456789abcdef0";

template <typename T> Bytes raw(const T& value) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    return Bytes(bytes, bytes + sizeof(value));
}
v4::Pairing pairing() {
    v4::Pairing p;
    p.role = v4::Role::Motion;
    std::strcpy(p.deviceId, "Babytech_01-test");
    std::strcpy(p.epoch, "0123456789abcdef0123456789abcdef");
    std::strcpy(p.localPhysicalId, "012345abcdef");
    std::strcpy(p.peerPhysicalId, "fedcba987654");
    return p;
}
ProductContext context(const v4::Pairing& p = pairing()) {
    ProductContext c;
    std::strcpy(c.deviceId, p.deviceId);
    c.profileVersion = 10;
    std::strcpy(c.babyId, "baby-original");
    std::strcpy(c.babyName, "Full baby name");
    std::strcpy(c.formulaBrand, "Full formula brand");
    c.waterMl = 120;
    c.temperatureC = 40;
    c.powderGPer100Ml = 13.5f;
    CHECK(validProductContext(c));
    return c;
}
ProductRequest request(uint64_t sequence = 8, v4::Source source = v4::Source::CloudCommand,
                       ProductCommand command = ProductCommand::Prepare,
                       const v4::Pairing& p = pairing()) {
    ProductRequest r;
    r.sequence = sequence;
    r.source = source;
    r.command = command;
    std::strcpy(r.deviceId, p.deviceId);
    if (source == v4::Source::LocalTouch) {
        auto brain = p;
        brain.role = v4::Role::Brain;
        std::swap(brain.localPhysicalId, brain.peerPhysicalId);
        CHECK(makeLocalCommandId(brain, sequence, r.commandId));
    } else std::snprintf(r.commandId, sizeof(r.commandId), "cloud-%llu",
                         static_cast<unsigned long long>(sequence));
    if (command == ProductCommand::Prepare) {
        const auto c = context(p);
        std::strcpy(r.babyId, c.babyId);
        r.profileVersion = c.profileVersion;
        r.waterMl = 180;
        r.temperatureC = 45;
        r.powderGPer100Ml = c.powderGPer100Ml;
    } else if (command == ProductCommand::SetTargetTemp) r.temperatureC = 45;
    CHECK(validProductRequest(r));
    return r;
}
ResultQuery query(const ProductRequest& request = ::request()) {
    ResultQuery q;
    q.source = request.source;
    q.sequence = request.sequence;
    std::memcpy(q.deviceId, request.deviceId, sizeof(q.deviceId));
    std::memcpy(q.commandId, request.commandId, sizeof(q.commandId));
    CHECK(validResultQuery(q));
    return q;
}
void install(MotionStateStore& store, const v4::Pairing& p = pairing()) {
    const auto c = context(p);
    CHECK(store.installInitial(p, &c) == MotionWrite::Stored);
}
std::string hexDigest(const ProductRequest& r) {
    uint8_t digest[kProductDigestSize];
    CHECK(requestDigest(r, digest));
    std::string result;
    for (auto byte : digest) {
        result += "0123456789abcdef"[byte >> 4];
        result += "0123456789abcdef"[byte & 15];
    }
    return result;
}
v4::Message wire(const std::string& json, v4::Kind kind = v4::Kind::ResultQuery) {
    CHECK(json.size() <= v4::kMaxMessage);
    v4::Message m;
    m.kind = kind;
    m.length = uint16_t(json.size());
    std::memcpy(m.payload, json.data(), json.size());
    return m;
}
std::string json(const v4::Message& m) {
    return std::string(reinterpret_cast<const char*>(m.payload), m.length);
}
void roundtrip(const QueriedResult& result) {
    v4::Message m;
    CHECK(encodeQueriedResult(result, m));
    CHECK(m.kind == v4::Kind::Result && !m.senderBoot && !m.receiverBoot && !m.messageId);
    QueriedResult decoded;
    CHECK(decodeQueriedResult(m, decoded));
    CHECK(sameResultQuery(result.query, decoded.query) && result.status == decoded.status &&
        result.accepted == decoded.accepted && !std::strcmp(result.reason, decoded.reason) &&
        result.outcome == decoded.outcome && !std::strcmp(result.requestDigestHex, decoded.requestDigestHex));
    v4::Message encoded;
    CHECK(encodeQueriedResult(decoded, encoded) && json(encoded) == json(m));
}
QueriedResult lookup(const MotionStateStore& store, const ResultQuery& q,
                    ResultQueryStatus expected, bool accepted = false,
                    const char* reason = nullptr, MotionOutcome outcome = MotionOutcome::None,
                    const ProductRequest* ordinary = nullptr) {
    const auto disk = io.disk;
    const auto storeBytes = raw(store);
    const size_t calls = io.calls.size();
    const unsigned cryptoCalls = fake_product_crypto::calls;
    QueriedResult result;
    CHECK(queryMotionResult(store, q, result));
    ++lookups;
    CHECK(io.calls.size() == calls && io.disk == disk && raw(store) == storeBytes);
    CHECK(fake_product_crypto::calls == cryptoCalls);
    CHECK(sameResultQuery(result.query, q) && result.status == expected);
    CHECK(result.accepted == accepted && result.outcome == outcome);
    if (!reason) {
        switch (expected) {
            case ResultQueryStatus::Unknown: reason = "unknown"; break;
            case ResultQueryStatus::Expired: reason = "result_expired"; break;
            case ResultQueryStatus::Conflict: reason = "request_conflict"; break;
            case ResultQueryStatus::StorageFault: reason = "storage_fault"; break;
            default: throw std::runtime_error("Known needs original reason");
        }
    }
    CHECK(!std::strcmp(result.reason, reason));
    CHECK(ordinary ? hexDigest(*ordinary) == result.requestDigestHex : !result.requestDigestHex[0]);
    roundtrip(result);
    return result;
}
void invalidLookup(const MotionStateStore& store, const ResultQuery& q) {
    QueriedResult result;
    std::strcpy(result.reason, "sentinel");
    const auto before = raw(result), storeBefore = raw(store);
    const auto disk = io.disk;
    const auto calls = io.calls.size();
    CHECK(!queryMotionResult(store, q, result));
    CHECK(raw(result) == before && raw(store) == storeBefore && io.disk == disk && io.calls.size() == calls);
    ++lookups;
}
void scenario(const std::string& name, const std::function<void()>& run) {
    fake::reset();
    fake_product_crypto::reset();
    io.disk["wifi-cfg"]["unrelated"] = {{1, 2, 3}, fake::Type::String};
    ++scenarios;
    try {
        run();
        fake::verifyFaults();
        CHECK(io.handles.empty() && !fake::count(Op::Erase) && !fake::count(Op::Init));
        CHECK(io.disk.at("wifi-cfg").at("unrelated").bytes == Bytes({1, 2, 3}));
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "FAIL %s: %s\n", name.c_str(), error.what());
        io.handles.clear();
    }
}
void noWrites() {
    CHECK(!fake::count(Op::OpenRW) && !fake::count(Op::Set) && !fake::count(Op::Commit));
}

void decisions() {
    for (auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch}) {
        for (const char* reason : {"busy", "not_ready", "context_stale", "request_expired", "storage_fault"})
            scenario(std::string("original rejection ") + reason, [=] {
                MotionStateStore store;
                install(store);
                const auto r = request(8, source);
                CHECK(store.recordDecision(r, false, reason) == MotionWrite::Stored);
                lookup(store, query(r), ResultQueryStatus::Known, false, reason, MotionOutcome::None, &r);
                MotionStateStore reloaded;
                CHECK(reloaded.load(pairing()) == MotionLoad::Ready);
                lookup(reloaded, query(r), ResultQueryStatus::Known, false, reason, MotionOutcome::None, &r);
            });
        for (auto command : {ProductCommand::Prepare, ProductCommand::Clean, ProductCommand::Initialize}) {
            if (command == ProductCommand::Initialize && source == v4::Source::CloudCommand) continue;
            for (auto outcome : {MotionOutcome::Succeeded, MotionOutcome::Interrupted, MotionOutcome::Failed}) {
                if (command == ProductCommand::Prepare && outcome == MotionOutcome::Interrupted) continue;
                scenario("accepted intent and terminal", [=] {
                    MotionStateStore store;
                    install(store);
                    const auto r = request(8, source, command);
                    CHECK(store.recordDecision(r, true, "accepted", execution) == MotionWrite::Stored);
                    lookup(store, query(r), ResultQueryStatus::Known, true, "accepted", MotionOutcome::None, &r);
                    if (command == ProductCommand::Prepare) {
                        const bool completed = outcome == MotionOutcome::Succeeded;
                        CHECK(store.finishFeeding(execution, completed, completed ? "" : "restart_interrupted",
                            completed ? "" : "LINK_LOST", 123) == MotionWrite::Stored);
                        lookup(store, query(r), ResultQueryStatus::Known, true, "accepted", outcome, &r);
                        CHECK(store.archiveFeeding(true) == MotionWrite::Stored);
                    } else CHECK(store.finishOperation(execution, outcome, true) == MotionWrite::Stored);
                    lookup(store, query(r), ResultQueryStatus::Known, true, "accepted", outcome, &r);
                    MotionStateStore reloaded;
                    CHECK(reloaded.load(pairing()) == MotionLoad::Ready);
                    lookup(reloaded, query(r), ResultQueryStatus::Known, true, "accepted", outcome, &r);
                });
            }
        }
        scenario("already clear and temperature original acceptance", [=] {
            MotionStateStore store;
            install(store);
            for (auto command : {ProductCommand::ResetError, ProductCommand::SetTargetTemp}) {
                const auto r = request(command == ProductCommand::ResetError ? 8 : 9, source, command);
                const char* reason = command == ProductCommand::ResetError ? "already_clear" : "accepted";
                CHECK(store.recordDecision(r, true, reason) == MotionWrite::Stored);
                lookup(store, query(r), ResultQueryStatus::Known, true, reason, MotionOutcome::None, &r);
            }
        });
    }
}
void retained() {
    for (auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch})
        scenario("older slot and full queue survive newer rejection", [=] {
            MotionStateStore store;
            install(store);
            std::vector<ProductRequest> history;
            std::vector<bool> completed;
            for (uint64_t seq = 1; seq <= kMotionResultQueueCapacity; ++seq) {
                const auto r = request(seq * 10, source);
                char id[33];
                std::snprintf(id, sizeof(id), "%032llu", static_cast<unsigned long long>(seq));
                CHECK(store.recordDecision(r, true, "accepted", id) == MotionWrite::Stored);
                const auto reject = request(seq * 10 + 1, source);
                CHECK(store.recordDecision(reject, false, "busy") == MotionWrite::Stored);
                lookup(store, query(r), ResultQueryStatus::Known, true, "accepted", MotionOutcome::None, &r);
                const bool success = seq % 2 == 0;
                CHECK(store.finishFeeding(id, success, success ? "" : "aborted", "", 500) == MotionWrite::Stored);
                lookup(store, query(r), ResultQueryStatus::Known, true, "accepted",
                    success ? MotionOutcome::Succeeded : MotionOutcome::Failed, &r);
                CHECK(store.archiveFeeding(true) == MotionWrite::Stored);
                history.push_back(r);
                completed.push_back(success);
                lookup(store, query(reject), ResultQueryStatus::Known, false, "busy", MotionOutcome::None, &reject);
                for (size_t i = 0; i < history.size(); ++i)
                    lookup(store, query(history[i]), ResultQueryStatus::Known, true, "accepted",
                        completed[i] ? MotionOutcome::Succeeded : MotionOutcome::Failed, &history[i]);
            }
            // A non-feeding intent can coexist with a full feeding-result queue.
            const auto clean = request(50, source, ProductCommand::Clean);
            CHECK(store.recordDecision(clean, true, "accepted", execution) == MotionWrite::Stored);
            const auto latest = request(60, source);
            CHECK(store.recordDecision(latest, false, "not_ready") == MotionWrite::Stored);
            lookup(store, query(clean), ResultQueryStatus::Known, true, "accepted", MotionOutcome::None, &clean);
            MotionStateStore reloaded;
            CHECK(reloaded.load(pairing()) == MotionLoad::Ready);
            for (size_t i = 0; i < history.size(); ++i) {
                auto q = query(history[i]);
                lookup(reloaded, q, ResultQueryStatus::Known, true, "accepted",
                    completed[i] ? MotionOutcome::Succeeded : MotionOutcome::Failed, &history[i]);
                std::strcpy(q.commandId, "other-id");
                lookup(reloaded, q, ResultQueryStatus::Conflict);
                q = query(history[i]);
                q.sequence = 99;
                lookup(reloaded, q, ResultQueryStatus::Conflict);
                char event[59];
                CHECK(makeProductEventId(pairing(), source, history[i].sequence, event));
                CHECK(store.acknowledge(event, completed[i], false) == MotionWrite::Stored);
                lookup(store, query(history[i]), ResultQueryStatus::Expired);
            }
            CHECK(store.finishOperation(execution, MotionOutcome::Interrupted, true) == MotionWrite::Stored);
            // An overwritten non-feeding outcome is not retained anywhere else.
            lookup(store, query(clean), ResultQueryStatus::Expired);
        });
    scenario("reused Cloud ID contradicts retained original identity", [] {
        MotionStateStore store;
        install(store);
        const auto old = request(8);
        CHECK(store.recordDecision(old, true, "accepted", execution) == MotionWrite::Stored);
        CHECK(store.finishFeeding(execution, true, "", "", 100) == MotionWrite::Stored);
        CHECK(store.archiveFeeding(true) == MotionWrite::Stored);
        auto reused = request(9);
        std::strcpy(reused.commandId, old.commandId);
        // Existing Store validates new sequence, not global historical ID reuse.
        CHECK(store.recordDecision(reused, false, "busy") == MotionWrite::Stored);
        lookup(store, query(old), ResultQueryStatus::Conflict);
        lookup(store, query(reused), ResultQueryStatus::Conflict);
    });
    for (auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch})
        scenario("current Intent and Terminal identity conflicts", [=] {
            MotionStateStore store;
            install(store);
            const auto old = request(8, source);
            CHECK(store.recordDecision(old, true, "accepted", execution) == MotionWrite::Stored);
            CHECK(store.recordDecision(request(10, source), false, "busy") == MotionWrite::Stored);
            for (bool terminal : {false, true}) {
                if (terminal) CHECK(store.finishFeeding(execution, false, "interrupted", "", 100) == MotionWrite::Stored);
                auto q = query(old);
                std::strcpy(q.commandId, "other");
                lookup(store, q, ResultQueryStatus::Conflict);
                q = query(old);
                q.sequence = 100;
                lookup(store, q, ResultQueryStatus::Conflict);
                q.sequence = 1;
                lookup(store, q, ResultQueryStatus::Conflict);
            }
        });
    scenario("legacy BMS1 journal queried without upgrading NVS", [] {
        MotionStateStore writer;
        install(writer);
        const auto old = request();
        CHECK(writer.recordDecision(old, true, "accepted", execution) == MotionWrite::Stored);
        CHECK(writer.finishFeeding(execution, true, "", "", 100) == MotionWrite::Stored);
        auto& blob = io.disk["productstate"]["record"].bytes;
        CHECK(blob.back() == 0);  // BMS2 empty pending-result count.
        blob.pop_back();
        detail::finishRecord(blob.data(), blob.size(), "BMS1");
        fake::reboot();
        MotionStateStore store;
        CHECK(store.load(pairing()) == MotionLoad::Ready);
        lookup(store, query(old), ResultQueryStatus::Known, true, "accepted", MotionOutcome::Succeeded, &old);
        noWrites();
    });
}
void watermarks() {
    scenario("empty verified history and independent sequences", [] {
        MotionStateStore store;
        install(store);
        lookup(store, query(request(1)), ResultQueryStatus::Unknown);
        lookup(store, query(request(1, v4::Source::LocalTouch)), ResultQueryStatus::Unknown);
        const auto cloud = request(900);
        CHECK(store.recordDecision(cloud, false, "busy") == MotionWrite::Stored);
        const auto local = request(2, v4::Source::LocalTouch);
        CHECK(store.recordDecision(local, false, "not_ready") == MotionWrite::Stored);
        lookup(store, query(cloud), ResultQueryStatus::Known, false, "busy", MotionOutcome::None, &cloud);
        lookup(store, query(local), ResultQueryStatus::Known, false, "not_ready", MotionOutcome::None, &local);
        for (auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch}) {
            const auto latest = source == v4::Source::CloudCommand ? cloud : local;
            auto q = query(latest);
            std::strcpy(q.commandId, "different");
            lookup(store, q, ResultQueryStatus::Conflict);
            for (uint64_t seq : {uint64_t(1), latest.sequence - 1, latest.sequence + 1, v4::kMaxSequence}) {
                q = query(latest);
                q.sequence = seq;
                lookup(store, q, ResultQueryStatus::Conflict);
                std::strcpy(q.commandId, "unretained-id");
                lookup(store, q, seq <= latest.sequence ? ResultQueryStatus::Expired : ResultQueryStatus::Unknown);
            }
        }
        auto otherSource = query(cloud);
        otherSource.source = v4::Source::LocalTouch;
        lookup(store, otherSource, ResultQueryStatus::Unknown);
        auto q = query(local);
        q.source = v4::Source::CloudCommand;
        lookup(store, q, ResultQueryStatus::Expired);
    });
    scenario("maximum watermark never wraps or invents acceptance", [] {
        MotionStateStore store;
        install(store);
        for (auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch}) {
            const auto r = request(v4::kMaxSequence, source);
            CHECK(store.recordDecision(r, false, "busy") == MotionWrite::Stored);
            lookup(store, query(r), ResultQueryStatus::Known, false, "busy", MotionOutcome::None, &r);
            lookup(store, query(request(1, source)), ResultQueryStatus::Expired);
            auto q = query(r);
            q.sequence = v4::kMaxSequence + 1;
            invalidLookup(store, q);
        }
    });
}
void stops() {
    for (const char* reason : {"accepted", "already_idle", "stale_execution", "storage_fault"})
        scenario(std::string("CloudStop cache ") + reason, [=] {
            MotionStateStore store;
            install(store);
            const auto old = request(2);
            CHECK(store.recordDecision(old, true, "accepted", execution) == MotionWrite::Stored);
            const bool accepted = !std::strcmp(reason, "accepted") || !std::strcmp(reason, "already_idle");
            CHECK(store.recordCloudStop(10, "cloud-stop", execution, accepted, reason, true) == MotionWrite::Stored);
            auto q = query(request(10));
            std::strcpy(q.commandId, "cloud-stop");
            lookup(store, q, ResultQueryStatus::Known, accepted, reason);
            lookup(store, query(old), ResultQueryStatus::Known, true, "accepted", MotionOutcome::None, &old);
            MotionStateStore reloaded;
            CHECK(reloaded.load(pairing()) == MotionLoad::Ready);
            lookup(reloaded, q, ResultQueryStatus::Known, accepted, reason);
            std::strcpy(q.commandId, "other-stop");
            lookup(store, q, ResultQueryStatus::Conflict);
            std::strcpy(q.commandId, "cloud-stop");
            q.sequence = 11;
            lookup(store, q, ResultQueryStatus::Conflict);
            q.source = v4::Source::LocalTouch;
            lookup(store, q, ResultQueryStatus::Unknown);
            q.source = v4::Source::CloudCommand;
            CHECK(store.recordDecision(request(12), false, "busy") == MotionWrite::Stored);
            q.sequence = 10;
            lookup(store, q, ResultQueryStatus::Expired);
        });
    scenario("idle stop and local seq zero invalid", [] {
        MotionStateStore store;
        install(store);
        CHECK(store.recordCloudStop(1, "idle-stop", nullptr, true, "already_idle", true) == MotionWrite::Stored);
        auto q = query(request(1));
        std::strcpy(q.commandId, "idle-stop");
        lookup(store, q, ResultQueryStatus::Known, true, "already_idle");
        q.source = v4::Source::LocalTouch;
        q.sequence = 0;
        invalidLookup(store, q);
    });
    scenario("CloudStop maximum escaped identity and reason", [] {
        auto p = pairing();
        std::memset(p.deviceId, 'D', 64);
        p.deviceId[64] = 0;
        MotionStateStore store;
        install(store, p);
        const std::string id(128, '\x01'), reason(64, 'R');
        CHECK(store.recordCloudStop(v4::kMaxSequence, id.c_str(), nullptr, false,
            reason.c_str(), true) == MotionWrite::Stored);
        auto q = query(request(v4::kMaxSequence, v4::Source::CloudCommand, ProductCommand::Clean, p));
        std::strcpy(q.commandId, id.c_str());
        lookup(store, q, ResultQueryStatus::Known, false, reason.c_str());
        MotionStateStore reloaded;
        CHECK(reloaded.load(p) == MotionLoad::Ready);
        lookup(reloaded, q, ResultQueryStatus::Known, false, reason.c_str());
    });
}
void faults() {
    scenario("unloaded and missing do not have zero watermarks", [] {
        MotionStateStore store;
        lookup(store, query(), ResultQueryStatus::StorageFault);
        CHECK(store.load(pairing()) == MotionLoad::Missing);
        lookup(store, query(), ResultQueryStatus::StorageFault);
        noWrites();
    });
    for (auto op : {Op::OpenRO, Op::Query, Op::Read})
        for (bool previouslyReady : {false, true})
            scenario("NVS IO fault including last-good history", [=] {
                MotionStateStore writer;
                install(writer);
                const auto r = request();
                CHECK(writer.recordDecision(r, false, "busy") == MotionWrite::Stored);
                fake::reboot();
                MotionStateStore store;
                if (previouslyReady) CHECK(store.load(pairing()) == MotionLoad::Ready);
                fake::fail(op, fake::count(op) + 1);
                CHECK(store.load(pairing()) == MotionLoad::IoError);
                CHECK(store.faulted());
                for (uint64_t seq : {uint64_t(1), r.sequence, uint64_t(999)}) {
                    auto q = query(r);
                    q.sequence = seq;
                    lookup(store, q, ResultQueryStatus::StorageFault);
                }
                if (previouslyReady) {
                    auto q = query();
                    std::strcpy(q.deviceId, "foreign-device");
                    invalidLookup(store, q);
                }
                noWrites();
            });
    for (bool previouslyReady : {false, true})
        for (bool wrongType : {false, true})
            scenario("corruption never appears unknown or expired", [=] {
                MotionStateStore writer;
                install(writer);
                CHECK(writer.recordDecision(request(), false, "busy") == MotionWrite::Stored);
                fake::reboot();
                MotionStateStore store;
                if (previouslyReady) CHECK(store.load(pairing()) == MotionLoad::Ready);
                auto& blob = io.disk["productstate"]["record"];
                if (wrongType) blob.type = fake::Type::String;
                else blob.bytes.back() ^= 1;
                CHECK(store.load(pairing()) == MotionLoad::Corrupt);
                lookup(store, query(), ResultQueryStatus::StorageFault);
                lookup(store, query(request(1)), ResultQueryStatus::StorageFault);
                noWrites();
            });
    scenario("digest corruption with repaired CRC still faults", [] {
        MotionStateStore writer;
        install(writer);
        CHECK(writer.recordDecision(request(), false, "busy") == MotionWrite::Stored);
        const auto digest = hexDigest(request());
        uint8_t bytes[kProductDigestSize];
        CHECK(requestDigest(request(), bytes));
        auto& blob = io.disk["productstate"]["record"].bytes;
        const auto at = std::search(blob.begin(), blob.end(), bytes, bytes + sizeof(bytes));
        CHECK(at != blob.end() && digest.size() == 64);
        *at ^= 1;
        detail::finishRecord(blob.data(), blob.size(), "BMS2");
        fake::reboot();
        MotionStateStore store;
        CHECK(store.load(pairing()) == MotionLoad::Corrupt);
        lookup(store, query(), ResultQueryStatus::StorageFault);
        noWrites();
    });
    for (auto op : {Op::Set, Op::Commit})
        scenario("latched uncertain write cannot expose last-good result", [=] {
            MotionStateStore store;
            install(store);
            const auto old = request();
            CHECK(store.recordDecision(old, false, "busy") == MotionWrite::Stored);
            fake::fail(op, fake::count(op) + 1, ESP_FAIL, true);
            CHECK(store.recordDecision(request(9), false, "not_ready") == MotionWrite::StorageFault);
            lookup(store, query(old), ResultQueryStatus::StorageFault);
            lookup(store, query(request(9)), ResultQueryStatus::StorageFault);
        });
    scenario("only already-verified snapshot is consulted", [] {
        MotionStateStore store;
        install(store);
        const auto r = request();
        CHECK(store.recordDecision(r, false, "busy") == MotionWrite::Stored);
        io.disk["productstate"]["record"].bytes.back() ^= 1;
        // Query cannot discover new Flash faults: the owner must load/check.
        lookup(store, query(r), ResultQueryStatus::Known, false, "busy", MotionOutcome::None, &r);
        CHECK(store.load(pairing()) == MotionLoad::Corrupt);
        lookup(store, query(r), ResultQueryStatus::StorageFault);
    });
    scenario("SHA unavailable during query does not recalculate or mutate", [] {
        MotionStateStore store;
        install(store);
        const auto r = request();
        CHECK(store.recordDecision(r, false, "busy") == MotionWrite::Stored);
        const auto expected = hexDigest(r);
        const auto before = io.calls.size();
        const auto calls = fake_product_crypto::calls;
        fake_product_crypto::fail = true;
        QueriedResult result;
        CHECK(queryMotionResult(store, query(r), result));
        CHECK(result.status == ResultQueryStatus::Known && expected == result.requestDigestHex);
        CHECK(io.calls.size() == before && fake_product_crypto::calls == calls && store.ready());
        roundtrip(result);
    });
}
void identities() {
    scenario("foreign and invalid query atomic rejection", [] {
        MotionStateStore store;
        install(store);
        auto q = query();
        std::strcpy(q.deviceId, "other-device");
        invalidLookup(store, q);
        for (auto mutate : std::vector<std::function<void(ResultQuery&)>>{
            [](ResultQuery& v) { v.sequence = 0; },
            [](ResultQuery& v) { v.sequence = v4::kMaxSequence + 1; },
            [](ResultQuery& v) { v.source = v4::Source(0); },
            [](ResultQuery& v) { v.source = v4::Source(3); },
            [](ResultQuery& v) { v.commandId[0] = 0; },
            [](ResultQuery& v) { v.deviceId[0] = 0; },
            [](ResultQuery& v) { v.deviceId[0] = '_'; },
            [](ResultQuery& v) { v.commandId[0] = char(0xff); },
            [](ResultQuery& v) { std::memset(v.commandId, 'a', sizeof(v.commandId)); },
            [](ResultQuery& v) { std::memset(v.deviceId, 'a', sizeof(v.deviceId)); }}) {
            q = query();
            mutate(q);
            CHECK(!validResultQuery(q) && !sameResultQuery(q, q));
            invalidLookup(store, q);
            v4::Message m;
            m.messageId = 17;
            const auto before = raw(m);
            CHECK(!encodeResultQuery(q, m) && raw(m) == before);
        }
        const auto original = query();
        CHECK(sameResultQuery(original, original));
        q = original;
        q.source = v4::Source::LocalTouch;
        CHECK(!sameResultQuery(original, q));
        q = original;
        ++q.sequence;
        CHECK(!sameResultQuery(original, q));
        q = original;
        std::strcpy(q.commandId, "other");
        CHECK(!sameResultQuery(original, q));
    });
}
void queryCodec() {
    for (auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch})
        for (uint64_t seq : {uint64_t(1), v4::kMaxSequence})
            scenario("query wire golden and transport fragments", [=] {
                auto q = query(request(seq, source));
                v4::Message m;
                CHECK(encodeResultQuery(q, m));
                CHECK(m.kind == v4::Kind::ResultQuery && !m.messageId && !m.senderBoot && !m.receiverBoot);
                CHECK(json(m) == "{\"device_id\":\"Babytech_01-test\",\"command_id\":\"" +
                    std::string(q.commandId) + "\",\"source\":\"" +
                    (source == v4::Source::CloudCommand ? "cloud_command" : "local_touch") +
                    "\",\"seq\":\"" + std::to_string(seq) + "\"}");
                ResultQuery decoded;
                CHECK(decodeResultQuery(m, decoded) && sameResultQuery(q, decoded));
                m.senderBoot = 1;
                m.receiverBoot = 2;
                m.messageId = 17;
                v4::Assembler assembler;
                v4::Message assembled;
                for (size_t offset = 0; offset < m.length; offset += v4::kMaxFragment) {
                    v4::Frame frame;
                    CHECK(v4::fragment(m, offset, frame));
                    uint8_t bytes[v4::kMaxFrame];
                    const auto length = v4::encode(frame, bytes, sizeof(bytes));
                    CHECK(length);
                    v4::Parser parser;
                    v4::Frame parsed;
                    bool parsedFrame = false;
                    for (size_t i = 0; i < length; ++i) parsedFrame |= parser.push(bytes[i], 100, parsed);
                    CHECK(parsedFrame);
                    const auto result = assembler.accept(parsed, 100, assembled);
                    CHECK(result == (offset + frame.length == m.length ? v4::AssemblyResult::Complete :
                        v4::AssemblyResult::Incomplete));
                }
                CHECK(decodeResultQuery(assembled, decoded) && sameResultQuery(q, decoded));
            });
    const std::string good = "{\"device_id\":\"D\",\"command_id\":\"C\",\"source\":\"cloud_command\",\"seq\":\"1\"}";
    auto reject = [&](const std::string& text) {
        ResultQuery output = query();
        const auto before = raw(output);
        CHECK(!decodeResultQuery(wire(text), output) && raw(output) == before);
        ++codecRejections;
    };
    scenario("strict malformed query grammar and field rejection", [&] {
        for (const char* text : {"", "[]", "{}", "null", "{device_id:'D'}", "{}{}", "/*x*/{}"}) reject(text);
        for (const char* seq : {"0", "01", "-1", "+1", "1.0", "1e0", " 1", "1 ", "",
                               "9223372036854775808", "18446744073709551615", "11111111111111111111"}) {
            auto text = good;
            text.replace(text.find("\"seq\":\"1\""), 9, "\"seq\":\"" + std::string(seq) + "\"");
            reject(text);
        }
        for (const char* value : {"1", "true", "false", "null", "{}", "[]", "1.0", "NaN", "Infinity"}) {
            auto text = good;
            text.replace(text.find("\"seq\":\"1\""), 9, "\"seq\":" + std::string(value));
            reject(text);
        }
        for (const char* key : {"device_id", "command_id", "source", "seq"}) {
            auto text = good;
            text.insert(text.size() - 1, ",\"" + std::string(key) + "\":\"1\"");
            reject(text);
            text = good;
            text.replace(text.find(key), std::strlen(key), "unknown_field");
            reject(text);
        }
        for (const char* extra : {",", ",\"extra\":1", ",\"s\\u0065q\":\"1\""}) {
            auto text = good;
            text.insert(text.size() - 1, extra);
            reject(text);
        }
        for (const char* id : {"", "\\u0000", "\\ud800", "\\udc00", "\\ud800\\u0041", "\\x01"}) {
            auto text = good;
            text.replace(text.find("\"command_id\":\"C\""), 16, "\"command_id\":\"" + std::string(id) + "\"");
            reject(text);
        }
        for (const char* source : {"cloud", "local", "Cloud_command", ""}) {
            auto text = good;
            text.replace(text.find("cloud_command"), 13, source);
            reject(text);
        }
        for (size_t length = 0; length < good.size(); ++length) reject(good.substr(0, length));
        reject(good + "x");
        auto text = good;
        text[text.find('C')] = '\x01';
        reject(text);
        text[text.find('\x01')] = char(0xff);
        reject(text);
        ResultQuery output = query();
        const auto before = raw(output);
        auto m = wire(good, v4::Kind::Result);
        CHECK(!decodeResultQuery(m, output) && raw(output) == before);
        m.kind = v4::Kind::ResultQuery;
        m.length = v4::kMaxMessage + 1;
        CHECK(!decodeResultQuery(m, output) && raw(output) == before);
        m = wire(good + std::string(v4::kMaxMessage - good.size(), ' '));
        CHECK(decodeResultQuery(m, output) && output.sequence == 1);
        m = wire(" \r\n\t{\"seq\":\"1\",\"source\":\"local_touch\",\"command_id\":\"C\",\"device_id\":\"D\"} \t");
        CHECK(decodeResultQuery(m, output) && output.source == v4::Source::LocalTouch);
    });
}
void resultCodec() {
    scenario("all canonical statuses and atomic invalid results", [] {
        QueriedResult base;
        base.query = query();
        base.status = ResultQueryStatus::Known;
        base.accepted = true;
        std::strcpy(base.reason, "accepted");
        std::strcpy(base.requestDigestHex, hexDigest(request()).c_str());
        for (auto outcome : {MotionOutcome::None, MotionOutcome::Succeeded, MotionOutcome::Interrupted, MotionOutcome::Failed}) {
            base.outcome = outcome;
            roundtrip(base);
        }
        auto reject = [&](const QueriedResult& result) {
            v4::Message output;
            output.senderBoot = 11;
            output.length = 99;
            std::memset(output.payload, 0xa5, sizeof(output.payload));
            const auto before = raw(output);
            CHECK(!encodeQueriedResult(result, output) && raw(output) == before);
            ++codecRejections;
        };
        for (auto status : {ResultQueryStatus::Unknown, ResultQueryStatus::Expired,
                            ResultQueryStatus::Conflict, ResultQueryStatus::StorageFault}) {
            QueriedResult r;
            r.query = query();
            r.status = status;
            const char* reason = status == ResultQueryStatus::Unknown ? "unknown" :
                status == ResultQueryStatus::Expired ? "result_expired" :
                status == ResultQueryStatus::Conflict ? "request_conflict" : "storage_fault";
            std::strcpy(r.reason, reason);
            roundtrip(r);
            auto bad = r;
            bad.accepted = true;
            reject(bad);
            bad = r;
            bad.outcome = MotionOutcome::Failed;
            reject(bad);
            bad = r;
            std::strcpy(bad.requestDigestHex, base.requestDigestHex);
            reject(bad);
            bad = r;
            std::strcpy(bad.reason, "busy");
            reject(bad);
        }
        base.outcome = MotionOutcome::None;
        for (auto mutate : std::vector<std::function<void(QueriedResult&)>>{
            [](QueriedResult& r) { r.status = ResultQueryStatus(5); },
            [](QueriedResult& r) { r.outcome = MotionOutcome(4); },
            [](QueriedResult& r) { r.query.sequence = 0; },
            [](QueriedResult& r) { r.reason[0] = 0; },
            [](QueriedResult& r) { r.reason[0] = '-'; },
            [](QueriedResult& r) { r.reason[0] = char(0xff); },
            [](QueriedResult& r) { std::memset(r.reason, 'a', sizeof(r.reason)); },
            [](QueriedResult& r) { r.accepted = false; },
            [](QueriedResult& r) { std::strcpy(r.reason, "already_idle"); },
            [](QueriedResult& r) { std::strcpy(r.reason, "already_clear"); r.outcome = MotionOutcome::Succeeded; },
            [](QueriedResult& r) { r.requestDigestHex[0] = 'G'; },
            [](QueriedResult& r) { r.requestDigestHex[0] = 'A'; },
            [](QueriedResult& r) { r.requestDigestHex[63] = 0; },
            [](QueriedResult& r) { r.requestDigestHex[64] = 'a'; },
            [](QueriedResult& r) { r.requestDigestHex[0] = 0; r.query.source = v4::Source::LocalTouch; },
            [](QueriedResult& r) { r.requestDigestHex[0] = 0; r.outcome = MotionOutcome::Failed; },
            [](QueriedResult& r) { r.requestDigestHex[0] = 0; std::strcpy(r.reason, "already_clear"); },
            [](QueriedResult& r) { r.accepted = false; std::strcpy(r.reason, "busy"); r.outcome = MotionOutcome::Succeeded; }}) {
            auto bad = base;
            mutate(bad);
            reject(bad);
        }
        auto r = base;
        r.accepted = false;
        std::strcpy(r.reason, "busy");
        roundtrip(r);
        std::memset(r.reason, 'R', 64);
        r.reason[64] = 0;
        roundtrip(r);
    });
    scenario("result decoder strict types status digest and fields", [] {
        QueriedResult original;
        original.query = query();
        original.status = ResultQueryStatus::Known;
        original.accepted = true;
        std::strcpy(original.reason, "accepted");
        std::strcpy(original.requestDigestHex, hexDigest(request()).c_str());
        v4::Message m;
        CHECK(encodeQueriedResult(original, m));
        const auto good = json(m);
        auto reject = [&](const std::string& text) {
            auto output = original;
            const auto before = raw(output);
            CHECK(!decodeQueriedResult(wire(text, v4::Kind::Result), output) && raw(output) == before);
            ++codecRejections;
        };
        for (const char* field : {"device_id", "command_id", "source", "seq", "status", "accepted", "reason", "outcome", "request_digest"}) {
            auto text = good;
            text.insert(text.size() - 1, ",\"" + std::string(field) + "\":null");
            reject(text);
            text = good;
            text.replace(text.find(field), std::strlen(field), "unknown_field");
            reject(text);
        }
        for (const char* value : {"1", "0", "\"true\"", "null", "{}", "[]"}) {
            auto text = good;
            text.replace(text.find("\"accepted\":true"), 15, "\"accepted\":" + std::string(value));
            reject(text);
        }
        for (const char* value : {"unknown", "result_expired", "request_conflict", "storage_fault", "KNOWN", "nonsense", ""}) {
            auto text = good;
            text.replace(text.find("\"status\":\"known\""), 16, "\"status\":\"" + std::string(value) + "\"");
            reject(text);
        }
        for (const char* value : {"unknown", "succeeded_", "", "None"}) {
            auto text = good;
            text.replace(text.find("\"outcome\":\"none\""), 16, "\"outcome\":\"" + std::string(value) + "\"");
            reject(text);
        }
        for (const char* field : {"device_id", "command_id", "source", "seq", "status", "reason", "outcome", "request_digest"}) {
            for (const char* value : {"null", "true", "1", "{}", "[]"}) {
                auto text = good;
                const auto start = text.find("\"" + std::string(field) + "\":");
                CHECK(start != std::string::npos);
                const auto valueStart = start + std::strlen(field) + 3;
                const auto end = text.find('"', valueStart + 1);
                CHECK(end != std::string::npos);
                text.replace(valueStart, end - valueStart + 1, value);
                reject(text);
            }
        }
        auto text = good;
        text.insert(text.size() - 1, ",\"request_\\u0064igest\":\"\"");
        reject(text);
        for (size_t n = 0; n < good.size(); ++n) reject(good.substr(0, n));
        text = good;
        text[text.find(original.requestDigestHex)] = 'A';
        reject(text);
        text = good;
        text.replace(text.find("accepted\""), 9, "busy\"");
        reject(text);
        auto output = original;
        const auto before = raw(output);
        m.kind = v4::Kind::ResultQuery;
        CHECK(!decodeQueriedResult(m, output) && raw(output) == before);
        m.kind = v4::Kind::Result;
        m.length = v4::kMaxMessage + 1;
        CHECK(!decodeQueriedResult(m, output) && raw(output) == before);
        m = wire(good + std::string(v4::kMaxMessage - good.size(), ' '), v4::Kind::Result);
        CHECK(decodeQueriedResult(m, output));
    });
}
void boundaries() {
    for (const std::string& command : {std::string(128, 'a'), std::string(128, '\x01'),
                                      std::string(128, '"'), std::string(128, '\\'),
                                      [] { std::string s; for (int i = 0; i < 32; ++i) s += "\xf0\x9f\x98\x80"; return s; }()})
        scenario("max identity escaped UTF8 production store", [&] {
            auto p = pairing();
            std::memset(p.deviceId, 'D', 64);
            p.deviceId[64] = 0;
            MotionStateStore store;
            install(store, p);
            auto r = request(v4::kMaxSequence, v4::Source::CloudCommand, ProductCommand::Prepare, p);
            std::memcpy(r.commandId, command.data(), command.size());
            r.commandId[command.size()] = 0;
            const std::string reason(64, 'R');
            CHECK(store.recordDecision(r, false, reason.c_str()) == MotionWrite::Stored);
            auto q = query(r);
            v4::Message m;
            CHECK(encodeResultQuery(q, m));
            for (size_t i = 0; i < m.length; ++i) CHECK(m.payload[i] >= 0x20);
            CHECK(m.length <= v4::kMaxMessage);
            ResultQuery decoded;
            CHECK(decodeResultQuery(m, decoded) && sameResultQuery(q, decoded));
            lookup(store, q, ResultQueryStatus::Known, false, reason.c_str(), MotionOutcome::None, &r);
            MotionStateStore reloaded;
            CHECK(reloaded.load(p) == MotionLoad::Ready);
            lookup(reloaded, q, ResultQueryStatus::Known, false, reason.c_str(), MotionOutcome::None, &r);
        });
    scenario("Unicode escape decoded length boundaries and malformed UTF8", [] {
        const auto make = [](const std::string& id, const std::string& device = "D") {
            return "{\"device_id\":\"" + device + "\",\"command_id\":\"" + id +
                "\",\"source\":\"cloud_command\",\"seq\":\"1\"}";
        };
        std::string escaped;
        for (int i = 0; i < 32; ++i) escaped += "\\ud83d\\ude00";
        ResultQuery output;
        CHECK(decodeResultQuery(wire(make(escaped, std::string(64, 'D'))), output));
        CHECK(std::strlen(output.commandId) == 128 && std::strlen(output.deviceId) == 64);
        const auto reject = [&](const std::string& text) {
            const auto before = raw(output);
            CHECK(!decodeResultQuery(wire(text), output) && raw(output) == before);
            ++codecRejections;
        };
        reject(make(escaped + "a"));
        reject(make(std::string(129, 'a')));
        reject(make("id", std::string(65, 'D')));
        reject(make("id", "_D"));
        reject(make("id", "\\u00e9"));
        for (const auto& id : std::vector<std::string>{"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82", "\x80"})
            reject(make(id));
        CHECK(decodeResultQuery(wire(make("\\u0001\\n\\\"\\\\")), output));
        CHECK(std::string(output.commandId) == "\x01\n\"\\");
        v4::Message encoded;
        CHECK(encodeResultQuery(output, encoded) && json(encoded).find("\\u0001") != std::string::npos);
        for (unsigned c = 1; c < 0x20; ++c) {
            std::memset(output.commandId, int(c), 128);
            output.commandId[128] = 0;
            CHECK(encodeResultQuery(output, encoded));
            ResultQuery decoded;
            CHECK(decodeResultQuery(encoded, decoded) && sameResultQuery(output, decoded));
        }
    });
}
}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::pair<const char*, std::function<void()>>> groups = {
        {"decisions", decisions}, {"retained", retained}, {"watermarks", watermarks},
        {"stops", stops}, {"faults", faults}, {"identities", identities},
        {"query_codec", queryCodec}, {"result_codec", resultCodec}, {"boundaries", boundaries}};
    bool found = argc == 1;
    for (const auto& group : groups) {
        if (argc > 1 && std::strcmp(argv[1], group.first)) continue;
        found = true;
        const auto before = failures;
        group.second();
        std::printf("%s: %s\n", group.first, failures == before ? "PASS" : "FAIL");
    }
    if (!found || argc > 2) { std::fprintf(stderr, "Unknown test group\n"); return 2; }
    std::printf("%u scenarios, %u read-only lookups, %u atomic codec rejections, %u failures\n",
                scenarios, lookups, codecRejections, failures);
    return failures ? 1 : 0;
}
