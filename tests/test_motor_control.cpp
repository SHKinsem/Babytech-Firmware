// Host integration tests for the *real* motion/src/MotorControl.cpp.
//
// The module is compiled as-is; it talks to a fake X42sProtocol (a fake CAN
// driver) whose receive queue, millis() clock, controller state and captured
// TX traffic are controlled by the tests. Nothing here needs hardware or the
// ESP-IDF TWAI driver.
//
// Tiny hand-rolled harness on purpose: no external test framework.

#include "Arduino.h"
#include "MotorControl.h"
#include "fake_x42s.h"
#include "ProtocolGate.h"

#include <cstddef>
#include <cstdio>
#include <set>
#include <string>
#include <utility>

using namespace motion;
using namespace fakecan;

// --- Minimal check harness -------------------------------------------------

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::printf("    FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

// --- Rig + helpers ---------------------------------------------------------

struct Rig {
    MotorControl mc;
    Rig() { fakeReset(); }
};

static std::string status(const Rig& rig, uint8_t id) {
    return rig.mc.statusJson(id).str();
}

static bool activeIs(const Rig& rig, uint8_t id) {
    return status(rig, id).find("\"activeId\":" + std::to_string(id)) !=
           std::string::npos;
}

static MoveRequest moveRequest(uint8_t id, float angleDeg) {
    MoveRequest r;
    r.id = id;
    r.angleDeg = angleDeg;
    r.speedRpm = 30.0f;
    r.accelRpmS = 100.0f;
    r.decelRpmS = 100.0f;
    r.currentMa = 1000;
    return r;
}

// enable(true) + the matching F3 0x02 ack, so the node is confirmed enabled.
static void enableAndConfirm(Rig& rig, uint8_t id, uint32_t atMs) {
    setMillis(atMs);
    rig.mc.enable(id, true);
    injectRx(makeAck(id, kFrameEnable, 0x02));
    setMillis(atMs + 20);
    rig.mc.poll();
}

// Fresh, approximately stationary feedback, then queue a relative move.
static Result startMove(Rig& rig, uint8_t id, float angleDeg, uint32_t atMs) {
    setMillis(atMs);
    injectRx(makePosition(id, 0));
    injectRx(makeVelocity(id, 0));
    rig.mc.poll();
    setMillis(atMs);
    return rig.mc.move(moveRequest(id, angleDeg));
}

// --- Tests -----------------------------------------------------------------

static void test_begin_sends_no_motion() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);

    // begin() only brings the controller up: no enable/move/stop on the wire.
    CHECK(capturedTX.empty());
    CHECK(countTx(TxKind::Enable) == 0);
    CHECK(countTx(TxKind::Move) == 0);
    CHECK(countTx(TxKind::Stop) == 0);

    const std::string j = status(rig, 1);
    CHECK(j.find("\"canReady\":true") != std::string::npos);
    CHECK(j.find("\"busState\":\"running\"") != std::string::npos);
    CHECK(j.find("\"activeId\":0") != std::string::npos);
    CHECK(j.find("\"state\":\"disabled\"") != std::string::npos);
}

static void test_watch_select_1_to_255_not_exhausted() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);

    uint32_t t = 0;
    for (int id = 1; id <= 255; ++id) {
        t += 40;
        setMillis(t);
        rig.mc.watch(static_cast<uint8_t>(id));
        rig.mc.poll();
        const TxRecord last = lastTx();
        CHECK(last.kind == TxKind::ReadSysParam);
        CHECK(last.addr == static_cast<uint8_t>(id));
    }

    // Cycling back round must keep working: selecting never exhausts a slot.
    t += 40;
    setMillis(t);
    rig.mc.watch(1);
    rig.mc.poll();
    CHECK(lastTx().kind == TxKind::ReadSysParam);
    CHECK(lastTx().addr == 1);
}

static void test_query_first_after_30ms_and_repeats_past_600ms() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    rig.mc.watch(1);

    setMillis(0);
    rig.mc.poll();
    CHECK(countTx(TxKind::ReadSysParam) == 0);  // first query needs >= 30 ms

    setMillis(29);
    rig.mc.poll();
    CHECK(countTx(TxKind::ReadSysParam) == 0);

    setMillis(40);
    rig.mc.poll();
    CHECK(countTx(TxKind::ReadSysParam) == 1);

    const uint32_t afterFirst = countTx(TxKind::ReadSysParam);
    setMillis(640);  // > 600 ms after the first query
    rig.mc.poll();
    CHECK(countTx(TxKind::ReadSysParam) > afterFirst);

    setMillis(1240);
    rig.mc.poll();
    CHECK(countTx(TxKind::ReadSysParam) > afterFirst + 1);
}

static void test_query_rotation_exactly_four_fields_selected_and_active() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);

    enableAndConfirm(rig, 2, 0);
    const Result m = startMove(rig, 2, 10.0f, 40);
    CHECK(m.code == kCodeQueued);

    // Acknowledge the move, then watch a different id: the active job stays 2.
    injectRx(makeAck(2, kFrameMove, 0x02));
    setMillis(60);
    rig.mc.poll();
    rig.mc.watch(1);

    txLog.clear();
    capturedTX.clear();

    std::set<std::pair<int, int> > seen;
    uint32_t t = 80;
    for (int i = 0; i < 8; ++i) {
        t += 40;
        setMillis(t);
        // Keep the active job's feedback fresh but moving, so it never completes.
        injectRx(makePosition(2, 50));
        injectRx(makeVelocity(2, 100));
        rig.mc.poll();

        const TxRecord last = lastTx();
        CHECK(last.kind == TxKind::ReadSysParam);
        seen.insert(std::make_pair(static_cast<int>(last.addr),
                                   static_cast<int>(last.param)));
    }

    // Exactly the three feedback fields (Cpos, Vel, Cpha) for both the selected
    // node and the active job, no more and no fewer combinations.
    CHECK(seen.size() == 8);
    for (int addr = 1; addr <= 2; ++addr) {
        int fields = 0;
        if (seen.count(std::make_pair(addr, static_cast<int>(X42sSysParam::Cpos)))) ++fields;
        if (seen.count(std::make_pair(addr, static_cast<int>(X42sSysParam::Vel)))) ++fields;
        if (seen.count(std::make_pair(addr, static_cast<int>(X42sSysParam::Cpha)))) ++fields;
        if (seen.count(std::make_pair(addr, static_cast<int>(X42sSysParam::Flag)))) ++fields;
        CHECK(fields == 4);
    }
}

static void test_enable_confirmed_only_after_f3_02() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);

    setMillis(0);
    const Result r = rig.mc.enable(1, true);
    CHECK(r.code == kCodeQueued);
    CHECK(countTxTo(1, TxKind::Enable) == 1);
    CHECK(status(rig, 1).find("\"enabled\":false") != std::string::npos);

    setMillis(40);
    rig.mc.poll();  // no ack yet
    CHECK(status(rig, 1).find("\"enabled\":false") != std::string::npos);

    injectRx(makeAck(1, kFrameEnable, 0x02));
    setMillis(80);
    rig.mc.poll();

    const std::string j = status(rig, 1);
    CHECK(j.find("\"enabled\":true") != std::string::npos);
    CHECK(j.find("\"state\":\"idle\"") != std::string::npos);
}

static void test_late_f3_ack_after_stop_does_not_enable() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);

    setMillis(0);
    rig.mc.enable(1, true);
    setMillis(10);
    rig.mc.stop(1);  // cancel the in-flight enable

    injectRx(makeAck(1, kFrameEnable, 0x02));  // late ack must be ignored
    setMillis(50);
    rig.mc.poll();

    const std::string j = status(rig, 1);
    CHECK(j.find("\"enabled\":false") != std::string::npos);
    CHECK(j.find("\"enabled\":true") == std::string::npos);
}

static void test_move_requires_fresh_feedback_and_enable() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);

    setMillis(0);
    const Result notEnabled = rig.mc.move(moveRequest(1, 90.0f));
    CHECK(notEnabled.code == kCodeBusy);
    CHECK(std::string(notEnabled.message) == "not_enabled");

    enableAndConfirm(rig, 1, 0);

    setMillis(60);
    const Result noFeedback = rig.mc.move(moveRequest(1, 90.0f));
    CHECK(noFeedback.code == kCodeUnavailable);
    CHECK(std::string(noFeedback.message) == "feedback_unavailable");

    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    setMillis(80);
    rig.mc.poll();
    setMillis(80);
    const Result ok = rig.mc.move(moveRequest(1, 90.0f));
    CHECK(ok.code == kCodeQueued);
}

static void test_move_completion_needs_cd_ack_and_two_distinct_pairs() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    enableAndConfirm(rig, 1, 0);

    const Result m = startMove(rig, 1, 10.0f, 40);  // target = 100 tenths
    CHECK(m.code == kCodeQueued);
    CHECK(activeIs(rig, 1));

    injectRx(makeAck(1, kFrameMove, 0x02));  // CD ack
    setMillis(60);
    rig.mc.poll();
    CHECK(activeIs(rig, 1));  // ack alone is not enough

    injectRx(makePosition(1, 96));  // first post-command pair
    injectRx(makeVelocity(1, 0));
    setMillis(80);
    rig.mc.poll();
    CHECK(activeIs(rig, 1));

    injectRx(makePosition(1, 100));  // second, distinct pair completes it
    injectRx(makeVelocity(1, 0));
    setMillis(100);
    rig.mc.poll();
    CHECK(!activeIs(rig, 1));
    CHECK(status(rig, 1).find("\"state\":\"idle\"") != std::string::npos);
}

static void test_repeated_poll_does_not_complete_move() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    enableAndConfirm(rig, 1, 0);

    startMove(rig, 1, 10.0f, 40);
    injectRx(makeAck(1, kFrameMove, 0x02));
    setMillis(60);
    rig.mc.poll();

    injectRx(makePosition(1, 96));
    injectRx(makeVelocity(1, 0));
    setMillis(80);
    rig.mc.poll();
    CHECK(activeIs(rig, 1));

    // Re-polling the same sample must never count it twice.
    for (uint32_t t = 81; t <= 90; ++t) {
        setMillis(t);
        rig.mc.poll();
    }
    CHECK(activeIs(rig, 1));

    // Only a genuinely newer pair finishes the move.
    injectRx(makePosition(1, 100));
    injectRx(makeVelocity(1, 0));
    setMillis(100);
    rig.mc.poll();
    CHECK(!activeIs(rig, 1));
}

static void test_loss_of_velocity_faults_and_stops() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    enableAndConfirm(rig, 1, 0);

    startMove(rig, 1, 10.0f, 40);
    injectRx(makeAck(1, kFrameMove, 0x02));
    setMillis(60);
    rig.mc.poll();

    // Feedback goes silent: past the freshness grace the job must fault and a
    // best-effort stop must be sent, never a silent "keep going".
    setMillis(700);
    rig.mc.poll();

    CHECK(!activeIs(rig, 1));
    CHECK(sawStopFor(1));
    const std::string j = status(rig, 1);
    CHECK(j.find("\"state\":\"fault\"") != std::string::npos);
    CHECK(j.find("\"enabled\":false") != std::string::npos);
}

static void test_bus_off_invalidates_enable() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    enableAndConfirm(rig, 1, 0);
    CHECK(status(rig, 1).find("\"enabled\":true") != std::string::npos);

    busState = CanControllerState::BusOff;
    setMillis(100);
    rig.mc.poll();

    const std::string j = status(rig, 1);
    CHECK(j.find("\"enabled\":false") != std::string::npos);
    CHECK(j.find("\"state\":\"fault\"") != std::string::npos);
    CHECK(j.find("\"busState\":\"bus_off\"") != std::string::npos);
    CHECK(j.find("\"canReady\":false") != std::string::npos);

    setMillis(120);
    const Result r = rig.mc.enable(1, true);
    CHECK(r.code == kCodeUnavailable);
    CHECK(std::string(r.message) == "bus_off");
}

static void test_partial_move_tx_failure_sends_stop() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    enableAndConfirm(rig, 1, 0);

    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    setMillis(40);
    rig.mc.poll();

    failNextMoveTx = true;
    setMillis(40);
    const Result r = rig.mc.move(moveRequest(1, 10.0f));
    CHECK(r.code == kCodeUnavailable);
    CHECK(std::string(r.message) == "can_tx_failed");

    // The attempted move must be followed by a best-effort stop for the same id.
    bool moveThenStop = false;
    for (size_t i = 1; i < txLog.size(); ++i) {
        if (txLog[i - 1].kind == TxKind::Move && txLog[i].kind == TxKind::Stop &&
            txLog[i].addr == 1) {
            moveThenStop = true;
        }
    }
    CHECK(moveThenStop);
    CHECK(status(rig, 1).find("\"state\":\"fault\"") != std::string::npos);
}

static void test_stop_all_works_with_active_job() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    enableAndConfirm(rig, 1, 0);
    startMove(rig, 1, 10.0f, 40);
    CHECK(activeIs(rig, 1));

    setMillis(60);
    const Result r = rig.mc.stopAll();
    CHECK(r.code == kCodeQueued);
    CHECK(!activeIs(rig, 1));
    CHECK(countTxTo(0, TxKind::Stop) == 1);  // id 0 broadcast stop
}

// --- Runner ----------------------------------------------------------------

static void test_diagnostics_capture_unrecognized_reply_and_flags() {
    Rig rig; rig.mc.begin(4, 5, 500000); rig.mc.watch(1);
    const uint8_t flagData[] = {0x3A, 0x03, 0x6B};
    injectRx(makeFrame(1, flagData, 3));
    injectRx(makeAck(1, 0x00, 0xEE));
    setMillis(80); rig.mc.poll();
    CHECK(status(rig, 1).find("\"driverFlags\":3") != std::string::npos);
    CHECK(status(rig, 1).find("\"driverEnabled\":true") != std::string::npos);
    CHECK(status(rig, 1).find("\"enabled\":false") != std::string::npos);
    CHECK(rig.mc.canDebugJson().str().find("[0,238,107]") != std::string::npos);
    setMillis(800); rig.mc.poll();
    CHECK(status(rig, 1).find("\"driverFlags\":null") != std::string::npos);
}

static void test_protocol_gateway() {
    Rig rig; rig.mc.begin(4, 5, 500000);
    uint8_t unknown[] = {1, 0xFF, 0x66, 0x6B};
    CHECK(rig.mc.command(unknown, sizeof(unknown)).code == 400);
    uint8_t read[] = {1, 0x42, 0x6C, 0x6B};
    CHECK(rig.mc.command(read, sizeof(read)).code == 202);
    CHECK(rig.mc.traceJson().str().find("[66,108,107]") != std::string::npos);
    uint8_t velocity[] = {1, 0xC6, 0, 0, 60, 0, 100, 0, 3, 32, 0x6B};
    CHECK(rig.mc.command(velocity, sizeof(velocity)).code == 409);
    enableAndConfirm(rig, 1, 0);
    CHECK(rig.mc.hasActiveMotion()); // Wi-Fi must not scan while enabled.
    setMillis(40);
    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    rig.mc.poll();
    CHECK(rig.mc.command(velocity, sizeof(velocity)).code == 202);
    CHECK(status(rig, 1).find("experiment_running") != std::string::npos);
    setMillis(80); rig.mc.watch(2); rig.mc.poll();
    CHECK(status(rig, 1).find("experiment_running") != std::string::npos);
    setMillis(700); rig.mc.poll(); // stale telemetry aborts without the browser
    CHECK(sawStopFor(1));
    CHECK(status(rig, 1).find("feedback_stale") != std::string::npos);
    velocity[7] = 1;
    CHECK(rig.mc.command(velocity, sizeof(velocity)).code == 400);
    uint8_t config[] = {1, 0x0A, 0x6D, 0x6B};
    CHECK(rig.mc.command(config, sizeof(config)).code == 409);
}

static void test_experiment_deadline_and_partial_failure() {
    Rig rig; rig.mc.begin(4, 5, 500000); enableAndConfirm(rig, 1, 0);
    uint8_t velocity[] = {1, 0xC6, 0, 0, 60, 0, 100, 0, 3, 32, 0x6B};
    setMillis(40); injectRx(makePosition(1,0)); injectRx(makeVelocity(1,0)); rig.mc.poll();
    CHECK(rig.mc.command(velocity, sizeof(velocity)).code == 202);
    for (uint32_t t = 140; t < 5040; t += 100) {
        setMillis(t); injectRx(makePosition(1,100)); injectRx(makeVelocity(1,100)); rig.mc.poll();
    }
    CHECK(!sawStopFor(1));
    setMillis(5040); injectRx(makePosition(1,100)); injectRx(makeVelocity(1,100)); rig.mc.poll();
    CHECK(sawStopFor(1));
    CHECK(status(rig,1).find("stop_requested") != std::string::npos);
    CHECK(status(rig,1).find("\"enabled\":false") != std::string::npos);
    // A new independent controller exercises failed multi-frame command TX.
    Rig failed; failed.mc.begin(4,5,500000); enableAndConfirm(failed,1,0);
    setMillis(40); injectRx(makePosition(1,0)); injectRx(makeVelocity(1,0)); failed.mc.poll();
    failNextMoveTx = true;
    CHECK(failed.mc.command(velocity,sizeof(velocity)).code == 503);
    CHECK(sawStopFor(1));
    CHECK(status(failed,1).find("command_tx_failed") != std::string::npos);
}

static void test_protocol_validation_bounds() {
    const uint8_t valid[][20] = {
        {1,0x1F,0x6B}, {1,0x42,0x6C,0x6B}, {1,0x43,0x7A,0x6B},
        {1,0xF3,0xAB,1,0,0x6B}, {1,0xFE,0x98,0,0x6B},
        {1,0x11,0x18,0x36,0,30,0x6B}, {1,0x46,0x69,0,0,0x6B},
        {1,0xC6,0,0,60,0,100,0,3,32,0x6B},
        {1,0xCD,0,0,60,0,60,0,100,0,0,0,100,2,0,3,32,0x6B}
    };
    const uint8_t lengths[] = {3,4,4,6,5,7,6,11,18};
    for (unsigned i=0; i<sizeof(lengths); ++i) {
        CHECK(validateCommand(valid[i], lengths[i]) != CommandKind::Invalid);
        for (uint8_t n=0; n<lengths[i]; ++n) CHECK(validateCommand(valid[i], n) == CommandKind::Invalid);
    }
    uint8_t highCurrent[] = {1,0xC6,0,0,60,0,100,0,0xFF,0xFF,0x6B};
    CHECK(validateCommand(highCurrent,sizeof(highCurrent)) == CommandKind::Invalid);
    highCurrent[8]=3; highCurrent[9]=32; highCurrent[7]=1;
    CHECK(validateCommand(highCurrent,sizeof(highCurrent)) == CommandKind::Invalid);
    highCurrent[7]=0; highCurrent[0]=0;
    CHECK(validateCommand(highCurrent,sizeof(highCurrent)) == CommandKind::Invalid);
}

struct TestCase {
    const char* name;
    void (*fn)();
};

int main() {
    const TestCase tests[] = {
        {"protocol truncation, limits and sync rejection", test_protocol_validation_bounds},
        {"experiment 5s deadline and failed TX stop", test_experiment_deadline_and_partial_failure},
        {"protocol whitelist, trace, guarded experiment and WiFi gate", test_protocol_gateway},
        {"raw diagnostics preserve unexpected replies and actual flags", test_diagnostics_capture_unrecognized_reply_and_flags},
        {"begin sends no motion", test_begin_sends_no_motion},
        {"watch/select 1..255 not exhausted", test_watch_select_1_to_255_not_exhausted},
        {"query first >=30ms and repeats past 600ms", test_query_first_after_30ms_and_repeats_past_600ms},
        {"query rotation: 4 fields, selected + active", test_query_rotation_exactly_four_fields_selected_and_active},
        {"enable confirmed only after F3 02", test_enable_confirmed_only_after_f3_02},
        {"late F3 ack after stop does not enable", test_late_f3_ack_after_stop_does_not_enable},
        {"move requires fresh feedback + enable", test_move_requires_fresh_feedback_and_enable},
        {"move completion: CD ack + two distinct pairs", test_move_completion_needs_cd_ack_and_two_distinct_pairs},
        {"repeated poll does not complete move", test_repeated_poll_does_not_complete_move},
        {"loss of velocity faults and stops", test_loss_of_velocity_faults_and_stops},
        {"bus off invalidates enable", test_bus_off_invalidates_enable},
        {"partial move TX failure sends stop", test_partial_move_tx_failure_sends_stop},
        {"stopAll works with active job", test_stop_all_works_with_active_job},
    };

    const int count = static_cast<int>(sizeof(tests) / sizeof(tests[0]));
    for (int i = 0; i < count; ++i) {
        const int before = g_failures;
        tests[i].fn();
        std::printf("%s  %s\n", g_failures == before ? "PASS" : "FAIL",
                    tests[i].name);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
