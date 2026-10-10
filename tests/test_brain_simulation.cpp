#include "brain_simulation_console.h"

#include <ArduinoJson.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>

using namespace babytech::boardlink;
using namespace babytech::brain;
namespace v4 = babytech::v4;

static_assert(!std::is_copy_constructible<BrainSimulation>::value, "single RAM owner");
static_assert(BrainSimulation::kResultCapacity == 4, "four terminal slots including active reservation");

namespace {
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + \
    std::to_string(__LINE__) + ": " #x); } while (false)

template <size_t N> void text(char (&out)[N], const std::string& value) {
    CHECK(value.size() < N);
    std::memset(out, 0, N);
    std::memcpy(out, value.data(), value.size());
}
v4::Pairing pairing() {
    v4::Pairing p;
    p.role = v4::Role::Brain;
    text(p.deviceId, "Babytech_01");
    text(p.epoch, "0123456789abcdef0123456789abcdef");
    text(p.localPhysicalId, "aabbccddeeff");
    text(p.peerPhysicalId, "112233445566");
    CHECK(v4::validPairing(p));
    return p;
}
ProductRequest request(uint64_t seq = 1, v4::Source source = v4::Source::CloudCommand) {
    ProductRequest r;
    r.command = ProductCommand::Prepare;
    r.source = source;
    r.sequence = seq;
    text(r.deviceId, "Babytech_01");
    if (source == v4::Source::LocalTouch) CHECK(makeLocalCommandId(pairing(), seq, r.commandId));
    else text(r.commandId, "cloud-" + std::to_string(seq));
    text(r.babyId, "baby-7");
    r.profileVersion = 7;
    r.waterMl = 120;
    r.temperatureC = 40;
    r.powderGPer100Ml = 13.123456f;
    CHECK(validProductRequest(r));
    return r;
}
void enable(BrainSimulation& s) {
    CHECK(s.setEnabled(true, false) == SimulationModeResult::Changed);
}
std::string encoded(const TerminalEvent& t) {
    v4::Message m;
    CHECK(encodeTerminalEvent(pairing(), t, m));
    CHECK(m.kind == v4::Kind::Terminal && !m.senderBoot && !m.receiverBoot && !m.messageId);
    TerminalEvent decoded;
    CHECK(decodeTerminalEvent(m, pairing(), decoded));
    CHECK(sameProductRequest(t.request, decoded.request));
    CHECK(t.targetPowderG == decoded.targetPowderG && t.uptimeMs == decoded.uptimeMs);
    CHECK(t.completed == decoded.completed && !std::strcmp(t.eventId, decoded.eventId));
    CHECK(!std::strcmp(t.reason, decoded.reason) && !std::strcmp(t.errorCode, decoded.errorCode));
    v4::Message again;
    CHECK(encodeTerminalEvent(pairing(), decoded, again));
    CHECK(m.length == again.length && !std::memcmp(m.payload, again.payload, m.length));
    return {reinterpret_cast<const char*>(m.payload), m.length};
}
CloudReceipt stored(const TerminalEvent& t) {
    CloudReceipt receipt;
    text(receipt.deviceId, t.request.deviceId);
    text(receipt.eventId, t.eventId);
    v4::Message m;
    CHECK(encodeCloudReceipt(receipt, m));
    CloudReceipt decoded, bytesDecoded;
    CHECK(decodeCloudReceipt(m, "Babytech_01", decoded));
    CHECK(decodeCloudReceipt(m.payload, m.length, "Babytech_01", bytesDecoded));
    CHECK(!std::strcmp(decoded.eventId, bytesDecoded.eventId));
    return decoded;
}
std::string results(const BrainSimulation& s) {
    std::string value;
    for (size_t i = 0; i < s.resultCount(); ++i) value += encoded(*s.result(i)) + '\n';
    CHECK(s.result(s.resultCount()) == nullptr);
    return value;
}

void mode() {
    BrainSimulation s(pairing());
    CHECK(!s.enabled() && !s.running() && !s.canStart() && !s.activeRequest());
    CHECK(s.durationMs() == 15000 && s.resultCount() == 0 && !s.result(0));
    CHECK(s.start(request(), 0) == SimulationStart::Disabled);
    s.poll(UINT32_MAX);
    CHECK(!s.stop(0));
    CHECK(s.setEnabled(true, true) == SimulationModeResult::Busy && !s.enabled());
    CHECK(s.setEnabled(false, true) == SimulationModeResult::Unchanged);
    enable(s);
    CHECK(s.canStart() && s.setEnabled(true, true) == SimulationModeResult::Unchanged);
    CHECK(s.start(request(), 0) == SimulationStart::Accepted);
    const auto active = *s.activeRequest();
    CHECK(s.setEnabled(false, false) == SimulationModeResult::Busy);
    CHECK(sameProductRequest(active, *s.activeRequest()) && s.resultCount() == 0);
    CHECK(s.stop(3));
    const auto evidence = results(s);
    CHECK(s.setEnabled(false, true) == SimulationModeResult::Busy);
    CHECK(results(s) == evidence && s.enabled());
    CHECK(s.setEnabled(false, false) == SimulationModeResult::Changed);
    CHECK(s.start(request(2), 4) == SimulationStart::Disabled && results(s) == evidence);
    enable(s);
    CHECK(results(s) == evidence && s.canStart());
}

void timer() {
    BrainSimulation zero(pairing(), 0), tooLong(pairing(), uint32_t(INT32_MAX) + 1);
    for (auto* s : {&zero, &tooLong}) {
        enable(*s);
        CHECK(!s->canStart() && s->start(request(), 0) == SimulationStart::Invalid);
        CHECK(!s->running() && !s->resultCount());
    }
    BrainSimulation s(pairing(), 10);
    enable(s);
    CHECK(s.start(request(), 0) == SimulationStart::Accepted);
    for (uint32_t time : {0u, 0u, 9u}) { s.poll(time); CHECK(s.running() && !s.resultCount()); }
    s.poll(10);
    CHECK(!s.running() && !s.activeRequest() && s.resultCount() == 1);
    CHECK(s.result(0)->completed && s.result(0)->uptimeMs == 10);
    const auto frozen = results(s);
    for (uint32_t time : {10u, 11u, UINT32_MAX}) s.poll(time);
    CHECK(!s.stop(12) && results(s) == frozen);

    BrainSimulation wrap(pairing(), 10);
    enable(wrap);
    CHECK(wrap.start(request(), UINT32_MAX - 4) == SimulationStart::Accepted);
    wrap.poll(UINT32_MAX);
    wrap.poll(0);
    wrap.poll(4);
    CHECK(wrap.running() && !wrap.resultCount());
    wrap.poll(5);
    CHECK(wrap.resultCount() == 1 && wrap.result(0)->uptimeMs == 5);
    CHECK(wrap.result(0)->completed);

    BrainSimulation maximum(pairing(), INT32_MAX);
    enable(maximum);
    CHECK(maximum.canStart() && maximum.start(request(), 0) == SimulationStart::Accepted);
    maximum.poll(uint32_t(INT32_MAX) - 1);
    CHECK(maximum.running());
    maximum.poll(INT32_MAX);
    CHECK(maximum.resultCount() == 1 && maximum.result(0)->completed);
}

void stop() {
    for (const auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch}) {
        BrainSimulation s(pairing(), 10);
        enable(s);
        CHECK(s.start(request(1, source), 0) == SimulationStart::Accepted);
        // Stop takes effect even at the deadline if poll has not completed yet.
        CHECK(s.stop(10));
        CHECK(!s.running() && s.resultCount() == 1 && !s.result(0)->completed);
        CHECK(s.result(0)->uptimeMs == 10);
        CHECK(!std::strcmp(s.result(0)->reason, "stopped"));
        CHECK(!std::strcmp(s.result(0)->errorCode, "E_STOPPED"));
        const auto before = results(s);
        s.poll(10);
        s.poll(100000);
        CHECK(!s.stop(20) && results(s) == before);
        CHECK(before.find("feeding_completed") == std::string::npos);
        CHECK(before.find("feeding_failed") != std::string::npos);
    }
}

void snapshot() {
    for (const auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch}) {
        auto p = pairing();
        BrainSimulation s(p, 10);
        text(p.deviceId, "changed-after-construction");
        enable(s);
        auto input = request(42, source);
        text(input.babyId, "baby-\"\\\n\001");
        const auto original = input;
        CHECK(s.start(input, 100) == SimulationStart::Accepted);
        input = request(43, source);
        input.waterMl = 300;
        input.temperatureC = 50;
        input.powderGPer100Ml = 20;
        input.profileVersion = 8;
        CHECK(sameProductRequest(*s.activeRequest(), original));
        s.poll(117);
        const auto t = *s.result(0);
        CHECK(sameProductRequest(t.request, original) && t.uptimeMs == 117);
        CHECK(t.targetPowderG == 15.7f); // Existing target contract rounds to 0.1 g.
        const auto bytes = encoded(t);
        DynamicJsonDocument doc(8192);
        CHECK(!deserializeJson(doc, bytes));
        CHECK(std::string(doc["baby_id"].as<const char*>()) == original.babyId);
        CHECK(doc["water_ml"].as<unsigned>() == 120 && doc["temp"].as<unsigned>() == 40);
        CHECK(doc["feeding_context_profile_version"].as<unsigned>() == 7);
        CHECK(doc["uptime_ms"].as<uint32_t>() == 117);
        CHECK(doc["powder_g_per_100ml"].as<float>() == original.powderGPer100Ml);
        CHECK(doc["target_powder_g"].as<float>() == t.targetPowderG);
        CHECK(doc.containsKey("dispensed_water_ml") && doc["dispensed_water_ml"].isNull());
        CHECK(std::string(doc["water_delivery_basis"].as<const char*>()) == "estimated_turns");
        CHECK(doc.containsKey("command_seq") == (source == v4::Source::CloudCommand));
        if (source == v4::Source::CloudCommand) CHECK(std::string(doc["command_seq"].as<const char*>()) == "42");
        char expected[59]{};
        CHECK(makeProductEventId(pairing(), source, 42, expected));
        CHECK(!std::strcmp(t.eventId, expected));
        CHECK(s.start(input, 200) == SimulationStart::Accepted);
        s.poll(1000);
        CHECK(encoded(*s.result(0)) == bytes && s.resultCount() == 2);
        CHECK(s.setEnabled(false, false) == SimulationModeResult::Changed);
        CHECK(encoded(*s.result(0)) == bytes);
    }
}

void deduplication() {
    for (const auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch}) {
        BrainSimulation s(pairing(), 1);
        enable(s);
        const auto original = request(5, source);
        CHECK(s.start(original, 10) == SimulationStart::Accepted);
        CHECK(s.start(original, 100) == SimulationStart::Duplicate);
        auto changed = original;
        changed.waterMl = 150;
        CHECK(s.start(changed, 100) == SimulationStart::Conflict);
        auto differentId = original;
        text(differentId.commandId, "other-command");
        CHECK(s.start(differentId, 100) == SimulationStart::Conflict);
        s.poll(11); // Duplicate starts must not restart the original timer.
        CHECK(s.resultCount() == 1 && s.result(0)->uptimeMs == 11);
        CHECK(s.start(original, 20) == SimulationStart::Duplicate);
        CHECK(s.start(changed, 20) == SimulationStart::Conflict);
        CHECK(s.acknowledge(stored(*s.result(0))));
        CHECK(s.start(original, 30) == SimulationStart::Duplicate && !s.running());
        CHECK(s.start(changed, 30) == SimulationStart::Conflict);
        CHECK(s.start(request(6, source), 30) == SimulationStart::Accepted);
        s.poll(31);
        CHECK(s.acknowledge(stored(*s.result(0))));
        CHECK(s.start(original, 32) == SimulationStart::Expired && !s.running());
        CHECK(s.start(request(4, source), 32) == SimulationStart::Expired);
    }
    BrainSimulation s(pairing(), 1);
    enable(s);
    CHECK(s.start(request(5), 10) == SimulationStart::Accepted);
    s.poll(11);
    auto local = request(5, v4::Source::LocalTouch);
    CHECK(s.start(local, 20) == SimulationStart::Accepted); // Sequences are per source.
    s.poll(21);
    auto collision = request(6);
    text(collision.commandId, local.commandId);
    CHECK(s.start(collision, 22) == SimulationStart::Conflict);
    CHECK(s.start(collision, 30) == SimulationStart::Conflict && !s.running());
    CHECK(s.start(request(6), 30) == SimulationStart::Conflict);
    CHECK(s.start(request(7), 30) == SimulationStart::Accepted);
}

void rejected_admission() {
    for (const auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch}) {
        BrainSimulation busy(pairing());
        enable(busy);
        const auto a = request(1, source), b = request(2, source);
        CHECK(busy.start(a, 0) == SimulationStart::Accepted);
        CHECK(busy.start(b, 14999) == SimulationStart::Busy);
        busy.poll(15000);
        const auto before = results(busy);
        CHECK(busy.start(b, 15000) == SimulationStart::Busy);
        CHECK(!busy.running() && results(busy) == before && busy.canStart());
        CHECK(busy.acknowledge(stored(*busy.result(0))));
        CHECK(busy.start(a, 15001) == SimulationStart::Expired);
        CHECK(busy.start(b, 15001) == SimulationStart::Busy && !busy.running());
        CHECK(busy.start(request(3, source), 15001) == SimulationStart::Accepted);

        BrainSimulation full(pairing(), 1);
        enable(full);
        for (uint64_t seq = 1; seq <= 4; ++seq) {
            CHECK(full.start(request(seq, source), uint32_t(seq * 10)) == SimulationStart::Accepted);
            full.poll(uint32_t(seq * 10 + 1));
        }
        const auto fifth = request(5, source);
        CHECK(full.start(fifth, 50) == SimulationStart::Full);
        CHECK(full.acknowledge(stored(*full.result(1))));
        CHECK(full.canStart() && full.resultCount() == 3);
        CHECK(full.start(fifth, 51) == SimulationStart::Full);
        CHECK(!full.running() && full.resultCount() == 3);
        CHECK(full.start(request(6, source), 51) == SimulationStart::Accepted);

    }
    BrainSimulation unavailable(pairing(), 1);
    enable(unavailable);
    auto rejected = request(2, v4::Source::LocalTouch);
    text(rejected.commandId, "noncanonical-local");
    CHECK(unavailable.start(rejected, 0) == SimulationStart::Unavailable);
    CHECK(unavailable.start(rejected, 10) == SimulationStart::Unavailable);
    CHECK(unavailable.start(request(1, v4::Source::LocalTouch), 10) == SimulationStart::Expired);
    CHECK(unavailable.start(request(2, v4::Source::LocalTouch), 10) == SimulationStart::Conflict);
    CHECK(unavailable.start(request(3, v4::Source::LocalTouch), 10) == SimulationStart::Accepted);
}

void rejected_conflict() {
    std::string counterexamples;
    for (const auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch}) {
        BrainSimulation s(pairing(), 1);
        enable(s);
        const auto a = request(1, source), b = request(2, source);
        CHECK(s.start(a, 0) == SimulationStart::Accepted);
        s.poll(1);
        auto reusedId = b;
        text(reusedId.commandId, a.commandId);
        CHECK(s.start(reusedId, 2) == SimulationStart::Conflict);
        CHECK(s.acknowledge(stored(*s.result(0))));
        CHECK(s.start(reusedId, 3) == SimulationStart::Conflict);
        CHECK(s.start(b, 3) == SimulationStart::Conflict);
        const auto actual = s.start(a, 3);
        CHECK(!s.running() && !s.resultCount());
        if (actual != SimulationStart::Expired) {
            counterexamples += std::string(source == v4::Source::CloudCommand ? "cloud" : "local") +
                ": seq=1 completed+stored after seq=2 same-ID Conflict; older seq=1 expected Expired, got enum=" +
                std::to_string(int(actual)) + "; ";
        }
        CHECK(s.start(request(3, source), 3) == SimulationStart::Accepted);
    }
    if (!counterexamples.empty()) throw std::runtime_error(counterexamples);
}

void invalid_decision() {
    std::string counterexamples;
    for (const auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch}) {
        BrainSimulation s(pairing(), 1);
        enable(s);
        auto invalid = request(2, source);
        invalid.command = ProductCommand::Clean;
        invalid.babyId[0] = 0;
        invalid.profileVersion = invalid.waterMl = invalid.temperatureC = 0;
        invalid.powderGPer100Ml = 0;
        CHECK(validProductRequest(invalid));
        CHECK(s.start(invalid, 0) == SimulationStart::Invalid);
        CHECK(s.start(invalid, 1) == SimulationStart::Invalid);
        const auto actual = s.start(request(1, source), 2);
        if (actual != SimulationStart::Expired) {
            counterexamples += "valid-identity Clean seq=2 rejected Invalid, then Prepare seq=1 "
                "expected Expired, got enum=" + std::to_string(int(actual)) +
                " source=" + (source == v4::Source::CloudCommand ? "cloud" : "local") + "; ";
            continue;
        }
        CHECK(!s.running() && !s.resultCount());
        CHECK(s.start(request(3, source), 3) == SimulationStart::Accepted);
    }
    if (!counterexamples.empty()) throw std::runtime_error(counterexamples);
}

void capacity_receipts() {
    BrainSimulation s(pairing(), 1), other(pairing(), 1);
    enable(s);
    enable(other);
    CHECK(other.start(request(99), 0) == SimulationStart::Accepted);
    other.poll(1);
    const auto otherBefore = results(other);
    std::array<CloudReceipt, 4> receipts;
    std::array<std::string, 4> bytes;
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(s.canStart()); // Old unacknowledged results must not block the next bottle.
        const auto r = request(i + 1, i % 2 ? v4::Source::LocalTouch : v4::Source::CloudCommand);
        CHECK(s.start(r, i * 10) == SimulationStart::Accepted);
        CHECK(!s.canStart() && s.resultCount() == i);
        CHECK(s.setEnabled(false, false) == SimulationModeResult::Busy);
        if (i == 3) CHECK(s.stop(i * 10)); // Fourth active slot already reserved for failure.
        else s.poll(i * 10 + 1);
        CHECK(s.resultCount() == i + 1);
        receipts[i] = stored(*s.result(i));
        bytes[i] = encoded(*s.result(i));
    }
    CHECK(!s.canStart() && s.start(request(100), 50) == SimulationStart::Full);
    CHECK(s.start(request(3), 50) == SimulationStart::Duplicate);
    const auto all = results(s);
    CHECK(s.setEnabled(false, false) == SimulationModeResult::Changed);
    CHECK(results(s) == all);
    enable(s);
    CHECK(results(s) == all && !s.canStart());
    auto wrongDevice = receipts[1];
    text(wrongDevice.deviceId, "Other_device");
    CHECK(!s.acknowledge(wrongDevice));
    CHECK(!s.acknowledge(stored(*other.result(0))));
    auto wrongEvent = receipts[1];
    wrongEvent.eventId[4] = 'f';
    CHECK(!s.acknowledge(wrongEvent));
    auto unterminated = receipts[0];
    std::memset(unterminated.deviceId, 'x', sizeof(unterminated.deviceId));
    CHECK(!s.acknowledge(unterminated));
    unterminated = receipts[0];
    std::memset(unterminated.eventId, 'x', sizeof(unterminated.eventId));
    CHECK(!s.acknowledge(unterminated));
    CHECK(results(s) == all && results(other) == otherBefore);
    CHECK(s.acknowledge(receipts[2])); // Out of order, stable order of remaining bytes.
    CHECK(s.resultCount() == 3 && results(s) == bytes[0] + '\n' + bytes[1] + '\n' + bytes[3] + '\n');
    CHECK(!s.acknowledge(receipts[2]) && s.resultCount() == 3 && s.canStart());
    CHECK(s.start(request(100), 60) == SimulationStart::Full && !s.running());
    CHECK(s.start(request(101), 60) == SimulationStart::Accepted);
    CloudReceipt activeReceipt;
    text(activeReceipt.deviceId, "Babytech_01");
    CHECK(makeProductEventId(pairing(), v4::Source::CloudCommand, 101, activeReceipt.eventId));
    CHECK(!s.acknowledge(activeReceipt) && s.running());
    CHECK(!s.acknowledge(stored(*other.result(0))) && s.running());
    CHECK(s.acknowledge(receipts[0]) && s.running() && s.activeRequest()->sequence == 101);
    s.poll(61);
    CHECK(s.resultCount() == 3);
    const auto latest = stored(*s.result(2));
    CHECK(s.acknowledge(receipts[3]) && s.acknowledge(receipts[1]));
    CHECK(s.resultCount() == 1 && s.result(0)->request.sequence == 101);
    CHECK(s.acknowledge(latest) && !s.resultCount() && s.canStart());
    CHECK(!s.acknowledge(latest) && results(other) == otherBefore);
}

void validation() {
    auto motion = pairing(), corrupt = pairing();
    motion.role = v4::Role::Motion;
    corrupt.epoch[0] = 'x';
    for (const auto& p : {motion, corrupt}) {
        BrainSimulation s(p);
        enable(s);
        CHECK(!s.canStart() && s.start(request(), 0) == SimulationStart::Invalid);
    }
    BrainSimulation s(pairing());
    enable(s);
    auto wrong = request();
    text(wrong.deviceId, "Other_device");
    CHECK(s.start(wrong, 0) == SimulationStart::Invalid);
    wrong = request();
    wrong.waterMl = 0;
    CHECK(s.start(wrong, 0) == SimulationStart::Invalid);
    wrong = request();
    wrong.command = ProductCommand::Clean;
    wrong.babyId[0] = 0;
    wrong.profileVersion = wrong.waterMl = wrong.temperatureC = 0;
    wrong.powderGPer100Ml = 0;
    // Legal unsupported commands consume sequence; isolate from malformed admission.
    BrainSimulation unsupported(pairing());
    enable(unsupported);
    CHECK(validProductRequest(wrong) && unsupported.start(wrong, 0) == SimulationStart::Invalid);
    wrong = request(1, v4::Source::LocalTouch);
    text(wrong.commandId, "noncanonical-local");
    CHECK(validProductRequest(wrong) && s.start(wrong, 0) == SimulationStart::Unavailable);
    CHECK(s.canStart() && !s.running() && !s.resultCount());
    CHECK(s.start(request(), 0) == SimulationStart::Accepted); // Local rejection does not consume Cloud sequence.
}

void console() {
    BrainSimulation s(pairing(), 10);
    char out[128]{};
    CHECK(BrainSimulationConsole::handle("SIM STATUS", s, false, out, sizeof(out)));
    CHECK(std::string(out) == "[simulation] off running=no pending=0 duration_ms=10 ram_only\n");
    const char* nonSim[] = {nullptr, "", "SIM", "sim ON", " SIM ON", "PING", "SIM\tON"};
    for (const char* line : nonSim) {
        std::strcpy(out, "untouched");
        CHECK(!BrainSimulationConsole::handle(line, s, false, out, sizeof(out)));
        CHECK(std::string(out) == "untouched" && !s.enabled());
    }
    for (const char* line : {"SIM on", "SIM ON ", "SIM  ON", "SIM ON\n", "SIM OFF\r", "SIM STATUS extra", "SIM "}) {
        CHECK(BrainSimulationConsole::handle(line, s, false, out, sizeof(out)));
        CHECK(std::string(out) == "[simulation] unknown_command\n" && !s.enabled());
    }
    CHECK(BrainSimulationConsole::handle("SIM ON", s, true, out, sizeof(out)));
    CHECK(std::string(out) == "[simulation] busy\n" && !s.enabled());
    CHECK(BrainSimulationConsole::handle("SIM ON", s, false, nullptr, sizeof(out)) && !s.enabled());
    std::strcpy(out, "untouched");
    CHECK(BrainSimulationConsole::handle("SIM ON", s, false, out, 0));
    CHECK(std::string(out) == "untouched" && !s.enabled());
    struct { char left = 'L'; char buffer[1] = {'?'}; char right = 'R'; } tiny;
    CHECK(BrainSimulationConsole::handle("SIM ON", s, false, tiny.buffer, sizeof(tiny.buffer)));
    CHECK(s.enabled() && tiny.buffer[0] == 0 && tiny.left == 'L' && tiny.right == 'R');
    CHECK(s.start(request(), 0) == SimulationStart::Accepted);
    CHECK(BrainSimulationConsole::handle("SIM OFF", s, false, out, sizeof(out)));
    CHECK(std::string(out) == "[simulation] busy\n" && s.running());
    CHECK(BrainSimulationConsole::handle("SIM STATUS", s, false, out, sizeof(out)));
    CHECK(std::string(out) == "[simulation] on running=yes pending=0 duration_ms=10 ram_only\n");
    CHECK(s.stop(1));
    const auto before = results(s);
    CHECK(BrainSimulationConsole::handle("SIM OFF", s, false, out, sizeof(out)));
    CHECK(std::string(out) == "[simulation] off\n" && !s.enabled() && results(s) == before);
    CHECK(BrainSimulationConsole::handle("SIM STATUS", s, false, out, sizeof(out)));
    CHECK(std::string(out) == "[simulation] off running=no pending=1 duration_ms=10 ram_only\n");
    CHECK(BrainSimulationConsole::handle("SIM ON", s, false, out, sizeof(out)));
    CHECK(std::string(out) == "[simulation] on\n");
    char small[5]{};
    CHECK(BrainSimulationConsole::handle("SIM STATUS", s, false, small, sizeof(small)));
    CHECK(std::string(small) == "[sim" && results(s) == before);
}

void acknowledged_cross_source_conflict() {
    std::string counterexamples;
    for (const auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch}) {
        BrainSimulation s(pairing(), 1);
        enable(s);
        auto cloud = request(1);
        const auto local = request(1, v4::Source::LocalTouch);
        text(cloud.commandId, local.commandId);
        CHECK(validProductRequest(cloud));
        const auto& first = source == v4::Source::CloudCommand ? cloud : local;
        const auto& second = source == v4::Source::CloudCommand ? local : cloud;
        CHECK(s.start(first, 0) == SimulationStart::Accepted);
        s.poll(1);
        CHECK(s.acknowledge(stored(*s.result(0))));
        // Do not deliver the second request before receipt: that would record
        // a Conflict and conceal forgetting the other source's last identity.
        const auto actual = s.start(second, 3);
        if (actual != SimulationStart::Conflict) {
            counterexamples += std::string(source == v4::Source::CloudCommand ? "cloud->local" : "local->cloud") +
                ": seq=1 completed+stored, other source seq=1 same canonical command_id "
                "expected Conflict, got enum=" + std::to_string(int(actual)) +
                " running=" + std::to_string(s.running()) + "; ";
            continue;
        }
        CHECK(!s.running() && !s.resultCount());
    }
    if (!counterexamples.empty()) throw std::runtime_error(counterexamples);
}
} // namespace

int main(int argc, char** argv) {
    const struct { const char* name; void (*run)(); } groups[] = {
        {"mode", mode}, {"timer", timer}, {"stop", stop}, {"snapshot", snapshot},
        {"deduplication", deduplication}, {"capacity_receipts", capacity_receipts},
        {"rejected_admission", rejected_admission},
        {"rejected_conflict", rejected_conflict},
        {"invalid_decision", invalid_decision},
        {"validation", validation}, {"console", console},
        {"acknowledged_cross_source_conflict", acknowledged_cross_source_conflict}
    };
    if (argc > 2) { std::fprintf(stderr, "Usage: %s [group]\n", argv[0]); return 2; }
    unsigned passed = 0, failed = 0;
    for (const auto& group : groups) {
        if (argc == 2 && std::strcmp(argv[1], group.name)) continue;
        try { group.run(); ++passed; std::printf("PASS %s\n", group.name); }
        catch (const std::exception& error) {
            ++failed;
            std::fprintf(stderr, "FAIL %s: %s\n", group.name, error.what());
        }
    }
    if (!passed && !failed) { std::fprintf(stderr, "Unknown test group\n"); return 2; }
    std::printf("BrainSimulation: %u groups passed, %u failed; pure RAM/codec, no main or transport\n", passed, failed);
    return failed ? 1 : 0;
}
