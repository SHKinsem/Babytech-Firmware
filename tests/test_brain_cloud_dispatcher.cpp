#include "brain_cloud_dispatcher.h"
#include "BrainStateStore.h"
#include "FakeBrainNvs.h"
#include "FakeProductCrypto.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

using namespace babytech::boardlink;
namespace v4 = babytech::v4;
namespace cloud = babytech::cloud;
namespace nvs = fake_brain;
namespace crypto = fake_product_crypto;

namespace {
constexpr char device[] = "Babytech_dispatcher-test";
constexpr char sessionA[] = "0123456789abcdef0123456789abcdef";
constexpr char sessionB[] = "abcdef0123456789abcdef0123456789";
constexpr char executionA[] = "11111111111111111111111111111111";
constexpr char executionB[] = "22222222222222222222222222222222";
uint32_t nowMs = 100;
unsigned clockCalls = 0, admissionCalls = 0, scenarios = 0, failures = 0;
const char* blocked = nullptr;
std::function<void()> clockHook;
std::function<const char*()> admissionHook;
std::function<void(const ProductRequest&, uint32_t)> acceptanceHook;

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    "line " + std::to_string(__LINE__) + ": " #condition); } while (false)

uint32_t clockNow() { ++clockCalls; if (clockHook) clockHook(); return nowMs; }
const char* admission() { ++admissionCalls; return admissionHook ? admissionHook() : blocked; }
void observeAcceptance(const ProductRequest& request, uint32_t at) {
    CHECK(acceptanceHook); acceptanceHook(request, at);
}

void scenario(const std::string& name, const std::function<void()>& run, bool usesNvs = false) {
    ++scenarios;
    nvs::reset(); crypto::reset(); nowMs = 100; clockCalls = admissionCalls = 0;
    blocked = nullptr; clockHook = {}; admissionHook = {}; acceptanceHook = {};
    try {
        run();
        if (!usesNvs) CHECK(nvs::io.calls.empty());
        std::printf("PASS %s\n", name.c_str());
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "FAIL %s: %s\n", name.c_str(), error.what());
    }
    acceptanceHook = {};
}

struct Ack {
    std::string id, name, session, reason;
    uint64_t sequence;
    bool accepted, sent;
};
struct FreshCheck {
    std::string session;
    uint32_t generation, sample, at;
    uint16_t ttl;
    cloud::Freshness result;
};
// Only network boundaries are replaced; freshness remains production logic.
struct FakeNetwork {
    cloud::CloudSession session;
    bool publishAvailable = true;
    std::vector<Ack> acks;
    std::vector<FreshCheck> checks;
    std::function<void(size_t)> beforeCheck;
    FakeNetwork(uint32_t opened = 0) { CHECK(session.open(sessionA, 7, opened)); }
    cloud::Freshness checkFreshness(const char* id, uint32_t generation, uint32_t sample, uint16_t ttl) {
        if (beforeCheck) beforeCheck(checks.size());
        const auto result = session.check(id, generation, sample, ttl, nowMs);
        checks.push_back({id, generation, sample, nowMs, ttl, result});
        return result;
    }
    bool publishAck(const char* id, const char* name, uint64_t seq, const char* originalSession,
                    bool accepted, const char* reason) {
        acks.push_back({id, name, originalSession, reason, seq, accepted, publishAvailable});
        return publishAvailable;
    }
};

CloudCommand command(ProductCommand kind = ProductCommand::Prepare, uint64_t seq = 1,
                     uint32_t sample = 100) {
    CloudCommand c;
    std::strcpy(c.session, sessionA); c.sampledAtMs = sample; c.ttlMs = 5000;
    auto& r = c.request;
    r.source = v4::Source::CloudCommand; r.command = kind; r.sequence = seq;
    std::strcpy(r.deviceId, device);
    std::snprintf(r.commandId, sizeof(r.commandId), "cloud-%llu", static_cast<unsigned long long>(seq));
    if (kind == ProductCommand::Prepare) {
        std::strcpy(r.babyId, "original-baby"); r.profileVersion = 10;
        r.waterMl = 120; r.temperatureC = 40; r.powderGPer100Ml = 13.5f;
    } else if (kind == ProductCommand::SetTargetTemp) r.temperatureC = 40;
    CHECK(validProductRequest(r));
    return c;
}
CloudStop stop(uint64_t seq = 2, uint32_t sample = 100) {
    CloudStop s;
    std::strcpy(s.deviceId, device); std::strcpy(s.session, sessionA);
    std::snprintf(s.commandId, sizeof(s.commandId), "stop-%llu", static_cast<unsigned long long>(seq));
    s.sequence = seq; s.sampledAtMs = sample; s.ttlMs = 5000;
    return s;
}
CommandResult result(const ProductRequest& r, bool accepted = true) {
    CommandResult reply;
    reply.source = r.source; reply.sequence = r.sequence;
    std::strcpy(reply.commandId, r.commandId); reply.accepted = accepted;
    std::strcpy(reply.reason, accepted ? "accepted" : "not_ready");
    return reply;
}
void digestText(const ProductRequest& r, char (&out)[65]) {
    uint8_t bytes[kProductDigestSize]; CHECK(requestDigest(r, bytes));
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        out[2 * i] = hex[bytes[i] >> 4]; out[2 * i + 1] = hex[bytes[i] & 15];
    }
    out[64] = 0;
}
QueriedResult known(const ProductRequest& r, bool accepted = true) {
    QueriedResult q;
    q.query.source = r.source; q.query.sequence = r.sequence;
    std::strcpy(q.query.deviceId, r.deviceId); std::strcpy(q.query.commandId, r.commandId);
    q.status = ResultQueryStatus::Known; q.accepted = accepted;
    std::strcpy(q.reason, accepted ? "accepted" : "not_ready");
    digestText(r, q.requestDigestHex);
    return q;
}

// Precise state faults only. All valid traffic still crosses production codecs.
struct FakeLink {
    bool board = true, allowCommand = true, allowQuery = true, allowStop = true;
    bool telemetry = true, telemetryFresh = true;
    uint32_t telemetryReceivedAt = 100;
    Status status;
    CommandSendState send = CommandSendState::Idle;
    ResultLookupState lookup = ResultLookupState::Idle;
    StopSendState stopState = StopSendState::Idle;
    CommandResult reply;
    QueriedResult queried;
    std::vector<CommandMessage> commands;
    std::vector<uint32_t> sentAt;
    std::vector<ResultQuery> queries;
    std::vector<uint32_t> queryAt;
    std::vector<v4::StopRequest> stops;
    unsigned cancellations = 0, queryCancellations = 0, queryAttempts = 0;
    FakeLink() { status.stationary = true; }
    bool connected(uint32_t) const { return board; }
    const Status* lastTelemetry() const { return telemetry && telemetryFresh ? &status : nullptr; }
    uint32_t lastTelemetryReceivedAtMs() const { return telemetryReceivedAt; }
    bool requestCommand(const CommandMessage& c, uint32_t at) {
        if (!board || !allowCommand || send == CommandSendState::Pending) return false;
        v4::Message wire; CommandMessage decoded;
        CHECK(encodeCommand(c, wire) && decodeCommand(wire, decoded));
        commands.push_back(decoded); sentAt.push_back(at); send = CommandSendState::Pending;
        return true;
    }
    CommandSendState commandSendState() const { return send; }
    const CommandResult& commandResponse() const { return reply; }
    void cancelCommand() { ++cancellations; send = CommandSendState::Cancelled; }
    void complete(const CommandResult& r) {
        v4::Message wire; CHECK(encodeCommandResult(r, wire) && decodeCommandResult(wire, reply));
        send = CommandSendState::Complete;
    }
    bool requestResult(const ResultQuery& q, uint32_t at) {
        ++queryAttempts;
        if (!board || !allowQuery || lookup == ResultLookupState::Pending) return false;
        v4::Message wire; ResultQuery decoded;
        CHECK(encodeResultQuery(q, wire) && decodeResultQuery(wire, decoded));
        queries.push_back(decoded); queryAt.push_back(at); lookup = ResultLookupState::Pending;
        return true;
    }
    ResultLookupState resultLookupState() const { return lookup; }
    const QueriedResult& resultQueryResponse() const { return queried; }
    void cancelResultQuery() { ++queryCancellations; lookup = ResultLookupState::Idle; }
    void completeQuery(const QueriedResult& q, bool codec = true) {
        if (codec) {
            v4::Message wire; CHECK(encodeQueriedResult(q, wire) && decodeQueriedResult(wire, queried));
        } else queried = q; // Deliberately corrupt an already-decoded boundary.
        lookup = ResultLookupState::Complete;
    }
    bool requestStop(const v4::StopRequest& s, uint32_t) {
        if (!board || !allowStop || stopState == StopSendState::Pending) return false;
        uint8_t wire[v4::kMaxFragment]; v4::StopRequest decoded;
        const auto length = v4::encodeStop(s, wire, sizeof(wire));
        CHECK(length && v4::decodeStop(wire, length, decoded));
        stops.push_back(decoded); cancelCommand(); stopState = StopSendState::Pending;
        return true;
    }
    StopSendState stopSendState() const { return stopState; }
};
using Dispatcher = babytech::brain::BrainCloudDispatcher<FakeLink, FakeNetwork>;
static_assert(!std::is_copy_constructible<Dispatcher>::value, "one dispatcher owner");
struct Rig {
    FakeLink link;
    FakeNetwork net;
    Dispatcher d;
    Rig() : d(link, net, clockNow, admission) {}
    void start(const CloudCommand& c = command()) { d.command(c, 7, nowMs); }
    void poll(uint32_t at) { nowMs = at; d.poll(at); }
    void resolve(const ProductRequest& r, bool accepted = true) {
        link.complete(result(r, accepted)); d.poll(nowMs);
        CHECK(!d.busy() && d.resultPending()); d.poll(nowMs);
    }
    void query(const CloudCommand& c) {
        start(c); CHECK(d.busy()); link.send = CommandSendState::TimedOut;
        poll(101); poll(1100); CHECK(link.queries.empty()); poll(1101);
        CHECK(link.queries.size() == 1 && d.busy());
    }
};
void ack(const Ack& a, const CloudCommand& c, bool accepted, const char* reason) {
    CHECK(a.id == c.request.commandId && a.sequence == c.request.sequence);
    CHECK(a.session == c.session && a.accepted == accepted && a.reason == reason);
}
void stopAck(const Ack& a, const CloudStop& s, bool accepted, const char* reason) {
    CHECK(a.id == s.commandId && a.name == "stop" && a.sequence == s.sequence);
    CHECK(a.session == s.session && a.accepted == accepted && a.reason == reason);
}

ResultQuery stopIdentity(const CloudStop& s) {
    ResultQuery q;
    q.source = v4::Source::CloudCommand; q.sequence = s.sequence;
    std::strcpy(q.deviceId, s.deviceId); std::strcpy(q.commandId, s.commandId);
    return q;
}
QueriedResult knownStop(const CloudStop& s, bool accepted = true, const char* reason = "already_idle") {
    QueriedResult q; q.query = stopIdentity(s); q.status = ResultQueryStatus::Known;
    q.accepted = accepted; std::strcpy(q.reason, reason); return q;
}
void beginStopQuery(Rig& r, const CloudStop& s = stop(), StopSendState state = StopSendState::TimedOut) {
    r.d.stop(s, 7, nowMs);
    CHECK(r.link.stops.size() == 1 && !r.d.busy());
    r.link.stopState = state; r.poll(101);
    CHECK(!r.d.busy() && r.net.acks.empty());
    r.poll(1100); CHECK(r.link.queries.empty()); r.poll(1101);
    CHECK(r.d.busy() && r.link.queries.size() == 1 && r.link.queryAt[0] == 1101);
    CHECK(sameResultQuery(r.link.queries[0], stopIdentity(s)));
}

void stopRecoveryFaults() {
    for (auto state : {StopSendState::TimedOut, StopSendState::Unavailable})
        scenario("Stop recovery initial uncertainty / " + std::to_string(unsigned(state)), [=] {
            Rig r; const auto s = stop(); beginStopQuery(r, s, state);
            for (uint32_t at = 1102; at < 1200; ++at) { r.poll(at); r.d.stop(s, 7, nowMs); }
            CHECK(r.d.busy() && r.link.queries.size() == 1 && r.link.stops.size() == 1 && r.net.acks.empty());
            r.link.completeQuery(knownStop(s)); r.poll(1200); r.poll(10000);
            CHECK(!r.d.busy() && !r.d.resultPending() && r.net.acks.size() == 1);
            stopAck(r.net.acks[0], s, true, "already_idle");
            CHECK(r.link.stops.size() == 1 && r.link.queryCancellations == 1);
        });
    for (const char* reason : {"already_idle", "accepted", "stop_rejected"})
        scenario(std::string("Stop recovery exact original reason/session / ") + reason, [=] {
            Rig r; auto s = stop();
            if (!std::strcmp(reason, "accepted")) {
                std::strcpy(r.link.status.activeExecutionId, executionA);
                r.link.status.motionBusy = true; r.link.status.stationary = false;
            }
            beginStopQuery(r, s);
            // Recovery is informational: no fresh Cloud session, STATUS or admission required.
            r.net.session.close(); r.link.telemetry = false; blocked = "maintenance";
            r.net.publishAvailable = false;
            const bool accepted = std::strcmp(reason, "stop_rejected") != 0;
            r.link.completeQuery(knownStop(s, accepted, reason)); r.poll(6000);
            CHECK(!r.d.busy() && r.net.acks.size() == 1 && !r.net.acks[0].sent);
            // Copy the reason before canceling/reusing the shared query-response buffer.
            std::strcpy(r.link.queried.reason, "overwritten");
            CHECK(r.net.session.open(sessionB, 8, nowMs)); r.poll(6001);
            r.net.publishAvailable = true; r.poll(6002); r.poll(10000);
            CHECK(r.net.acks.size() == 3 && r.net.acks.back().sent);
            for (const auto& a : r.net.acks) stopAck(a, s, accepted, reason);
            CHECK(r.link.stops.size() == 1 && r.link.queries.size() == 1 && !admissionCalls);
        });
    for (unsigned status = 1; status <= 4; ++status) for (bool forged : {false, true})
        scenario("Stop recovery uncertain lookup never ACK / " + std::to_string(status) +
                 (forged ? " forged acceptance" : " valid codec"), [=] {
            Rig r; const auto s = stop(); beginStopQuery(r, s);
            auto q = knownStop(s, forged); q.status = ResultQueryStatus(status);
            const char* reasons[] = {"known", "unknown", "result_expired", "request_conflict", "storage_fault"};
            std::strcpy(q.reason, reasons[status]); r.link.completeQuery(q, !forged); r.poll(1102);
            CHECK(!r.d.busy() && r.net.acks.empty() && r.link.queryCancellations == 1);
            for (uint32_t at = 1103; at < 2102; ++at) r.poll(at);
            CHECK(r.link.queryAttempts == 1); r.poll(2102);
            CHECK(r.d.busy() && r.link.queries.size() == 2 && r.link.queryAt.back() == 2102);
            CHECK(sameResultQuery(r.link.queries.back(), stopIdentity(s)));
            r.link.completeQuery(knownStop(s)); r.poll(2103);
            CHECK(r.net.acks.size() == 1 && r.link.stops.size() == 1);
            stopAck(r.net.acks[0], s, true, "already_idle");
        });
    for (unsigned fault = 0; fault < 5; ++fault)
        scenario("Stop recovery rejects mismatched identity or ordinary digest / " + std::to_string(fault), [=] {
            Rig r; const auto s = stop(); beginStopQuery(r, s); auto q = knownStop(s, true, "accepted");
            if (fault == 0) q.query.source = v4::Source::LocalTouch;
            if (fault == 1) std::strcpy(q.query.deviceId, "Babytech_foreign");
            if (fault == 2) ++q.query.sequence;
            if (fault == 3) std::strcpy(q.query.commandId, "another-stop");
            if (fault == 4) digestText(command().request, q.requestDigestHex);
            r.link.completeQuery(q, fault != 0); r.poll(1102);
            CHECK(!r.d.busy() && r.net.acks.empty()); r.poll(2102);
            CHECK(r.link.queries.size() == 2 && sameResultQuery(r.link.queries.back(), stopIdentity(s)));
            r.link.completeQuery(knownStop(s)); r.poll(2103);
            CHECK(r.net.acks.size() == 1 && r.link.stops.size() == 1);
        });
    for (auto outcome : {MotionOutcome::Succeeded, MotionOutcome::Interrupted, MotionOutcome::Failed})
        scenario("Stop recovery rejects non-None outcome / " + std::to_string(unsigned(outcome)), [=] {
            Rig r; const auto s = stop(); beginStopQuery(r, s); auto q = knownStop(s, true, "accepted");
            q.outcome = outcome;
            // The production codec rejects this shape; inject a decoded-boundary fault explicitly.
            v4::Message wire; CHECK(!encodeQueriedResult(q, wire));
            r.link.completeQuery(q, false); r.poll(1102);
            CHECK(!r.d.busy() && r.net.acks.empty() && r.link.stops.size() == 1);
        });
    for (auto state : {ResultLookupState::Idle, ResultLookupState::TimedOut, ResultLookupState::Unavailable})
        scenario("Stop recovery query transport fault retries only query / " + std::to_string(unsigned(state)), [=] {
            Rig r; const auto s = stop(); beginStopQuery(r, s); r.link.lookup = state; r.poll(1102);
            CHECK(!r.d.busy() && r.net.acks.empty()); r.poll(2101);
            CHECK(r.link.queryAttempts == 1); r.poll(2102);
            CHECK(r.link.queries.size() == 2 && r.link.stops.size() == 1);
            CHECK(sameResultQuery(r.link.queries.back(), stopIdentity(s)));
        });
    for (bool disconnected : {false, true})
        scenario(disconnected ? "Stop recovery disconnected retry bounded" : "Stop recovery unavailable query slot retry bounded", [=] {
            Rig r; const auto s = stop(); r.d.stop(s, 7, nowMs);
            r.link.stopState = StopSendState::Unavailable; r.poll(101);
            if (disconnected) r.link.board = false; else r.link.allowQuery = false;
            r.poll(1101); CHECK(!r.d.busy() && r.link.queryAttempts == 1 && r.link.queries.empty());
            for (uint32_t at = 1102; at < 2101; ++at) r.poll(at);
            CHECK(r.link.queryAttempts == 1 && !r.link.queryCancellations);
            r.link.board = r.link.allowQuery = true; r.link.telemetry = false;
            r.poll(2101); CHECK(r.d.busy() && r.link.queries.size() == 1);
            r.link.completeQuery(knownStop(s)); r.poll(2102);
            CHECK(r.net.acks.size() == 1 && r.link.stops.size() == 1);
        });
    for (bool maximumId : {false, true})
        scenario(maximumId ? "Stop recovery full identity capacity" : "Stop recovery retry timer wraps uint32", [=] {
            const uint32_t sample = maximumId ? 100u : UINT32_MAX - 100u;
            nowMs = sample; FakeLink link; FakeNetwork net(sample - 1); Dispatcher d(link, net, clockNow, admission);
            auto s = stop(maximumId ? v4::kMaxSequence : 2, sample);
            if (maximumId) { std::memset(s.commandId, 's', 128); s.commandId[128] = 0; }
            d.stop(s, 7, nowMs); CHECK(link.stops.size() == 1);
            link.stopState = StopSendState::TimedOut; d.poll(nowMs);
            nowMs = sample + 999u; d.poll(nowMs); CHECK(link.queries.empty() && !d.busy());
            nowMs = sample + 1000u; d.poll(nowMs); CHECK(d.busy() && link.queries.size() == 1);
            CHECK(sameResultQuery(link.queries[0], stopIdentity(s)));
            link.completeQuery(knownStop(s)); ++nowMs; d.poll(nowMs);
            CHECK(!d.busy() && net.acks.size() == 1 && link.stops.size() == 1);
            stopAck(net.acks[0], s, true, "already_idle");
        });
}

void stopRecoveryArbitration() {
    scenario("local operation yields only informational Stop query", [] {
        Rig r; const auto s = stop(); beginStopQuery(r, s);
        CHECK(r.d.busy() && !r.d.ordinaryBusy());
        CHECK(r.d.yieldToLocal(nowMs));
        CHECK(!r.d.busy() && r.link.queryCancellations == 1 && r.net.acks.empty());
        CHECK(r.link.stops.size() == 1 && r.link.commands.empty());
        CHECK(r.d.yieldToLocal(nowMs) && r.link.queryCancellations == 1);
    });
    scenario("local operation cannot cancel Cloud acceptance owner", [] {
        Rig r; const auto c = command(); r.query(c);
        CHECK(r.d.ordinaryBusy() && !r.d.yieldToLocal(nowMs));
        CHECK(r.link.queryCancellations == 0 && r.link.commands.size() == 1);
    });
    scenario("local operation waits for actual priority Stop transport", [] {
        Rig r; r.d.stop(stop(), 7, nowMs);
        CHECK(!r.d.ordinaryBusy() && !r.d.yieldToLocal(nowMs));
        CHECK(!r.link.queryCancellations && r.link.stopState == StopSendState::Pending);
    });
    for (const char* owner : {"durable local pending", "maintenance"})
        scenario(std::string("Stop recovery defers to external owner / ") + owner, [] {
            Rig r; const auto s = stop(); r.d.stop(s, 7, nowMs);
            r.link.stopState = StopSendState::TimedOut; r.poll(101);
            const auto external = known(command(ProductCommand::Clean, 9).request).query;
            CHECK(r.link.requestResult(external, nowMs));
            for (uint32_t at = 1101; at < 2101; ++at) { nowMs = at; r.d.poll(at, true); }
            CHECK(!r.d.busy() && r.link.queryAttempts == 1 && !r.link.queryCancellations);
            CHECK(r.link.lookup == ResultLookupState::Pending && r.net.acks.empty());
            // Even without the ownership hint, failed admission never cancels someone else's query.
            r.poll(2101); CHECK(r.link.queryAttempts == 2 && !r.link.queryCancellations && !r.d.busy());
            r.link.cancelResultQuery(); r.poll(3100); CHECK(r.link.queryAttempts == 2);
            r.poll(3101); CHECK(r.d.busy() && r.link.queryAttempts == 3);
            CHECK(sameResultQuery(r.link.queries.back(), stopIdentity(s)) && r.link.stops.size() == 1);
        });
    for (const char* owner : {"durable local pending", "maintenance"})
        scenario(std::string("Stop recovery yields only its owned query / ") + owner, [] {
            Rig r; const auto s = stop(); beginStopQuery(r, s);
            nowMs = 1102; r.d.poll(nowMs, true);
            CHECK(!r.d.busy() && r.link.queryCancellations == 1);
            const auto external = known(command(ProductCommand::Clean, 9).request).query;
            CHECK(r.link.requestResult(external, nowMs));
            r.link.completeQuery(known(command(ProductCommand::Clean, 9).request));
            for (uint32_t at = 1103; at < 2102; ++at) { nowMs = at; r.d.poll(at, true); }
            CHECK(r.link.queryCancellations == 1 && r.link.lookup == ResultLookupState::Complete);
            CHECK(!r.d.busy() && r.net.acks.empty());
            r.link.cancelResultQuery(); r.poll(2102);
            CHECK(r.d.busy() && r.link.queries.size() == 3 && r.link.stops.size() == 1);
            r.link.completeQuery(knownStop(s)); r.poll(2103);
            CHECK(r.net.acks.size() == 1 && r.link.queryCancellations == 3);
        });
    for (bool querying : {false, true})
        scenario(querying ? "ordinary command preempts owned Stop query" : "ordinary command starts during Stop retry wait", [=] {
            Rig r; const auto s = stop(); r.d.stop(s, 7, nowMs);
            r.link.stopState = StopSendState::TimedOut; r.poll(101);
            if (querying) r.poll(1101);
            const auto c = command(ProductCommand::Clean, 3, nowMs); r.start(c);
            CHECK(r.d.busy() && r.link.commands.size() == 1 && r.net.acks.empty());
            CHECK(r.link.queryCancellations == (querying ? 1u : 0u));
            r.link.send = CommandSendState::TimedOut; r.poll(nowMs + 1);
            r.poll(nowMs + 1000); CHECK(r.d.busy());
            CHECK(sameResultQuery(r.link.queries.back(), known(c.request).query));
            r.link.completeQuery(known(c.request)); r.poll(nowMs + 1); r.poll(nowMs + 1);
            CHECK(r.net.acks.size() == 1); ack(r.net.acks[0], c, true, "accepted");
            r.poll(nowMs + 1000); CHECK(r.d.busy());
            CHECK(sameResultQuery(r.link.queries.back(), stopIdentity(s)));
            r.link.completeQuery(knownStop(s)); r.poll(nowMs + 1);
            CHECK(!r.d.busy() && r.net.acks.size() == 2 && r.link.stops.size() == 1 && r.link.commands.size() == 1);
            stopAck(r.net.acks[1], s, true, "already_idle");
        });
    for (bool querying : {false, true})
        scenario(querying ? "new explicit Stop replaces informational owned query" : "new explicit Stop replaces informational retry wait", [=] {
            Rig r; const auto old = stop(); r.d.stop(old, 7, nowMs);
            r.link.stopState = StopSendState::TimedOut; r.poll(101);
            if (querying) r.poll(1101);
            std::strcpy(r.link.status.activeExecutionId, executionB);
            r.link.status.motionBusy = true; r.link.status.stationary = false;
            blocked = "storage_fault"; const auto next = stop(3, nowMs); r.d.stop(next, 7, nowMs);
            CHECK(r.link.stops.size() == 2 && !admissionCalls && !r.d.busy());
            CHECK(r.link.queryCancellations == (querying ? 1u : 0u));
            for (auto byte : r.link.stops.back().executionId) CHECK(byte == 0x22);
            r.link.stopState = StopSendState::TimedOut; r.poll(nowMs + 1); r.poll(nowMs + 1000);
            CHECK(sameResultQuery(r.link.queries.back(), stopIdentity(next)));
            r.link.completeQuery(knownStop(old)); r.poll(nowMs + 1);
            CHECK(r.net.acks.empty() && !r.d.busy()); r.poll(nowMs + 1000);
            r.link.completeQuery(knownStop(next, true, "accepted")); r.poll(nowMs + 1); r.poll(nowMs + 5000);
            CHECK(r.net.acks.size() == 1 && r.link.stops.size() == 2);
            stopAck(r.net.acks[0], next, true, "accepted");
        });
    scenario("ordinary-owned recovery and local query are not canceled by new Stop", [] {
        Rig r; const auto c = command(); r.query(c); const auto s = stop();
        r.d.stop(s, 7, nowMs); r.link.stopState = StopSendState::TimedOut; r.poll(1102);
        CHECK(r.d.busy() && !r.link.queryCancellations && r.link.queries.size() == 1);
        r.link.completeQuery(known(c.request)); r.poll(1103); r.poll(1104);
        CHECK(r.net.acks.size() == 1); ack(r.net.acks[0], c, true, "accepted");
        r.poll(2104); CHECK(r.d.busy() && r.link.queries.size() == 2 && r.link.stops.size() == 1);
        CHECK(sameResultQuery(r.link.queries.back(), stopIdentity(s)));
    });
}

void basics() {
    scenario("configuration proof gates only new Prepare and rejected bytes cannot replay", [] {
        Rig r;
        r.d.setPrepareReadyHandler(+[] { return false; });
        const auto c = command(); r.start(c);
        CHECK(r.link.commands.empty() && r.net.acks.size() == 1);
        ack(r.net.acks[0], c, false, "context_required");
        r.d.setPrepareReadyHandler(+[] { return true; });
        r.start(c);
        CHECK(r.link.commands.empty());
        r.start(command(ProductCommand::Prepare, 2));
        CHECK(r.link.commands.size() == 1);
    });
    for (auto kind : {ProductCommand::Clean, ProductCommand::ResetError,
                      ProductCommand::SetTargetTemp, ProductCommand::CheckFirmwareUpdate})
        scenario("non-Prepare yields configuration without demanding proof / " + std::to_string(unsigned(kind)), [=] {
            Rig r;
            r.d.setPrepareReadyHandler(+[] { return false; });
            r.link.allowCommand = false;
            static FakeLink* yieldingLink;
            yieldingLink = &r.link;
            r.d.setConfigurationYieldHandler(+[](uint32_t at) {
                CHECK(at == nowMs); yieldingLink->allowCommand = true;
            });
            const auto c = command(kind); r.start(c);
            CHECK(r.link.commands.size() == 1);
            r.link.allowCommand = false;
            r.start(c); // Duplicate/expired commands must not cancel a new transfer.
            CHECK(!r.link.allowCommand && r.link.commands.size() == 1);
        });
    scenario("expired non-Prepare never yields configuration", [] {
        Rig r; r.d.setConfigurationYieldHandler(+[](uint32_t) { CHECK(false); });
        nowMs = 5101;
        r.start(command(ProductCommand::Clean));
        CHECK(r.link.commands.empty());
        ack(r.net.acks[0], command(ProductCommand::Clean), false, "request_expired");
    });
    const ProductCommand kinds[] = {ProductCommand::Prepare, ProductCommand::Clean,
        ProductCommand::SetTargetTemp, ProductCommand::ResetError, ProductCommand::CheckFirmwareUpdate};
    const char* names[] = {"prepare", "clean", "set_target_temp", "reset_error", "check_firmware_update"};
    for (unsigned i = 0; i < 5; ++i) for (bool accepted : {false, true})
        scenario(std::string("Current once / ") + names[i] + (accepted ? " accepted" : " rejected"), [=] {
            Rig r; const auto c = command(kinds[i]); r.start(c);
            CHECK(r.link.commands.size() == 1 && r.d.busy() && !r.d.resultPending());
            CHECK(crypto::calls == 1 && sameProductRequest(r.link.commands[0].request, c.request));
            for (uint32_t at = 100; at < 130; ++at) { r.start(c); r.poll(at); }
            CHECK(r.link.commands.size() == 1 && r.net.acks.empty());
            r.resolve(c.request, accepted); CHECK(r.net.acks.size() == 1);
            ack(r.net.acks[0], c, accepted, accepted ? "accepted" : "not_ready");
            CHECK(r.net.acks[0].name == names[i]);
            r.start(c); r.poll(200); CHECK(r.link.commands.size() == 1 && r.net.acks.size() == 1);
        });
    scenario("dispatcher adds no STATUS/Ready gate beyond Link connection policy", [] {
        Rig r; r.link.telemetry = false; r.start();
        CHECK(r.link.commands.size() == 1 && r.net.acks.empty());
    });
    scenario("noBoard command and Stop never request actions", [] {
        Rig r; r.link.board = false; r.link.telemetry = false;
        r.start(); r.d.stop(stop(), 7, nowMs); r.poll(10000);
        CHECK(r.link.commands.empty() && r.link.stops.empty() && r.link.queries.empty());
        CHECK(r.net.acks.size() == 2);
        ack(r.net.acks[0], command(), false, "link_lost");
        stopAck(r.net.acks[1], stop(), false, "motion_state_unavailable");
    });
}

void freshness() {
    for (uint32_t age : {0u, 123u, 4949u, 4950u, 4999u, 5000u, 5001u})
        scenario("original sample TTL and 50ms budget / age " + std::to_string(age), [=] {
            Rig r; auto c = command(); nowMs = c.sampledAtMs + age;
            // Deliberately stale caller nowMs; the supplied clock owns the deadline.
            r.d.command(c, 7, 0);
            const bool allowed = age < 4950;
            CHECK(r.link.commands.size() == (allowed ? 1u : 0u));
            if (allowed) CHECK(r.link.commands[0].remainingTtlMs == 5000 - age);
            else { CHECK(r.net.acks.size() == 1); ack(r.net.acks[0], c, false, "request_expired"); }
            for (const auto& check : r.net.checks)
                CHECK(check.sample == c.sampledAtMs && check.ttl == c.ttlMs && check.generation == 7);
        });
    for (bool afterSha : {false, true}) scenario(afterSha ? "clock expires after real SHA" : "clock expires before SHA", [=] {
        Rig r; const auto c = command();
        clockHook = [=] { if ((afterSha && crypto::calls) || (!afterSha && clockCalls >= 2)) nowMs = 5050; };
        r.start(c); CHECK(r.link.commands.empty() && !r.d.busy());
        CHECK(crypto::calls == (afterSha ? 1u : 0u));
        CHECK(r.net.acks.size() == 1); ack(r.net.acks[0], c, false, "request_expired");
    });
    scenario("successful SHA time is deducted, not just checked against deadline", [] {
        Rig r; nowMs = 2500; clockHook = [] { if (crypto::calls) nowMs = 2512; };
        r.start(); CHECK(crypto::calls == 1 && r.link.commands.size() == 1);
        CHECK(r.link.sentAt[0] == 2512 && r.link.commands[0].remainingTtlMs == 2588);
        for (const auto& check : r.net.checks) CHECK(check.sample == 100 && check.ttl == 5000);
    });
    scenario("SHA failure is storage_fault, no action or query", [] {
        Rig r; crypto::fail = true; r.start();
        CHECK(crypto::calls == 1 && r.link.commands.empty() && r.link.queries.empty());
        CHECK(r.net.acks.size() == 1); ack(r.net.acks[0], command(), false, "storage_fault");
    });
    for (unsigned shape = 0; shape < 7; ++shape) scenario("untrusted/expired command and Stop / " + std::to_string(shape), [=] {
        Rig r; auto c = command(); auto s = stop(); uint32_t generation = 7;
        if (shape == 0) generation = 6;
        if (shape == 1) { std::strcpy(c.session, sessionB); std::strcpy(s.session, sessionB); }
        if (shape == 2) r.net.session.close();
        if (shape == 3) { c.ttlMs = s.ttlMs = 4999; }
        if (shape == 4) { c.sampledAtMs = s.sampledAtMs = 101; }
        if (shape == 5) nowMs = 5101;
        if (shape == 6) { r.net.session.close(); CHECK(r.net.session.open(sessionB, 8, 99)); }
        r.d.command(c, generation, nowMs); r.d.stop(s, generation, nowMs);
        CHECK(r.link.commands.empty() && r.link.stops.empty());
        if (shape == 4 || shape == 5) {
            CHECK(r.net.acks.size() == 2);
            ack(r.net.acks[0], c, false, "request_expired");
            stopAck(r.net.acks[1], s, false, "request_expired");
            CHECK(r.net.checks[0].result == cloud::Freshness::Expired);
        } else CHECK(r.net.acks.empty());
        CHECK(!r.d.busy() && !crypto::calls && !admissionCalls);
    });
    for (size_t check = 1; check <= 2; ++check) scenario("session invalidates on recheck / " + std::to_string(check), [=] {
        Rig r; r.net.beforeCheck = [&](size_t at) { if (at == check) r.net.session.close(); };
        r.start(); CHECK(r.link.commands.empty() && !r.d.busy());
        CHECK(crypto::calls == (check == 2 ? 1u : 0u));
    });
    scenario("TTL subtraction covers uint32 wrap", [] {
        nowMs = 10; FakeLink link; FakeNetwork net(0xfffffc00u);
        Dispatcher d(link, net, clockNow, admission); const auto c = command(ProductCommand::Clean, 1, 0xfffffff0u);
        d.command(c, 7, nowMs); CHECK(link.commands.size() == 1 && link.commands[0].remainingTtlMs == 4974);
    });
    for (unsigned timing = 0; timing < 3; ++timing)
        scenario("Expired duplicate cannot freeze false ACK over original acceptance / " + std::to_string(timing), [=] {
            Rig r; const auto c = command(); r.start(c);
            if (timing == 1) { r.link.complete(result(c.request)); r.poll(101); CHECK(r.d.resultPending()); }
            if (timing == 2) { r.resolve(c.request); CHECK(r.net.acks.size() == 1); }
            const auto ackCount = r.net.acks.size(); nowMs = 5101;
            r.d.command(c, 7, nowMs);
            CHECK(r.net.checks.back().result == cloud::Freshness::Expired);
            CHECK(r.net.acks.size() == ackCount && r.link.commands.size() == 1);
            if (timing == 0) {
                r.poll(5101); r.poll(6101); CHECK(r.link.queries.size() == 1);
                r.link.completeQuery(known(c.request)); r.poll(6102); r.poll(6103);
            } else r.poll(5101);
            CHECK(r.net.acks.size() == 1); ack(r.net.acks[0], c, true, "accepted");
            r.d.command(c, 7, nowMs); CHECK(r.net.acks.size() == 1 && r.link.commands.size() == 1);
        });
    for (bool newerWhileBusy : {false, true})
        scenario(newerWhileBusy ? "new Expired higher seq while original awaits acceptance" : "new unseen Expired is rejected and consumed", [=] {
            Rig r; const auto first = command();
            if (newerWhileBusy) r.start(first);
            const auto expired = command(ProductCommand::Clean, newerWhileBusy ? 2 : 1);
            nowMs = 5101; r.start(expired);
            CHECK(r.net.checks.back().result == cloud::Freshness::Expired);
            CHECK(r.net.acks.size() == 1); ack(r.net.acks[0], expired, false, "request_expired");
            const auto sent = r.link.commands.size();
            r.start(expired); CHECK(r.net.acks.size() == 1 && r.link.commands.size() == sent);
            // Even a backwards test clock cannot give consumed bytes new permission.
            nowMs = 100; r.start(expired);
            CHECK(r.net.checks.back().result == cloud::Freshness::Current);
            CHECK(r.net.acks.size() == 1 && r.link.commands.size() == sent);
            if (newerWhileBusy) {
                r.resolve(first.request); CHECK(r.net.acks.size() == 2);
                ack(r.net.acks[1], first, true, "accepted"); r.start(expired);
                CHECK(r.link.commands.size() == 1);
            }
        });
}

void admissionAndReplay() {
    for (const char* reason : {"busy", "storage_fault", "maintenance"}) scenario(std::string("admission / ") + reason, [=] {
        Rig r; blocked = reason; r.start();
        CHECK(r.link.commands.empty() && r.link.queries.empty() && !crypto::calls);
        CHECK(r.net.acks.size() == 1); ack(r.net.acks[0], command(), false, reason);
        blocked = nullptr; r.start(); CHECK(r.link.commands.empty() && r.net.acks.size() == 1);
    });
    scenario("local busy rejection consumes newer RAM seq before later replay", [] {
        Rig r; const auto first = command(); const auto busy = command(ProductCommand::Clean, 2);
        r.start(first); r.start(busy); CHECK(r.link.commands.size() == 1 && r.net.acks.size() == 1);
        ack(r.net.acks[0], busy, false, "busy"); r.resolve(first.request);
        r.start(busy); auto changed = busy; std::strcpy(changed.request.commandId, "different-id-same-seq"); r.start(changed);
        r.start(first); CHECK(r.link.commands.size() == 1);
        r.start(command(ProductCommand::Clean, 3)); CHECK(r.link.commands.size() == 2);
    });
    scenario("link refuses immediate command: no queue and seq consumed", [] {
        Rig r; r.link.allowCommand = false; r.start();
        CHECK(!r.d.busy() && r.link.commands.empty()); CHECK(r.net.acks.size() == 1);
        ack(r.net.acks[0], command(), false, "busy"); r.link.allowCommand = true; r.start();
        CHECK(r.link.commands.empty());
    });
    scenario("reconnect cannot reopen consumed seq within this boot", [] {
        Rig r; blocked = "busy"; r.start(); blocked = nullptr;
        r.net.session.close(); CHECK(r.net.session.open(sessionB, 8, nowMs));
        auto c = command(); std::strcpy(c.session, sessionB); r.d.command(c, 8, nowMs);
        CHECK(r.link.commands.empty()); c.request.sequence = 2; r.d.command(c, 8, nowMs);
        CHECK(r.link.commands.size() == 1);
    });
    scenario("real Brain NVS read fault admission is not hidden or cleared", [] {
        Rig r; BrainStateStore store;
        v4::Pairing pair; pair.role = v4::Role::Brain; std::strcpy(pair.deviceId, device);
        std::strcpy(pair.epoch, sessionA); std::strcpy(pair.localPhysicalId, "112233445566");
        std::strcpy(pair.peerPhysicalId, "aabbccddeeff");
        nvs::fail(nvs::Op::OpenRO, 1); CHECK(store.load(pair) == BrainLoad::IoError); nvs::verifyFaults();
        const auto calls = nvs::io.calls.size(); const auto disk = nvs::io.disk;
        admissionHook = [&] { return store.ready() ? nullptr : "storage_fault"; };
        r.start(); CHECK(r.link.commands.empty() && nvs::io.calls.size() == calls && nvs::io.disk == disk);
        CHECK(!nvs::count(nvs::Op::Set) && !nvs::count(nvs::Op::Commit));
        ack(r.net.acks.at(0), command(), false, "storage_fault");
    }, true);
}

void acceptanceCallbacks() {
    for (bool recovered : {false, true})
        for (auto kind : {ProductCommand::Prepare, ProductCommand::Clean, ProductCommand::SetTargetTemp})
            for (bool accepted : {false, true})
                scenario("acceptance callback original request before release / " + std::to_string(unsigned(kind)) +
                         (recovered ? " query" : " ACK") + (accepted ? " accepted" : " rejected"), [=] {
                    Rig r; auto incoming = command(kind); const auto original = incoming;
                    unsigned calls = 0;
                    r.d.setAcceptanceHandler(observeAcceptance);
                    acceptanceHook = [&](const ProductRequest& request, uint32_t at) {
                        ++calls;
                        CHECK(sameProductRequest(request, original.request) && at == nowMs);
                        CHECK(r.d.ordinaryBusy() && !r.d.resultPending() && r.net.acks.empty());
                    };
                    if (recovered) r.query(incoming); else r.start(incoming);
                    // The owner's request, not the caller's reused buffer, is authoritative.
                    std::strcpy(incoming.request.commandId, "caller-overwritten");
                    if (kind == ProductCommand::Prepare) incoming.request.waterMl = 30;
                    CHECK(!calls);
                    r.net.publishAvailable = false;
                    if (recovered) r.link.completeQuery(known(original.request, accepted));
                    else r.link.complete(result(original.request, accepted));
                    r.poll(1200);
                    CHECK(calls == unsigned(accepted) && !r.d.ordinaryBusy() && r.d.resultPending());
                    r.poll(1201); r.poll(1202);
                    CHECK(calls == unsigned(accepted));
                    r.net.publishAvailable = true; r.poll(1203);
                    CHECK(!r.d.resultPending());
                    ack(r.net.acks.back(), original, accepted, accepted ? "accepted" : "not_ready");
                    r.start(original); r.poll(1204);
                    CHECK(calls == unsigned(accepted) && r.link.commands.size() == 1);
                });
    for (unsigned fault = 0; fault < 9; ++fault)
        scenario("recovered acceptance callback excludes wrong/uncertain proof / " + std::to_string(fault), [=] {
            Rig r; const auto original = command(); r.query(original);
            unsigned calls = 0;
            r.d.setAcceptanceHandler(observeAcceptance);
            acceptanceHook = [&](const ProductRequest& request, uint32_t at) {
                ++calls; CHECK(sameProductRequest(request, original.request) && at == nowMs);
                CHECK(r.d.ordinaryBusy() && r.net.acks.empty());
            };
            auto reply = known(original.request);
            if (fault == 0) reply.query.source = v4::Source::LocalTouch;
            if (fault == 1) ++reply.query.sequence;
            if (fault == 2) std::strcpy(reply.query.deviceId, "other-device");
            if (fault == 3) std::strcpy(reply.query.commandId, "other-command");
            if (fault == 4) reply.requestDigestHex[0] = reply.requestDigestHex[0] == '0' ? '1' : '0';
            if (fault >= 5) {
                reply.status = ResultQueryStatus(fault - 4);
                reply.requestDigestHex[0] = 0;
                const char* reasons[] = {"unknown", "result_expired", "request_conflict", "storage_fault"};
                std::strcpy(reply.reason, reasons[fault - 5]);
            }
            r.link.completeQuery(reply, fault < 5); r.poll(1102);
            CHECK(!calls && r.d.ordinaryBusy() && !r.d.resultPending() && r.net.acks.empty());
            r.poll(2102); r.link.completeQuery(known(original.request)); r.poll(2103);
            CHECK(calls == 1 && !r.d.ordinaryBusy()); r.poll(2104); CHECK(calls == 1);
        });
    for (unsigned fault = 0; fault < 3; ++fault)
        scenario("direct acceptance callback excludes mismatched ACK / " + std::to_string(fault), [=] {
            Rig r; const auto original = command(); r.start(original);
            unsigned calls = 0;
            r.d.setAcceptanceHandler(observeAcceptance);
            acceptanceHook = [&](const ProductRequest&, uint32_t) { ++calls; };
            auto reply = result(original.request);
            if (fault == 0) reply.source = v4::Source::LocalTouch;
            if (fault == 1) ++reply.sequence;
            if (fault == 2) std::strcpy(reply.commandId, "other-command");
            r.link.complete(reply); r.poll(101);
            CHECK(!calls && r.d.ordinaryBusy() && r.net.acks.empty());
        });
}

void results() {
    for (unsigned mismatch = 0; mismatch < 3; ++mismatch) scenario("COMMAND_RESULT exact identity / mismatch " + std::to_string(mismatch), [=] {
        Rig r; const auto c = command(); r.start(c); auto reply = result(c.request);
        if (mismatch == 0) reply.source = v4::Source::LocalTouch;
        if (mismatch == 1) ++reply.sequence;
        if (mismatch == 2) std::strcpy(reply.commandId, "wrong-id");
        r.link.complete(reply); r.poll(101); CHECK(r.d.busy() && !r.d.resultPending() && r.net.acks.empty());
        r.poll(1101); CHECK(r.link.queries.size() == 1);
        r.link.completeQuery(known(c.request)); r.poll(1102); r.poll(1103);
        CHECK(r.net.acks.size() == 1); ack(r.net.acks[0], c, true, "accepted");
    });
    for (bool accepted : {false, true}) scenario(accepted ? "query Known original digest acceptance" : "query Known original digest rejection", [=] {
        Rig r; const auto c = command(); r.query(c);
        r.link.completeQuery(known(c.request, accepted)); r.poll(1102);
        CHECK(!r.d.busy() && r.d.resultPending() && r.net.acks.empty()); r.poll(1103);
        CHECK(r.net.acks.size() == 1 && r.link.queryCancellations == 1 && r.link.commands.size() == 1);
        ack(r.net.acks[0], c, accepted, accepted ? "accepted" : "not_ready");
    });
    for (unsigned status = 1; status <= 4; ++status) for (bool accepted : {false, true})
        scenario("query uncertain must not ACK / status " + std::to_string(status) + (accepted ? " forged accepted" : " rejected bit"), [=] {
            Rig r; const auto c = command(); r.query(c); auto q = known(c.request, accepted);
            q.status = ResultQueryStatus(status); q.requestDigestHex[0] = 0;
            const char* reasons[] = {"known", "unknown", "result_expired", "request_conflict", "storage_fault"};
            std::strcpy(q.reason, reasons[status]);
            // accepted=true is deliberately an invalid decoded response boundary.
            r.link.completeQuery(q, !accepted); r.poll(1102); r.poll(2101);
            CHECK(r.d.busy() && !r.d.resultPending() && r.net.acks.empty() && r.link.queries.size() == 1);
            r.poll(2102); CHECK(r.link.queries.size() == 2 && r.link.commands.size() == 1);
            CHECK(sameResultQuery(r.link.queries[0], r.link.queries[1]));
        });
    for (unsigned status = 1; status <= 4; ++status)
        scenario("unresolved ordinary preserves independent Stop and exact Known recovery / " + std::to_string(status), [=] {
            Rig r; const auto original = command(); r.query(original);
            auto unknown = known(original.request);
            unknown.status = ResultQueryStatus(status); unknown.accepted = false;
            unknown.requestDigestHex[0] = 0;
            const char* reasons[] = {"known", "unknown", "result_expired", "request_conflict", "storage_fault"};
            std::strcpy(unknown.reason, reasons[status]);
            r.link.completeQuery(unknown); r.poll(1102);
            nowMs = 6000;
            r.link.telemetryReceivedAt = nowMs;
            std::strcpy(r.link.status.activeExecutionId, executionA);
            r.link.status.motionBusy = true; r.link.status.stationary = false;
            const auto blockedRequest = command(ProductCommand::Clean, 2, nowMs);
            r.start(blockedRequest);
            CHECK(r.d.busy() && r.link.commands.size() == 1 && r.net.acks.size() == 1);
            ack(r.net.acks[0], blockedRequest, false, "busy");
            const auto safetyStop = stop(3, nowMs); const auto admissions = admissionCalls;
            blocked = "storage_fault"; r.d.stop(safetyStop, 7, nowMs);
            CHECK(r.link.stops.size() == 1 && admissionCalls == admissions);
            CHECK(r.link.stops[0].scope == v4::StopScope::Product);
            for (auto byte : r.link.stops[0].executionId) CHECK(byte == 0x11);
            r.link.stopState = StopSendState::Received; r.poll(6001);
            CHECK(r.d.busy() && r.link.queries.size() == 2 && r.net.acks.size() == 2);
            stopAck(r.net.acks[1], safetyStop, true, "accepted");
            CHECK(sameResultQuery(r.link.queries[0], r.link.queries[1]));
            r.link.completeQuery(known(original.request)); r.poll(6002); r.poll(6003);
            CHECK(!r.d.busy() && !r.d.resultPending() && r.net.acks.size() == 3);
            ack(r.net.acks[2], original, true, "accepted");
            blocked = nullptr;
            r.start(original); r.start(blockedRequest);
            CHECK(r.link.commands.size() == 1 && r.net.acks.size() == 3);
            r.start(command(ProductCommand::Clean, 4, nowMs));
            CHECK(r.d.busy() && r.link.commands.size() == 2);
        });
    for (unsigned shape = 0; shape < 10; ++shape) for (bool accepted : {false, true})
        scenario("Known wrong identity/digest is uncertain / " + std::to_string(shape) + (accepted ? " accepted" : " rejected"), [=] {
            Rig r; const auto c = command(); r.query(c); auto q = known(c.request, accepted);
            if (shape == 0) ++q.query.sequence;
            if (shape == 1) q.query.source = v4::Source::LocalTouch;
            if (shape == 2) std::strcpy(q.query.deviceId, "other-device");
            if (shape == 3) std::strcpy(q.query.commandId, "other-id");
            if (shape == 4) q.requestDigestHex[0] = q.requestDigestHex[0] == '0' ? '1' : '0';
            if (shape == 5) q.requestDigestHex[0] = 0;
            if (shape == 6) q.requestDigestHex[63] = 0;
            if (shape == 7) q.requestDigestHex[64] = 'a';
            if (shape == 8) q.requestDigestHex[0] = 'g';
            if (shape == 9) {
                bool changed = false;
                for (unsigned i = 0; i < 64; ++i) if (q.requestDigestHex[i] >= 'a') {
                    q.requestDigestHex[i] -= 'a' - 'A'; changed = true;
                }
                CHECK(changed);
            }
            r.link.completeQuery(q, shape <= 4); r.poll(1102);
            CHECK(r.d.busy() && !r.d.resultPending() && r.net.acks.empty()); r.poll(2102);
            CHECK(r.link.queries.size() == 2 && r.link.commands.size() == 1);
        });
    scenario("late ordinary result after timeout cannot bypass owned result query", [] {
        Rig r; const auto c = command(); r.query(c);
        r.link.complete(result(c.request)); r.poll(1102);
        CHECK(r.net.acks.empty() && r.d.busy());
        r.link.completeQuery(known(c.request)); r.poll(1103); r.poll(1104);
        CHECK(r.net.acks.size() == 1 && r.link.commands.size() == 1);
    });
    for (auto state : {CommandSendState::Idle, CommandSendState::TimedOut,
                       CommandSendState::Unavailable, CommandSendState::Cancelled})
        scenario("ordinary uncertain transport / " + std::to_string(unsigned(state)), [=] {
            Rig r; r.start(); r.link.send = state; r.poll(101); r.poll(1101);
            CHECK(r.net.acks.empty() && r.d.busy() && r.link.queries.size() == 1 && r.link.commands.size() == 1);
        });
    for (auto state : {ResultLookupState::Idle, ResultLookupState::TimedOut, ResultLookupState::Unavailable})
        scenario("lookup transport retry / " + std::to_string(unsigned(state)), [=] {
            Rig r; r.query(command()); r.link.lookup = state; r.poll(1102); r.poll(2101);
            CHECK(r.link.queries.size() == 1 && r.net.acks.empty()); r.poll(2102);
            CHECK(r.link.queries.size() == 2 && r.link.commands.size() == 1);
        });
    scenario("Cloud disconnect cancels only unsent tracking; never Stop or replay", [] {
        Rig r; const auto c = command(); r.start(c); r.net.session.close(); r.poll(101);
        CHECK(r.link.cancellations == 1 && r.link.stops.empty() && r.d.busy());
        CHECK(r.net.session.open(sessionB, 8, 102)); r.poll(1101);
        CHECK(r.link.queries.size() == 1 && r.link.commands.size() == 1);
        r.link.completeQuery(known(c.request)); r.poll(1102); r.poll(1103);
        CHECK(r.net.acks.size() == 1); ack(r.net.acks[0], c, true, "accepted");
    });
    scenario("late previous result cannot ACK a newly issued command", [] {
        Rig r; const auto old = command(); r.start(old); r.resolve(old.request);
        const auto next = command(ProductCommand::Clean, 2); r.start(next);
        r.link.complete(result(old.request)); r.poll(101);
        CHECK(r.d.busy() && r.net.acks.size() == 1 && r.link.commands.size() == 2);
    });
    scenario("failed read-only lookup start is bounded and never replays action", [] {
        Rig r; r.link.allowQuery = false; r.start(); r.link.send = CommandSendState::TimedOut;
        r.poll(101); r.poll(1101); CHECK(r.link.queryAttempts == 1 && r.link.queries.empty());
        for (uint32_t at = 1102; at < 2101; ++at) r.poll(at);
        CHECK(r.link.queryAttempts == 1 && r.net.acks.empty());
        r.link.allowQuery = true; r.poll(2101);
        CHECK(r.link.queryAttempts == 2 && r.link.queries.size() == 1 && r.link.commands.size() == 1);
    });
}

void expiredUnknown() {
    for (bool wrap : {false, true}) for (bool querying : {false, true})
        for (uint32_t age : {4999u, 5000u, 5001u})
            scenario(std::string("unknown original TTL / ") + (wrap ? "wrap / " : "linear / ") +
                     (querying ? "querying / " : "waiting / ") + std::to_string(age), [=] {
                const uint32_t sample = wrap ? UINT32_MAX - 100u : 100u;
                nowMs = sample; FakeLink link; FakeNetwork net(sample - 1u);
                Dispatcher d(link, net, clockNow, admission); const auto c = command(ProductCommand::Clean, 1, sample);
                d.command(c, 7, nowMs); CHECK(d.busy());
                link.send = CommandSendState::TimedOut; d.poll(nowMs);
                link.allowQuery = querying;
                if (querying) { nowMs = sample + 1000u; d.poll(nowMs); CHECK(link.queries.size() == 1); }
                nowMs = sample + age; link.telemetryReceivedAt = nowMs;
                d.poll(nowMs);
                CHECK(d.busy() == (age <= 5000));
                CHECK(!d.resultPending() && net.acks.empty() && link.commands.size() == 1);
                if (age > 5000) {
                    CHECK(link.cancellations == 1 && link.queryCancellations == (querying ? 1u : 0u));
                    const auto attempts = link.queryAttempts; nowMs += 2000; d.poll(nowMs);
                    CHECK(!d.busy() && link.queryAttempts == attempts && net.acks.empty());
                }
            });
    const char* faults[] = {"preexpiry receipt", "exact expiry receipt", "future receipt", "age 1500",
        "age 1501", "offline Motion", "no telemetry", "stale telemetry view", "motion busy",
        "not stationary", "active execution"};
    for (bool wrap : {false, true}) for (unsigned fault = 0; fault < 11; ++fault)
        scenario(std::string("postexpiry STATUS blocks / ") + (wrap ? "wrap / " : "linear / ") + faults[fault], [=] {
            const uint32_t sample = wrap ? UINT32_MAX - 100u : 100u;
            nowMs = sample; FakeLink link; FakeNetwork net(sample - 1u);
            Dispatcher d(link, net, clockNow, admission); const auto c = command(ProductCommand::Clean, 1, sample);
            d.command(c, 7, nowMs); link.send = CommandSendState::TimedOut; d.poll(nowMs);
            nowMs = sample + 1000u; d.poll(nowMs); CHECK(link.queries.size() == 1);
            nowMs = sample + ((fault == 3 || fault == 4) ? 6900u : 5500u);
            link.telemetryReceivedAt = nowMs;
            if (fault == 0) link.telemetryReceivedAt = sample + 4999u;
            if (fault == 1) link.telemetryReceivedAt = sample + 5000u;
            if (fault == 2) link.telemetryReceivedAt = nowMs + 1u;
            if (fault == 3 || fault == 4) link.telemetryReceivedAt = nowMs - (fault == 3 ? 1500u : 1501u);
            if (fault == 5) link.board = false;
            if (fault == 6) link.telemetry = false;
            if (fault == 7) link.telemetryFresh = false;
            if (fault == 8) link.status.motionBusy = true;
            if (fault == 9) link.status.stationary = false;
            if (fault == 10) std::strcpy(link.status.activeExecutionId, executionA);
            d.poll(nowMs);
            CHECK(d.busy() && !d.resultPending() && net.acks.empty());
            CHECK(!link.cancellations && !link.queryCancellations && link.commands.size() == 1);
            link.board = link.telemetry = link.telemetryFresh = true;
            link.status.motionBusy = false; link.status.stationary = true; link.status.activeExecutionId[0] = 0;
            link.telemetryReceivedAt = nowMs; d.poll(nowMs);
            CHECK(!d.busy() && !d.resultPending() && net.acks.empty());
            CHECK(link.cancellations == 1 && link.queryCancellations == 1 && link.commands.size() == 1);
        });
    for (uint32_t age : {0u, 1499u}) scenario("postexpiry receipt valid age / " + std::to_string(age), [=] {
        Rig r; const auto c = command(); r.query(c);
        r.link.telemetryReceivedAt = 7000 - age; r.link.status.sampleUptimeMs = 0;
        r.poll(7000); CHECK(!r.d.busy() && !r.d.resultPending() && r.net.acks.empty());
        CHECK(r.link.commands.size() == 1 && r.link.queryCancellations == 1);
    });
    for (unsigned status = 1; status <= 4; ++status) for (bool forgedAccepted : {false, true})
        scenario("expired uncertain exits RAM without ACK / " + std::to_string(status) +
                 (forgedAccepted ? " forged accepted" : " rejected bit"), [=] {
            Rig r; const auto c = command(); r.query(c); auto q = known(c.request, forgedAccepted);
            q.status = ResultQueryStatus(status); q.requestDigestHex[0] = 0;
            const char* reasons[] = {"known", "unknown", "result_expired", "request_conflict", "storage_fault"};
            std::strcpy(q.reason, reasons[status]); r.link.completeQuery(q, !forgedAccepted);
            r.link.telemetryReceivedAt = 6000; r.poll(6000);
            CHECK(!r.d.busy() && !r.d.resultPending() && r.net.acks.empty());
            CHECK(r.link.commands.size() == 1 && r.link.cancellations == 1 && r.link.queryCancellations == 1);
            r.poll(10000); CHECK(r.net.acks.empty() && r.link.queries.size() == 1);
        });
    for (bool queried : {false, true}) for (bool accepted : {false, true})
        scenario(std::string("expired exact Known precedes release / ") + (queried ? "query / " : "direct / ") +
                 (accepted ? "accepted" : "rejected"), [=] {
            Rig r; const auto c = command();
            if (queried) { r.query(c); r.link.completeQuery(known(c.request, accepted)); }
            else { r.start(c); r.link.complete(result(c.request, accepted)); }
            r.link.telemetryReceivedAt = 6000; r.poll(6000);
            CHECK(!r.d.busy() && r.d.resultPending() && r.net.acks.empty() && !r.link.cancellations);
            r.poll(6001); CHECK(r.net.acks.size() == 1);
            ack(r.net.acks[0], c, accepted, accepted ? "accepted" : "not_ready");
            // Definitive results are not retired-ID conflicts; Motion owns deduplication.
            auto sameId = c; sameId.request.sequence = 2; sameId.sampledAtMs = nowMs;
            r.start(sameId); CHECK(r.link.commands.size() == 2 && r.net.acks.size() == 1);
        });
    for (bool maximumId : {false, true}) scenario(maximumId ? "retired full 128-byte ID retained" : "retired ID and seq never replay", [=] {
        Rig r; auto c = command();
        if (maximumId) { std::memset(c.request.commandId, 'x', 128); c.request.commandId[128] = 0; }
        r.query(c); r.link.telemetryReceivedAt = 6000; r.poll(6000);
        CHECK(!r.d.busy() && r.net.acks.empty());
        auto replay = c; replay.sampledAtMs = nowMs; r.start(replay);
        std::strcpy(replay.request.commandId, "different-id-same-seq"); r.start(replay);
        CHECK(r.link.commands.size() == 1 && r.net.acks.empty());
        auto conflict = c; conflict.sampledAtMs = nowMs; conflict.request.sequence = 2;
        r.start(conflict); CHECK(r.net.acks.size() == 1 && r.link.commands.size() == 1);
        ack(r.net.acks[0], conflict, false, "request_conflict");
        r.start(conflict); CHECK(r.net.acks.size() == 1);
        const auto next = command(ProductCommand::Clean, 3, nowMs);
        r.start(next); r.start(next); r.poll(6001);
        CHECK(r.d.busy() && r.link.commands.size() == 2 && r.net.acks.size() == 1);
        CHECK(sameProductRequest(r.link.commands.back().request, next.request));
        r.resolve(next.request); CHECK(r.net.acks.size() == 2);
        ack(r.net.acks.back(), next, true, "accepted");
        r.start(c); r.start(next); CHECK(r.link.commands.size() == 2 && r.net.acks.size() == 2);
    });
    for (bool queryReply : {false, true}) scenario(queryReply ? "late retired Known cannot ACK new query" : "late retired result cannot ACK new command", [=] {
        Rig r; const auto old = command(); r.query(old);
        r.link.telemetryReceivedAt = 6000; r.poll(6000); CHECK(!r.d.busy());
        const auto next = command(ProductCommand::Clean, 2, nowMs); r.start(next);
        if (queryReply) {
            r.link.send = CommandSendState::TimedOut; r.poll(6001); r.poll(7001);
            CHECK(r.link.queries.size() == 2); r.link.completeQuery(known(old.request));
        } else r.link.complete(result(old.request));
        r.poll(7002); CHECK(r.d.busy() && !r.d.resultPending() && r.net.acks.empty());
        r.poll(8002); CHECK(r.link.queries.size() == (queryReply ? 3u : 2u));
        CHECK(r.link.queries.back().sequence == next.request.sequence);
        r.link.completeQuery(known(next.request)); r.poll(8003); r.poll(8004);
        CHECK(r.net.acks.size() == 1 && r.link.commands.size() == 2);
        ack(r.net.acks[0], next, true, "accepted");
    });
    scenario("offline Cloud can release RAM but new Cloud still needs current session", [] {
        Rig r; const auto old = command(); r.start(old);
        r.net.session.close(); r.net.publishAvailable = false; r.poll(101);
        CHECK(r.d.busy() && r.link.cancellations == 1);
        r.link.telemetryReceivedAt = 6000; r.poll(6000);
        CHECK(!r.d.busy() && !r.d.resultPending() && r.net.acks.empty() && r.link.commands.size() == 1);
        const auto next = command(ProductCommand::Clean, 2, nowMs); r.start(next);
        CHECK(r.link.commands.size() == 1 && r.net.acks.empty());
        CHECK(r.net.session.open(sessionB, 8, nowMs)); r.start(next);
        auto current = next; std::strcpy(current.session, sessionB); r.d.command(current, 7, nowMs);
        CHECK(r.link.commands.size() == 1 && r.net.acks.empty());
        r.d.command(current, 8, nowMs); r.d.command(current, 8, nowMs);
        CHECK(r.link.commands.size() == 2 && r.d.busy() && r.net.acks.empty());
    });
    scenario("expired release never gates independent Product Stop on business admission", [] {
        Rig r; const auto c = command(); r.query(c);
        r.link.telemetryReceivedAt = 6000; r.poll(6000); CHECK(!r.d.busy());
        std::strcpy(r.link.status.activeExecutionId, executionB);
        r.link.status.motionBusy = true; r.link.status.stationary = false;
        blocked = "storage_fault"; const auto calls = admissionCalls; const auto s = stop(2, nowMs);
        r.d.stop(s, 7, nowMs);
        CHECK(r.link.stops.size() == 1 && admissionCalls == calls && r.link.stops[0].scope == v4::StopScope::Product);
        for (auto byte : r.link.stops[0].executionId) CHECK(byte == 0x22);
        r.link.stopState = StopSendState::Received; r.poll(6001);
        CHECK(r.net.acks.size() == 1); stopAck(r.net.acks[0], s, true, "accepted");
        CHECK(r.link.commands.size() == 1 && !r.d.busy());
    });
}

void replyRetries() {
    for (bool queried : {false, true}) for (bool accepted : {false, true})
        scenario(std::string("failed ACK retains original session / ") + (queried ? "query" : "direct") + (accepted ? " accept" : " reject"), [=] {
            Rig r; const auto c = command();
            if (queried) { r.query(c); r.link.completeQuery(known(c.request, accepted)); }
            else { r.start(c); r.link.complete(result(c.request, accepted)); }
            r.net.publishAvailable = false; r.poll(nowMs);
            CHECK(!r.d.busy() && r.d.resultPending());
            for (unsigned i = 0; i < 3; ++i) r.poll(++nowMs);
            CHECK(r.d.resultPending() && r.net.acks.size() == 3);
            r.net.session.close(); CHECK(r.net.session.open(sessionB, 8, nowMs));
            r.net.publishAvailable = true; r.poll(++nowMs); CHECK(!r.d.resultPending());
            for (const auto& a : r.net.acks) ack(a, c, accepted, accepted ? "accepted" : "not_ready");
            CHECK(r.net.acks.back().sent && r.link.commands.size() == 1);
            r.poll(++nowMs); CHECK(r.net.acks.size() == 4);
        });
    // Keep these assertions even if current production loses local rejections.
    for (const char* reason : {"busy", "storage_fault", "maintenance", "link_lost"})
        scenario(std::string("COUNTEREXAMPLE failed local rejection ACK must retry / ") + reason, [=] {
            Rig r; const auto c = command(); r.net.publishAvailable = false;
            if (!std::strcmp(reason, "link_lost")) r.link.board = false; else blocked = reason;
            r.start(c); CHECK(r.net.acks.size() == 1 && !r.net.acks[0].sent);
            blocked = nullptr; r.link.board = true; r.net.publishAvailable = true;
            r.net.session.close(); CHECK(r.net.session.open(sessionB, 8, nowMs));
            r.poll(101); CHECK(r.net.acks.size() == 2 && r.net.acks.back().sent);
            ack(r.net.acks.back(), c, false, reason); CHECK(r.link.commands.empty());
        });
    for (bool resultAlreadyKnown : {false, true})
        scenario(resultAlreadyKnown ? "busy rejection cannot overwrite pending original known ACK" : "busy rejection cannot overwrite original in-flight identity", [=] {
            Rig r; const auto original = command(); r.start(original); r.net.publishAvailable = false;
            if (resultAlreadyKnown) { r.link.complete(result(original.request)); r.poll(101); r.poll(102); }
            const auto newer = command(ProductCommand::Clean, 2); r.start(newer);
            CHECK(r.net.acks.back().id == newer.request.commandId && !r.net.acks.back().sent);
            CHECK(r.link.commands.size() == 1);
            if (!resultAlreadyKnown) { r.link.complete(result(original.request)); r.poll(103); }
            CHECK(r.d.resultPending()); r.net.publishAvailable = true; r.poll(104);
            CHECK(r.net.acks.back().sent); ack(r.net.acks.back(), original, true, "accepted");
            r.start(newer); CHECK(r.link.commands.size() == 1);
        });
    scenario("failed new Expired rejection retains original session for retry without execution", [] {
        Rig r; const auto c = command(); nowMs = 5101; r.net.publishAvailable = false; r.start(c);
        CHECK(r.d.resultPending() && !r.d.busy() && r.link.commands.empty());
        r.net.session.close(); CHECK(r.net.session.open(sessionB, 8, nowMs));
        r.net.publishAvailable = true; r.poll(5102);
        CHECK(r.net.acks.size() == 2 && r.net.acks.back().sent && !r.d.resultPending());
        ack(r.net.acks.back(), c, false, "request_expired"); CHECK(r.link.commands.empty());
    });
}

void stops() {
    for (unsigned ordinaryState = 0; ordinaryState < 4; ++ordinaryState)
        scenario("Stop seq19 remains valid after ordinary seq20 / " + std::to_string(ordinaryState), [=] {
            Rig r; const auto c = command(ProductCommand::Prepare, 20);
            if (ordinaryState == 0) blocked = "busy";
            r.start(c); blocked = nullptr;
            if (ordinaryState == 1 || ordinaryState == 2) r.resolve(c.request, ordinaryState == 2);
            std::strcpy(r.link.status.activeExecutionId, executionA);
            r.link.status.motionBusy = true; r.link.status.stationary = false;
            const auto s = stop(19); const auto admissions = admissionCalls;
            r.d.stop(s, 7, nowMs);
            CHECK(r.link.stops.size() == 1 && admissionCalls == admissions);
            CHECK(r.link.stops[0].sequence == 19 && r.link.stops[0].scope == v4::StopScope::Product);
            for (auto byte : r.link.stops[0].executionId) CHECK(byte == 0x11);
            r.link.stopState = StopSendState::Received; r.poll(101);
            std::strcpy(r.link.status.activeExecutionId, executionB);
            const auto acks = r.net.acks.size(); r.d.stop(s, 7, nowMs); r.poll(102);
            CHECK(r.link.stops.size() == 1 && r.net.acks.size() == acks);
            r.start(c); CHECK(r.link.commands.size() == (ordinaryState ? 1u : 0u));
        });
    scenario("larger Stop seq raises only the ordinary cancellation barrier", [] {
        Rig r; r.d.stop(stop(30), 7, nowMs); r.link.stopState = StopSendState::Received; r.poll(101);
        r.start(command(ProductCommand::Clean, 29)); CHECK(r.link.commands.empty());
        r.start(command(ProductCommand::Clean, 31)); CHECK(r.link.commands.size() == 1);
    });
    for (bool received : {false, true}) scenario(received ? "Expired duplicate of received Stop never false-ACKs" : "new Expired Stop consumed without rebind", [=] {
        Rig r; const auto s = stop();
        if (received) { r.d.stop(s, 7, nowMs); r.link.stopState = StopSendState::Received; r.poll(101); }
        nowMs = 5101; r.d.stop(s, 7, nowMs);
        CHECK(r.net.checks.back().result == cloud::Freshness::Expired);
        CHECK(r.net.acks.size() == 1);
        stopAck(r.net.acks[0], s, received, received ? "already_idle" : "request_expired");
        nowMs = 100; std::strcpy(r.link.status.activeExecutionId, executionB); r.d.stop(s, 7, nowMs);
        CHECK(r.link.stops.size() == (received ? 1u : 0u) && r.net.acks.size() == 1);
    });
    for (bool idle : {false, true}) for (bool received : {false, true})
        scenario(std::string("Stop matched target / ") + (idle ? "idle" : "execution") + (received ? " received" : " rejected"), [=] {
            Rig r; const auto s = stop(); blocked = "storage_fault";
            if (!idle) { std::strcpy(r.link.status.activeExecutionId, executionA); r.link.status.motionBusy = true; r.link.status.stationary = false; }
            r.d.stop(s, 7, nowMs); CHECK(r.link.stops.size() == 1 && !admissionCalls && !crypto::calls);
            const auto& target = r.link.stops[0]; CHECK(target.scope == (idle ? v4::StopScope::Idle : v4::StopScope::Product));
            CHECK(target.source == v4::Source::CloudCommand && target.sequence == s.sequence);
            CHECK(!std::strcmp(target.commandId, s.commandId));
            for (auto byte : target.executionId) CHECK(byte == (idle ? 0 : 0x11));
            CHECK(r.net.acks.empty()); r.poll(101); CHECK(r.net.acks.empty());
            r.link.stopState = received ? StopSendState::Received : StopSendState::Rejected; r.poll(102);
            CHECK(r.net.acks.size() == 1);
            stopAck(r.net.acks[0], s, received, received ? (idle ? "already_idle" : "accepted") : "stop_rejected");
            if (!idle) CHECK(r.link.status.motionBusy && !r.link.status.stationary); // Receipt is not stopped.
            r.d.stop(s, 7, nowMs); r.poll(103); CHECK(r.link.stops.size() == 1 && r.net.acks.size() == 1);
        });
    for (unsigned shape = 0; shape < 6; ++shape) scenario("Stop unavailable target / " + std::to_string(shape), [=] {
        Rig r;
        if (shape == 0) r.link.telemetry = false;
        if (shape == 1) r.link.telemetryFresh = false;
        if (shape == 2) r.link.status.motionBusy = true;
        if (shape == 3) r.link.status.stationary = false;
        if (shape == 4) std::strcpy(r.link.status.activeExecutionId, "invalid");
        if (shape == 5) r.link.board = false;
        r.d.stop(stop(), 7, nowMs); CHECK(r.link.stops.empty() && !admissionCalls && !crypto::calls);
        CHECK(r.net.acks.size() == 1); stopAck(r.net.acks[0], stop(), false, "motion_state_unavailable");
    });
    scenario("Stop cancels ordinary; uncertain original is only queried", [] {
        Rig r; const auto c = command(); r.start(c); blocked = "maintenance";
        std::strcpy(r.link.status.activeExecutionId, executionA);
        r.link.status.motionBusy = true; r.link.status.stationary = false;
        r.d.stop(stop(), 7, nowMs); CHECK(r.link.stops.size() == 1 && r.link.cancellations == 1 && r.d.busy());
        CHECK(admissionCalls == 1); r.link.stopState = StopSendState::Received; r.poll(101); r.poll(1101);
        CHECK(r.link.commands.size() == 1 && r.link.queries.size() == 1 && r.net.acks.size() == 1);
        r.link.completeQuery(known(c.request)); r.poll(1102); r.poll(1103);
        CHECK(r.net.acks.size() == 2); ack(r.net.acks[1], c, true, "accepted");
    });
    scenario("unpublished ordinary acceptance never blocks independent Stop", [] {
        Rig r; const auto c = command(); r.start(c); r.link.complete(result(c.request)); r.poll(101);
        r.net.publishAvailable = false; r.poll(102); CHECK(r.d.resultPending());
        const auto admissions = admissionCalls; blocked = "maintenance_active";
        r.d.stop(stop(), 7, nowMs);
        CHECK(r.link.stops.size() == 1 && admissionCalls == admissions && r.d.resultPending());
    });
    scenario("late Stop never rebinds from A to newer execution B", [] {
        Rig r; const auto s = stop(); std::strcpy(r.link.status.activeExecutionId, executionA);
        r.d.stop(s, 7, nowMs); std::strcpy(r.link.status.activeExecutionId, executionB);
        r.poll(101); r.link.stopState = StopSendState::Received; r.poll(102); r.d.stop(s, 7, nowMs);
        CHECK(r.link.stops.size() == 1); for (auto byte : r.link.stops[0].executionId) CHECK(byte == 0x11);
        CHECK(!std::strcmp(r.link.status.activeExecutionId, executionB));
    });
    for (auto state : {StopSendState::Idle, StopSendState::TimedOut, StopSendState::Unavailable})
        scenario("uncertain Stop receipt is not an acceptance or rejection / " + std::to_string(unsigned(state)), [=] {
            Rig r; const auto s = stop(); r.d.stop(s, 7, nowMs); r.link.stopState = state;
            r.poll(101); r.poll(10000); r.d.stop(s, 7, nowMs);
            CHECK(r.net.acks.empty() && r.link.stops.size() == 1);
        });
    scenario("failed Stop receipt ACK retries original session without resending Stop", [] {
        Rig r; const auto s = stop(); r.d.stop(s, 7, nowMs); r.net.publishAvailable = false;
        r.link.stopState = StopSendState::Received; r.poll(101); r.poll(102);
        CHECK(r.net.acks.size() == 2 && !r.net.acks.back().sent);
        r.net.session.close(); CHECK(r.net.session.open(sessionB, 8, 102));
        r.net.publishAvailable = true; r.poll(103); r.poll(104);
        CHECK(r.net.acks.size() == 3 && r.net.acks.back().sent && r.link.stops.size() == 1);
        for (const auto& a : r.net.acks) stopAck(a, s, true, "already_idle");
    });
    for (unsigned oldAck = 0; oldAck < 5; ++oldAck)
        scenario("ACK waiting for network never gates new explicit Stop / " + std::to_string(oldAck), [=] {
            Rig r; const auto old = stop(1); r.net.publishAvailable = false;
            if (oldAck == 1) {
                std::strcpy(r.link.status.activeExecutionId, executionA);
                r.link.status.motionBusy = true; r.link.status.stationary = false;
            }
            if (oldAck == 2) r.link.telemetry = false;
            if (oldAck == 3) r.link.allowStop = false;
            if (oldAck == 4) nowMs = 5101;
            r.d.stop(old, 7, nowMs);
            if (oldAck < 2) { r.link.stopState = StopSendState::Received; r.poll(101); }
            CHECK(r.net.acks.size() == 1 && !r.net.acks[0].sent);
            const auto actionsBefore = r.link.stops.size();
            r.link.telemetry = true; r.link.allowStop = true;
            std::strcpy(r.link.status.activeExecutionId, executionB);
            r.link.status.motionBusy = true; r.link.status.stationary = false;
            blocked = "storage_fault"; const auto admissions = admissionCalls;
            r.net.session.close(); CHECK(r.net.session.open(sessionB, 8, nowMs));
            auto next = stop(2, nowMs); std::strcpy(next.session, sessionB);
            r.d.stop(next, 8, nowMs);
            CHECK(r.link.stops.size() == actionsBefore + 1 && admissionCalls == admissions);
            for (auto byte : r.link.stops.back().executionId) CHECK(byte == 0x22);
            r.net.publishAvailable = true; r.poll(++nowMs);
            CHECK(r.net.acks.size() == 1); // Replaced informational ACK must not masquerade as new receipt.
            r.link.stopState = StopSendState::Received; r.poll(++nowMs);
            CHECK(r.net.acks.size() == 2 && r.net.acks.back().sent);
            stopAck(r.net.acks.back(), next, true, "accepted");
            r.d.stop(old, 7, nowMs); CHECK(r.link.stops.size() == actionsBefore + 1);
        });
    scenario("Stop busy rejection also consumes seq", [] {
        Rig r; const auto first = stop(1); const auto busy = stop(2);
        r.d.stop(first, 7, nowMs); r.d.stop(busy, 7, nowMs);
        CHECK(r.link.stops.size() == 1 && r.net.acks.size() == 1); stopAck(r.net.acks[0], busy, false, "busy");
        r.link.stopState = StopSendState::Received; r.poll(101); r.d.stop(busy, 7, nowMs);
        CHECK(r.link.stops.size() == 1); r.d.stop(stop(3), 7, nowMs); CHECK(r.link.stops.size() == 2);
    });
    scenario("Stop recheck before request blocks elapsed original deadline", [] {
        Rig r; clockHook = [] { nowMs = 5101; }; r.d.stop(stop(), 7, 100);
        CHECK(r.link.stops.empty());
    });
    for (bool wrap : {false, true}) for (uint32_t age : {4999u, 5000u, 5001u})
        scenario(std::string("Stop original TTL boundary / ") + (wrap ? "wrap / " : "linear / ") + std::to_string(age), [=] {
            Rig r;
            const uint32_t sampled = wrap ? UINT32_MAX - 100u : 100u;
            auto s = stop(2, sampled);
            if (wrap) {
                CHECK(r.net.session.open(sessionB, 7, sampled - 100u));
                std::strcpy(s.session, sessionB);
            }
            nowMs = sampled + age;
            r.d.stop(s, 7, nowMs);
            CHECK(r.link.stops.size() == (age <= 5000 ? 1u : 0u));
            CHECK(admissionCalls == 0 && crypto::calls == 0);
            if (age <= 5000) {
                CHECK(r.net.acks.empty()); r.link.stopState = StopSendState::Received;
                r.poll(nowMs + 1u); CHECK(r.net.acks.size() == 1);
                stopAck(r.net.acks[0], s, true, "already_idle");
            } else {
                CHECK(r.net.acks.size() == 1);
                stopAck(r.net.acks[0], s, false, "request_expired");
            }
        });
    scenario("COUNTEREXAMPLE Stop clock expires between freshness and requestStop", [] {
        Rig r; clockHook = [] { if (clockCalls >= 2) nowMs = 5101; };
        r.d.stop(stop(), 7, 100); CHECK(r.link.stops.empty());
    });
    for (unsigned fault = 0; fault < 2; ++fault)
        scenario("COUNTEREXAMPLE failed Stop local rejection ACK must retry / " + std::to_string(fault), [=] {
            Rig r; const auto s = stop(); r.net.publishAvailable = false;
            const char* reason = fault == 0 ? "motion_state_unavailable" : "busy";
            if (fault == 0) r.link.telemetry = false;
            if (fault == 1) r.link.allowStop = false;
            r.d.stop(s, 7, nowMs); CHECK(r.net.acks.size() == 1);
            r.net.publishAvailable = true; r.poll(101);
            CHECK(r.net.acks.size() == 2 && r.net.acks.back().sent);
            stopAck(r.net.acks.back(), s, false, reason);
            CHECK(r.link.stops.empty());
        });
    scenario("Stop transmission owns single slot; failed newer busy reply cannot overwrite it", [] {
        Rig r; const auto original = stop(1); const auto newer = stop(2);
        r.net.publishAvailable = false; r.d.stop(original, 7, nowMs); r.d.stop(newer, 7, nowMs);
        CHECK(r.link.stops.size() == 1 && r.net.acks.size() == 1 && !r.net.acks[0].sent);
        stopAck(r.net.acks[0], newer, false, "busy");
        r.net.publishAvailable = true; r.poll(101); CHECK(r.net.acks.size() == 1);
        r.link.stopState = StopSendState::Received; r.poll(102);
        CHECK(r.net.acks.size() == 2 && r.net.acks.back().sent);
        stopAck(r.net.acks.back(), original, true, "already_idle");
        r.d.stop(newer, 7, nowMs); CHECK(r.link.stops.size() == 1);
    });
}

// Test-only telemetry view over the real UART core, not ControllerLink/main or
// a synthetic result/state setter. No BrainNetwork worker/broker is involved.
struct TelemetryLink : ReadOnlyLink {
    const Status* lastTelemetry() const { return freshStatus(nowMs) ? &peerStatus() : nullptr; }
    uint32_t lastTelemetryReceivedAtMs() const { return peerStatusReceivedAtMs(); }
};
v4::Pairing pairing(v4::Role role) {
    v4::Pairing p; p.role = role; std::strcpy(p.deviceId, device); std::strcpy(p.epoch, sessionA);
    std::strcpy(p.localPhysicalId, role == v4::Role::Brain ? "112233445566" : "aabbccddeeff");
    std::strcpy(p.peerPhysicalId, role == v4::Role::Brain ? "aabbccddeeff" : "112233445566");
    return p;
}
ProductContext context() {
    ProductContext c; std::strcpy(c.deviceId, device); c.profileVersion = 10;
    std::strcpy(c.babyId, "original-baby"); std::strcpy(c.babyName, "Original baby");
    std::strcpy(c.formulaBrand, "Original formula"); c.waterMl = 120; c.temperatureC = 40; c.powderGPer100Ml = 13.5f;
    CHECK(validProductContext(c)); return c;
}
MotionStateStore* motionStore = nullptr;
bool motionAccepts = true;
unsigned commandCalls = 0, queryCalls = 0, stopCalls = 0, unknownQueryCalls = 0;
CommandMessage receivedCommand;
v4::StopRequest receivedStop;
bool commandHandler(const CommandMessage& c, uint32_t, CommandResult& output) {
    ++commandCalls; receivedCommand = c; CHECK(motionStore);
    CHECK(motionStore->recordDecision(c.request, motionAccepts, motionAccepts ? "accepted" : "not_ready",
                                     motionAccepts ? executionA : nullptr) == MotionWrite::Stored);
    output = result(c.request, motionAccepts); return true;
}
bool queryHandler(const ResultQuery& q, QueriedResult& output) {
    ++queryCalls; CHECK(motionStore);
    const bool answered = queryMotionResult(*motionStore, q, output);
    if (answered && output.status == ResultQueryStatus::Unknown) ++unknownQueryCalls;
    return answered;
}
bool stopHandler(const v4::StopRequest& s, uint32_t) { ++stopCalls; receivedStop = s; return true; }
struct ShortWire : v4::ByteSink {
    std::vector<uint8_t> bytes;
    v4::Parser observer;
    unsigned writes = 0, shorts = 0, zeros = 0, commands = 0, results = 0, queries = 0, stops = 0;
    bool idle() const override { return bytes.empty(); }
    size_t available() const override { return 128 - bytes.size(); }
    size_t write(const uint8_t* data, size_t length) override {
        CHECK(length <= available()); const size_t take = (++writes % 5) ? std::min(length, size_t(17)) : 0;
        if (take < length) ++shorts;
        if (!take) ++zeros;
        bytes.insert(bytes.end(), data, data + take); return take;
    }
    void deliver(ReadOnlyLink& peer, bool drop = false) {
        for (const auto byte : bytes) {
            v4::Frame f;
            if (observer.push(byte, nowMs, f) && !f.offset) {
                if (f.kind == v4::Kind::Command) ++commands;
                if (f.kind == v4::Kind::CommandResult || f.kind == v4::Kind::Result) ++results;
                if (f.kind == v4::Kind::ResultQuery) ++queries;
                if (f.kind == v4::Kind::Stop) ++stops;
            }
            if (!drop) peer.receive(byte, nowMs);
        }
        bytes.clear();
    }
};
void production() {
    scenario("production default unbegun noBoard cannot send commands or Stop", [] {
        TelemetryLink brain; FakeNetwork net; ShortWire wire;
        babytech::brain::BrainCloudDispatcher<TelemetryLink, FakeNetwork> d(brain, net, clockNow, admission);
        d.command(command(), 7, nowMs); d.stop(stop(), 7, nowMs); d.poll(nowMs); brain.poll(nowMs, wire);
        CHECK(!brain.configured() && !brain.connected(nowMs) && !brain.lastTelemetry());
        CHECK(brain.commandSendState() == CommandSendState::Idle && brain.stopSendState() == StopSendState::Idle);
        CHECK(!wire.writes && !d.busy() && net.acks.size() == 2);
        ack(net.acks[0], command(), false, "link_lost");
        stopAck(net.acks[1], stop(), false, "motion_state_unavailable");
    });
    for (unsigned mode = 0; mode < 5; ++mode)
        scenario("production dual-peer codec/short UART + real Store / " + std::to_string(mode), [=] {
            motionAccepts = mode != 1; commandCalls = queryCalls = stopCalls = unknownQueryCalls = 0;
            MotionStateStore store; const auto c = context();
            CHECK(store.installInitial(pairing(v4::Role::Motion), &c) == MotionWrite::Stored);
            nvs::reboot(); MotionStateStore loaded;
            CHECK(loaded.load(pairing(v4::Role::Motion)) == MotionLoad::Ready); motionStore = &loaded;
            BrainStateStore brainStore;
            if (mode == 3) {
                // Seed durable evidence independently of the canceled Cloud request.
                const auto historical = command(ProductCommand::ResetError, 5);
                CHECK(loaded.recordDecision(historical.request, false, "not_ready") == MotionWrite::Stored);
                CHECK(brainStore.installInitial(pairing(v4::Role::Brain), &c) == BrainWrite::Stored);
                auto local = command(ProductCommand::Clean).request;
                local.source = v4::Source::LocalTouch;
                CHECK(makeLocalCommandId(pairing(v4::Role::Brain), local.sequence, local.commandId));
                CHECK(brainStore.reserveLocal(local) == BrainWrite::Stored);
                CHECK(brainStore.state().pending && loaded.state().cloudSequence == 5);
            }
            const auto durableBrain = brainStore.state(); const auto durableMotion = loaded.state();
            TelemetryLink brain; ReadOnlyLink motion; ShortWire toMotion, toBrain; Status status;
            status.stationary = true;
            CHECK(brain.begin(pairing(v4::Role::Brain), 101) && motion.begin(pairing(v4::Role::Motion), 202));
            CHECK(motion.setCommandHandler(commandHandler) && motion.setResultQueryHandler(queryHandler));
            CHECK(motion.setStopHandler(stopHandler));
            auto step = [&](bool publishStatus = false, bool dropReply = false) {
                status.sampleUptimeMs = nowMs;
                brain.poll(nowMs, toMotion); motion.poll(nowMs, toBrain, publishStatus ? &status : nullptr);
                toMotion.deliver(motion); toBrain.deliver(brain, dropReply); ++nowMs;
            };
            for (unsigned tick = 0; tick < 400; ++tick) step(mode >= 3);
            CHECK(brain.connected(nowMs) && motion.connected(nowMs));
            if (mode < 3) CHECK(!brain.lastTelemetry());
            else CHECK(brain.lastTelemetry());
            FakeNetwork net; babytech::brain::BrainCloudDispatcher<TelemetryLink, FakeNetwork> d(brain, net, clockNow, admission);
            const auto request = command(ProductCommand::Prepare, mode == 3 ? 11 : 1, nowMs - 123);
            const uint32_t issued = nowMs;
            const auto callsBefore = nvs::io.calls.size();
            const auto diskBefore = nvs::io.disk;
            const auto setsBefore = nvs::count(nvs::Op::Set), commitsBefore = nvs::count(nvs::Op::Commit);
            if (mode != 4) { d.command(request, 7, nowMs); CHECK(d.busy()); }
            if (mode >= 3) {
                const auto s = stop(mode == 3 ? 12 : 2, nowMs); blocked = "storage_fault"; const auto beforeAdmission = admissionCalls;
                d.stop(s, 7, nowMs); CHECK(admissionCalls == beforeAdmission);
                for (unsigned tick = 0; tick < 300 && !stopCalls; ++tick) { step(true); d.poll(nowMs); }
                CHECK(stopCalls == 1 && toMotion.stops == 1 && receivedStop.scope == v4::StopScope::Idle);
                for (unsigned tick = 0; tick < 300 && net.acks.empty(); ++tick) { step(true); d.poll(nowMs); }
                CHECK(net.acks.size() == 1); stopAck(net.acks[0], s, true, "already_idle");
                CHECK(commandCalls == 0 && nvs::io.calls.size() == callsBefore);
                if (mode == 3) {
                    for (unsigned tick = 0; tick < 2400; ++tick) { step(true); d.poll(nowMs); }
                    CHECK(d.busy() && queryCalls >= 1 && unknownQueryCalls == queryCalls);
                    CHECK(commandCalls == 0 && toMotion.commands == 0 && net.acks.size() == 1);
                    CHECK(uint32_t(nowMs - request.sampledAtMs) <= request.ttlMs);
                    for (unsigned tick = 0; tick < 8000 && d.busy(); ++tick) {
                        step(true); d.poll(nowMs);
                        if (uint32_t(nowMs - request.sampledAtMs) <= request.ttlMs) CHECK(d.busy());
                    }
                    CHECK(!d.busy() && !d.resultPending() && net.acks.size() == 1);
                    CHECK(brain.connected(nowMs) && brain.lastTelemetry());
                    CHECK(uint32_t(brain.lastTelemetryReceivedAtMs() - request.sampledAtMs) > request.ttlMs);
                    CHECK(uint32_t(nowMs - brain.lastTelemetryReceivedAtMs()) < 1500);
                    CHECK(commandCalls == 0 && toMotion.commands == 0 && unknownQueryCalls == queryCalls);
                    CHECK(nvs::io.calls.size() == callsBefore && nvs::io.disk == diskBefore);
                    CHECK(nvs::count(nvs::Op::Set) == setsBefore && nvs::count(nvs::Op::Commit) == commitsBefore);
                    CHECK(sameBrainState(brainStore.state(), durableBrain) && brainStore.state().pending);
                    CHECK(sameMotionState(loaded.state(), durableMotion) && loaded.state().cloudSequence == 5);
                    auto replay = request; replay.sampledAtMs = nowMs;
                    d.command(replay, 7, nowMs); CHECK(!d.busy() && net.acks.size() == 1);
                    auto conflict = replay; conflict.request.sequence = 13;
                    d.command(conflict, 7, nowMs);
                    CHECK(net.acks.size() == 2); ack(net.acks[1], conflict, false, "request_conflict");
                    CHECK(nvs::io.calls.size() == callsBefore && nvs::io.disk == diskBefore);
                    blocked = nullptr;
                    const auto next = command(ProductCommand::Prepare, 14, nowMs);
                    d.command(next, 7, nowMs); d.command(next, 7, nowMs);
                    CHECK(d.busy() && commandCalls == 0 && nvs::io.calls.size() == callsBefore);
                    for (unsigned tick = 0; tick < 4500 && (d.busy() || d.resultPending()); ++tick) {
                        step(true); d.poll(nowMs);
                    }
                    CHECK(!d.busy() && !d.resultPending() && commandCalls == 1 && toMotion.commands == 1);
                    CHECK(net.acks.size() == 3); ack(net.acks[2], next, true, "accepted");
                    CHECK(sameProductRequest(receivedCommand.request, next.request));
                    CHECK(loaded.state().cloudSequence == 14 && loaded.state().slot.kind == MotionSlotKind::Intent);
                    CHECK(nvs::count(nvs::Op::Set) == setsBefore + 1 && nvs::count(nvs::Op::Commit) == commitsBefore + 1);
                    CHECK(nvs::io.disk != diskBefore && sameBrainState(brainStore.state(), durableBrain));
                    const auto afterNew = nvs::io.disk; const auto afterCalls = nvs::io.calls.size();
                    d.command(next, 7, nowMs); d.command(request, 7, nowMs);
                    for (unsigned tick = 0; tick < 400; ++tick) { step(true); d.poll(nowMs); }
                    CHECK(commandCalls == 1 && toMotion.commands == 1 && net.acks.size() == 3);
                    CHECK(nvs::io.disk == afterNew && nvs::io.calls.size() == afterCalls);
                }
            } else {
                for (unsigned tick = 0; tick < 4500 && (d.busy() || d.resultPending()); ++tick) {
                    const bool drop = mode == 2 && uint32_t(nowMs - issued) < 900;
                    step(false, drop); d.poll(nowMs);
                    if (mode == 2 && commandCalls) net.session.close();
                }
                CHECK(commandCalls == 1 && toMotion.commands == 1 && !d.busy() && !d.resultPending());
                CHECK(net.acks.size() == 1); ack(net.acks[0], request, motionAccepts, motionAccepts ? "accepted" : "not_ready");
                CHECK(sameProductRequest(receivedCommand.request, request.request));
                CHECK(receivedCommand.remainingTtlMs > 0 && receivedCommand.remainingTtlMs <= 5000 - 123 - 50);
                CHECK(queryCalls == (mode == 2 ? 1u : 0u));
                CHECK(toMotion.queries == queryCalls);
                const auto disk = nvs::io.disk; const auto calls = nvs::io.calls.size();
                for (unsigned tick = 0; tick < 400; ++tick) { step(); d.poll(nowMs); }
                CHECK(commandCalls == 1 && nvs::io.disk == disk && nvs::io.calls.size() == calls);
                // Result lookup must be read-only even after serialized reboot.
                nvs::reboot(); MotionStateStore rebooted;
                CHECK(rebooted.load(pairing(v4::Role::Motion)) == MotionLoad::Ready);
                const auto q = known(request.request); QueriedResult answer;
                CHECK(queryMotionResult(rebooted, q.query, answer) && answer.status == ResultQueryStatus::Known);
                CHECK(answer.accepted == motionAccepts && !std::strcmp(answer.requestDigestHex, q.requestDigestHex));
            }
            CHECK(toMotion.shorts && toBrain.shorts && toMotion.zeros && toBrain.zeros);
            std::printf("  UART commands=%u queries=%u results=%u stops=%u handlers=%u/%u/%u\n",
                        toMotion.commands, toMotion.queries, toBrain.results, toMotion.stops, commandCalls, queryCalls, stopCalls);
            motionStore = nullptr;
        }, true);
}

Status* simulatedStopStatus = nullptr;
bool persistedStopAccepted = true;
bool simulatedIdle = false;
std::vector<ResultQuery> persistedQueries;
// Separate from the nonpersistent callback used by all pre-existing fixtures.
// This models the persist-after-idle boundary, not actual MotionRuntime stopping.
bool persistedStopHandler(const v4::StopRequest& s, uint32_t) {
    ++stopCalls; receivedStop = s;
    CHECK(motionStore && simulatedStopStatus);
    simulatedStopStatus->motionBusy = false;
    simulatedStopStatus->stationary = true;
    simulatedStopStatus->activeExecutionId[0] = 0;
    simulatedIdle = true;
    char execution[33]{};
    if (s.scope == v4::StopScope::Product) {
        constexpr char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < sizeof(s.executionId); ++i) {
            execution[2 * i] = hex[s.executionId[i] >> 4];
            execution[2 * i + 1] = hex[s.executionId[i] & 15];
        }
    }
    const char* reason = persistedStopAccepted ?
        (s.scope == v4::StopScope::Idle ? "already_idle" : "accepted") : "stop_rejected";
    CHECK(simulatedIdle && simulatedStopStatus->stationary);
    CHECK(motionStore->recordCloudStop(s.sequence, s.commandId, execution, persistedStopAccepted,
                                     reason, simulatedStopStatus->stationary) == MotionWrite::Stored);
    return persistedStopAccepted;
}
bool persistedQueryHandler(const ResultQuery& q, QueriedResult& output) {
    ++queryCalls; persistedQueries.push_back(q); CHECK(motionStore && simulatedIdle);
    const auto calls = nvs::io.calls.size(); const auto disk = nvs::io.disk;
    const auto state = motionStore->state();
    CHECK(queryMotionResult(*motionStore, q, output));
    CHECK(nvs::io.calls.size() == calls && nvs::io.disk == disk);
    CHECK(sameMotionState(motionStore->state(), state));
    return true;
}
// Buffer only at the test wire boundary to selectively lose complete frames.
// Transmitter short/zero writes and both production parsers/codecs remain active.
struct FilteredReplyWire : ShortWire {
    std::vector<uint8_t> frameBytes;
    uint32_t lostResultId = 0;
    unsigned droppedReceipts = 0, droppedResults = 0;
    void deliverFiltered(ReadOnlyLink& peer, bool dropReceipt, bool dropFirstResult = false) {
        for (const auto byte : bytes) {
            frameBytes.push_back(byte); v4::Frame frame;
            if (!observer.push(byte, nowMs, frame)) continue;
            if (!frame.offset && frame.kind == v4::Kind::Result) ++results;
            const bool receipt = frame.kind == v4::Kind::LinkAck || frame.kind == v4::Kind::LinkReject;
            if (dropFirstResult && frame.kind == v4::Kind::Result && !lostResultId)
                lostResultId = frame.messageId;
            const bool loseResult = frame.kind == v4::Kind::Result && lostResultId && frame.messageId == lostResultId;
            if (dropReceipt && receipt) ++droppedReceipts;
            else if (loseResult) ++droppedResults;
            else for (const auto buffered : frameBytes) peer.receive(buffered, nowMs);
            frameBytes.clear();
        }
        bytes.clear();
    }
};
void stopRecoveryProduction() {
    for (unsigned mode = 0; mode < 3; ++mode)
        scenario("production persisted Stop lost receipt/query / " + std::to_string(mode), [=] {
            commandCalls = queryCalls = stopCalls = 0; persistedQueries.clear();
            simulatedIdle = false; persistedStopAccepted = mode != 2;
            MotionStateStore store; const auto c = context();
            CHECK(store.installInitial(pairing(v4::Role::Motion), &c) == MotionWrite::Stored);
            motionStore = &store;
            TelemetryLink brain; ReadOnlyLink motion; ShortWire toMotion; FilteredReplyWire toBrain; Status status;
            status.stationary = true; simulatedStopStatus = &status;
            CHECK(brain.begin(pairing(v4::Role::Brain), 101) && motion.begin(pairing(v4::Role::Motion), 202));
            CHECK(motion.setStopHandler(persistedStopHandler) && motion.setResultQueryHandler(persistedQueryHandler));
            auto step = [&](bool dropReceipt = false, bool dropQuery = false, bool telemetry = true) {
                status.sampleUptimeMs = nowMs;
                brain.poll(nowMs, toMotion); motion.poll(nowMs, toBrain, telemetry ? &status : nullptr);
                toMotion.deliver(motion); toBrain.deliverFiltered(brain, dropReceipt, dropQuery); ++nowMs;
            };
            for (unsigned tick = 0; tick < 400; ++tick) step();
            CHECK(brain.connected(nowMs) && motion.connected(nowMs) && brain.lastTelemetry());
            if (mode) {
                std::strcpy(status.activeExecutionId, executionA); status.motionBusy = true; status.stationary = false;
                for (unsigned tick = 0; tick < 600; ++tick) step();
                CHECK(brain.lastTelemetry() && !std::strcmp(brain.lastTelemetry()->activeExecutionId, executionA));
            }
            FakeNetwork net; babytech::brain::BrainCloudDispatcher<TelemetryLink, FakeNetwork> d(brain, net, clockNow, admission);
            const auto s = stop(19, nowMs); const auto sets = nvs::count(nvs::Op::Set);
            const auto commits = nvs::count(nvs::Op::Commit); blocked = "storage_fault";
            d.stop(s, 7, nowMs); CHECK(!admissionCalls && !d.busy());
            for (unsigned tick = 0; tick < 1800 && brain.stopSendState() == StopSendState::Pending; ++tick) {
                step(true); d.poll(nowMs);
            }
            CHECK(brain.stopSendState() == StopSendState::TimedOut);
            CHECK(simulatedIdle && stopCalls == 1 && toMotion.stops == 1 && net.acks.empty());
            CHECK(toBrain.droppedReceipts >= 1 && nvs::count(nvs::Op::Set) == sets + 1);
            CHECK(nvs::count(nvs::Op::Commit) == commits + 1);
            CHECK(store.state().cloudResult.kind == MotionResultKind::CloudStop && store.state().cloudSequence == s.sequence);
            const auto persisted = store.state();
            MotionStateStore rebooted;
            if (mode) {
                nvs::reboot(); CHECK(rebooted.load(pairing(v4::Role::Motion)) == MotionLoad::Ready);
                CHECK(sameMotionState(rebooted.state(), persisted)); motionStore = &rebooted;
                CHECK(motion.begin(pairing(v4::Role::Motion), 303));
                CHECK(motion.setStopHandler(persistedStopHandler) && motion.setResultQueryHandler(persistedQueryHandler));
                net.session.close(); CHECK(net.session.open(sessionB, 8, nowMs));
                // Current UART boot/session is independent of the original Cloud ACK session.
            }
            const auto disk = nvs::io.disk; const auto calls = nvs::io.calls.size();
            const auto querySets = nvs::count(nvs::Op::Set), queryCommits = nvs::count(nvs::Op::Commit);
            for (unsigned tick = 0; tick < 7000 && net.acks.empty(); ++tick) {
                // Mode 0 also loses one complete RESULT response; heartbeats still flow.
                step(false, mode == 0, false); d.poll(nowMs);
                CHECK(nvs::io.calls.size() == calls && nvs::io.disk == disk);
            }
            CHECK(brain.connected(nowMs) && motion.connected(nowMs));
            CHECK(!brain.lastTelemetry()); // Recovery adds no STATUS freshness gate.
            CHECK(net.acks.size() == 1 && !d.busy() && !d.resultPending());
            stopAck(net.acks[0], s, persistedStopAccepted, mode == 0 ? "already_idle" : mode == 1 ? "accepted" : "stop_rejected");
            CHECK(queryCalls >= (mode == 0 ? 2u : 1u) && toMotion.queries == queryCalls);
            CHECK(mode != 0 || toBrain.droppedResults > 0);
            for (const auto& q : persistedQueries) CHECK(sameResultQuery(q, stopIdentity(s)));
            const auto& recovered = brain.resultQueryResponse();
            CHECK(recovered.status == ResultQueryStatus::Known && !recovered.requestDigestHex[0]);
            CHECK(recovered.outcome == MotionOutcome::None && recovered.accepted == persistedStopAccepted);
            CHECK(sameMotionState(motionStore->state(), persisted));
            CHECK(nvs::count(nvs::Op::Set) == querySets && nvs::count(nvs::Op::Commit) == queryCommits);
            const auto queriesAfter = queryCalls;
            for (unsigned tick = 0; tick < 1800; ++tick) {
                d.stop(s, 7, nowMs); step(false, false, false); d.poll(nowMs);
            }
            CHECK(net.acks.size() == 1 && queryCalls == queriesAfter && stopCalls == 1 && toMotion.stops == 1);
            CHECK(!commandCalls && !toMotion.commands && nvs::io.calls.size() == calls && nvs::io.disk == disk);
            CHECK(toMotion.shorts && toMotion.zeros && toBrain.shorts && toBrain.zeros);
            std::printf("  persisted Stop mode=%u stops=%u queries=%u dropped-receipts=%u dropped-result-fragments=%u writes-during-query=0\n",
                        mode, toMotion.stops, queryCalls, toBrain.droppedReceipts, toBrain.droppedResults);
            motionStore = nullptr; simulatedStopStatus = nullptr;
        }, true);
}
void stopRecoveryProductionPreemption() {
    for (bool partial : {false, true})
        scenario(partial ? "production partial Stop query cancels/drains before fresh explicit Clean" :
                           "production unsent Stop query cancels for same-loop Clean", [=] {
            commandCalls = queryCalls = stopCalls = 0; persistedQueries.clear();
            simulatedIdle = false; persistedStopAccepted = motionAccepts = true;
            MotionStateStore store; const auto c = context();
            CHECK(store.installInitial(pairing(v4::Role::Motion), &c) == MotionWrite::Stored);
            motionStore = &store;
            TelemetryLink brain; ReadOnlyLink motion; ShortWire toMotion; FilteredReplyWire toBrain; Status status;
            status.stationary = true; simulatedStopStatus = &status;
            CHECK(brain.begin(pairing(v4::Role::Brain), 101) && motion.begin(pairing(v4::Role::Motion), 202));
            CHECK(motion.setStopHandler(persistedStopHandler) && motion.setResultQueryHandler(persistedQueryHandler));
            CHECK(motion.setCommandHandler(commandHandler));
            bool captureCanceledFrame = false;
            std::vector<uint8_t> canceledBytes;
            auto step = [&](bool dropReceipt = false) {
                status.sampleUptimeMs = nowMs;
                brain.poll(nowMs, toMotion); motion.poll(nowMs, toBrain, &status);
                if (captureCanceledFrame)
                    canceledBytes.insert(canceledBytes.end(), toMotion.bytes.begin(), toMotion.bytes.end());
                toMotion.deliver(motion); toBrain.deliverFiltered(brain, dropReceipt); ++nowMs;
            };
            for (unsigned tick = 0; tick < 400; ++tick) step();
            CHECK(brain.connected(nowMs) && motion.connected(nowMs) && brain.lastTelemetry());
            FakeNetwork net; babytech::brain::BrainCloudDispatcher<TelemetryLink, FakeNetwork> d(brain, net, clockNow, admission);
            const auto s = stop(19, nowMs); d.stop(s, 7, nowMs);
            for (unsigned tick = 0; tick < 1800 && brain.stopSendState() == StopSendState::Pending; ++tick) {
                step(true); d.poll(nowMs);
            }
            CHECK(stopCalls == 1 && toMotion.stops == 1 && toBrain.droppedReceipts >= 1);
            CHECK(brain.stopSendState() == StopSendState::TimedOut && net.acks.empty());
            const auto persisted = store.state(); const auto disk = nvs::io.disk;
            const auto calls = nvs::io.calls.size();
            for (unsigned tick = 0; tick < 1800 && brain.resultLookupState() != ResultLookupState::Pending; ++tick) {
                step(); d.poll(nowMs);
            }
            // poll queued the informational Stop query, but no UART pump has run since.
            CHECK(d.busy() && brain.resultLookupState() == ResultLookupState::Pending);
            CHECK(!queryCalls && !toMotion.queries && toMotion.bytes.empty());
            if (partial) {
                // Pump until the canceled QUERY's first bytes, not a priority control frame, are on wire.
                bool started = false;
                for (unsigned tick = 0; tick < 100 && !started; ++tick) {
                    brain.poll(nowMs, toMotion);
                    started = toMotion.bytes.size() >= 6 && !std::memcmp(toMotion.bytes.data(), "BTM4", 4) &&
                        toMotion.bytes[5] == uint8_t(v4::Kind::ResultQuery);
                    if (!started) { toMotion.deliver(motion); ++nowMs; }
                }
                CHECK(started && toMotion.bytes.size() < v4::kHeaderSize);
            }
            const auto initial = command(ProductCommand::Clean, 20, nowMs);
            d.command(initial, 7, nowMs);
            CHECK(brain.resultLookupState() == ResultLookupState::Idle && !queryCalls);
            auto accepted = initial;
            if (partial) {
                // No command queue is promised while real partial bytes still need draining.
                CHECK(net.acks.size() == 1 && !d.busy()); ack(net.acks[0], initial, false, "busy");
                CHECK(brain.commandSendState() != CommandSendState::Pending);
                captureCanceledFrame = true;
                for (unsigned tick = 0; tick < 40; ++tick) step();
                captureCanceledFrame = false;
                CHECK(canceledBytes.size() >= v4::kHeaderSize);
                const size_t frameSize = v4::kHeaderSize + size_t(canceledBytes[32]) +
                    (size_t(canceledBytes[33]) << 8) + 2;
                CHECK(frameSize <= canceledBytes.size());
                v4::Parser poisoned, restored; v4::Frame frame; unsigned restoredFrames = 0;
                for (size_t i = 0; i < frameSize; ++i) {
                    CHECK(!poisoned.push(canceledBytes[i], nowMs, frame));
                    const uint8_t original = i == frameSize - 1 ? uint8_t(canceledBytes[i] ^ 1) : canceledBytes[i];
                    if (restored.push(original, nowMs, frame)) {
                        ++restoredFrames; CHECK(frame.kind == v4::Kind::ResultQuery && frame.offset == 0);
                    }
                }
                CHECK(restoredFrames == 1); // Exactly the CRC changed; no control/command bytes interleaved.
                CHECK(!commandCalls && !queryCalls && !toMotion.commands && !toMotion.queries);
                CHECK(sameMotionState(store.state(), persisted) && nvs::io.calls.size() == calls && nvs::io.disk == disk);
                // Retry the consumed identity cannot silently enqueue it after the drain.
                d.command(initial, 7, nowMs); CHECK(net.acks.size() == 1 && !d.busy());
                accepted = command(ProductCommand::Clean, 21, nowMs);
                d.command(accepted, 7, nowMs);
            } else CHECK(net.acks.empty());
            CHECK(d.busy() && brain.commandSendState() == CommandSendState::Pending);
            CHECK(nvs::io.calls.size() == calls && nvs::io.disk == disk);
            const auto sets = nvs::count(nvs::Op::Set), commits = nvs::count(nvs::Op::Commit);
            for (unsigned tick = 0; tick < 900 && (d.busy() || d.resultPending()); ++tick) {
                step(); d.poll(nowMs);
                if (commandCalls && !d.resultPending() && net.acks.size() == (partial ? 2u : 1u)) break;
            }
            CHECK(commandCalls == 1 && toMotion.commands == 1 && !queryCalls && !toMotion.queries);
            CHECK(sameProductRequest(receivedCommand.request, accepted.request));
            CHECK(net.acks.size() == (partial ? 2u : 1u)); ack(net.acks.back(), accepted, true, "accepted");
            CHECK(nvs::count(nvs::Op::Set) == sets + 1 && nvs::count(nvs::Op::Commit) == commits + 1);
            const auto afterCommand = nvs::io.disk; const auto afterCalls = nvs::io.calls.size();
            bool sawExpired = false;
            for (unsigned tick = 0; tick < 3000; ++tick) {
                step();
                if (brain.resultLookupState() == ResultLookupState::Complete) {
                    CHECK(sameResultQuery(brain.resultQueryResponse().query, stopIdentity(s)));
                    CHECK(brain.resultQueryResponse().status == ResultQueryStatus::Expired);
                    sawExpired = true;
                }
                d.poll(nowMs);
            }
            CHECK(queryCalls >= 1 && toMotion.queries == queryCalls);
            for (const auto& q : persistedQueries) CHECK(sameResultQuery(q, stopIdentity(s)));
            // New ordinary evidence superseded the Stop: Expired is not a fabricated Stop ACK.
            CHECK(sawExpired);
            CHECK(net.acks.size() == (partial ? 2u : 1u) && commandCalls == 1 && toMotion.commands == 1);
            CHECK(stopCalls == 1 && toMotion.stops == 1 && brain.healthy() && motion.healthy());
            CHECK(nvs::io.calls.size() == afterCalls && nvs::io.disk == afterCommand);
            std::printf("  preemption partial=%u commands=%u stop-query-handlers=%u stops=%u\n",
                        unsigned(partial), toMotion.commands, queryCalls, toMotion.stops);
            motionStore = nullptr; simulatedStopStatus = nullptr;
        }, true);
}
void stopRecovery() {
    stopRecoveryFaults(); stopRecoveryArbitration(); stopRecoveryProduction(); stopRecoveryProductionPreemption();
}
} // namespace

int main(int argc, char** argv) {
    const std::string selected = argc == 2 ? argv[1] : "all";
    const struct { const char* name; void (*run)(); } groups[] = {
        {"basics", basics}, {"freshness", freshness}, {"admission", admissionAndReplay},
        {"results", results}, {"expired", expiredUnknown}, {"replies", replyRetries},
        {"stops", stops}, {"production", production}, {"stop_recovery", stopRecovery},
        {"acceptance", acceptanceCallbacks}};
    bool found = selected == "all";
    for (const auto& g : groups) if (selected == "all" || selected == g.name) { found = true; g.run(); }
    if (!found || argc > 2) { std::fprintf(stderr, "Unknown test group\n"); return 2; }
    std::printf("Brain cloud dispatcher: %u scenarios, %u passed, %u failed; mbedTLS SHA major %d\n",
                scenarios, scenarios - failures, failures, MBEDTLS_VERSION_MAJOR);
    std::puts("LIMIT: component tests only; not real main/broker/Network worker/Flash/CAN or stationary proof");
    return failures ? 1 : 0;
}
