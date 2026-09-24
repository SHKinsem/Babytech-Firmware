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

// Independent wire opcode; controller keeps its own private constant.
constexpr uint8_t kFrameHome = 0x9A;
static CanRawFrame makeFlags(uint8_t addr, uint8_t flags);

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
    injectRx(makeFlags(id, 1));
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

// --- Homing helpers --------------------------------------------------------

// One 0x3B homing status reply (manual V1.0.5 p62-63).
static CanRawFrame makeHomeStatus(uint8_t addr, uint8_t flags) {
    const uint8_t data[3] = {0x3B, flags, 0x6B};
    return makeFrame(addr, data, sizeof(data));
}

// Counts transmitted frames by function code, optionally for one address.
static uint32_t countTxOpcode(uint8_t opcode, uint8_t addr) {
    uint32_t n = 0;
    for (size_t i = 0; i < capturedTX.size(); ++i) {
        const CanRawFrame& f = capturedTX[i];
        if (f.length == 0 || f.data[0] != opcode) continue;
        if (addr != 0 && x42sCanAddress(f.identifier) != addr) continue;
        ++n;
    }
    return n;
}

// Confirmed enable plus one fresh stationary position/velocity pair.
static void enableAndFeedStationary(Rig& rig, uint8_t id, uint32_t atMs, int32_t tenths = 0) {
    enableAndConfirm(rig, id, atMs);
    setMillis(atMs + 40);
    injectRx(makePosition(id, tenths));
    injectRx(makeVelocity(id, 0));
    rig.mc.poll();
}

static bool outcomeIs(const Rig& rig, uint8_t id, const char* outcome) {
    return status(rig, id).find(std::string("\"homeOutcome\":\"") + outcome + "\"") !=
           std::string::npos;
}

// --- Direct (FB/CB) position helpers ---------------------------------------

// [addr][FB][dir][speed u16][angle u32][mode][sync][6B] (manual V1.0.5 p54).
static void buildDirect(
    uint8_t* frame, uint8_t addr, uint8_t dir, uint16_t speedTenths,
    uint32_t angleTenths, uint8_t mode, uint8_t sync) {
    frame[0] = addr;
    frame[1] = 0xFB;
    frame[2] = dir;
    frame[3] = static_cast<uint8_t>(speedTenths >> 8);
    frame[4] = static_cast<uint8_t>(speedTenths & 0xFF);
    frame[5] = static_cast<uint8_t>((angleTenths >> 24) & 0xFF);
    frame[6] = static_cast<uint8_t>((angleTenths >> 16) & 0xFF);
    frame[7] = static_cast<uint8_t>((angleTenths >> 8) & 0xFF);
    frame[8] = static_cast<uint8_t>(angleTenths & 0xFF);
    frame[9] = mode;
    frame[10] = sync;
    frame[11] = 0x6B;
}

// The CB form adds the max-current field after sync (manual V1.0.5 p54-55).
static void buildDirectLimit(
    uint8_t* frame, uint8_t addr, uint8_t dir, uint16_t speedTenths,
    uint32_t angleTenths, uint8_t mode, uint8_t sync, uint16_t currentMa) {
    buildDirect(frame, addr, dir, speedTenths, angleTenths, mode, sync);
    frame[1] = 0xCB;
    frame[11] = static_cast<uint8_t>(currentMa >> 8);
    frame[12] = static_cast<uint8_t>(currentMa & 0xFF);
    frame[13] = 0x6B;
}

// The manual's own worked example: motor 2, FB, CW, 300 (0.1 RPM), 900 (0.1
// degree), mode 2, immediate. CB adds 800 mA before the checksum.
static const uint8_t kDirectExample[12] =
    {2, 0xFB, 0x00, 0x01, 0x2C, 0x00, 0x00, 0x03, 0x84, 0x02, 0x00, 0x6B};
static const uint8_t kDirectLimitExample[14] =
    {2, 0xCB, 0x00, 0x01, 0x2C, 0x00, 0x00, 0x03, 0x84, 0x02, 0x00, 0x03, 0x20, 0x6B};

// Last transmitted direct (FB/CB) command, or null when there is none.
static const TxRecord* lastDirect() {
    for (size_t i = txLog.size(); i > 0; --i) {
        if (txLog[i - 1].kind == TxKind::Direct) return &txLog[i - 1];
    }
    return nullptr;
}

// Confirmed enable, fresh stationary feedback and one fresh 0x33 sample of the
// driver's own target position (manual V1.0.5 p70).
static void enableWithTarget(
    Rig& rig, uint8_t id, uint32_t atMs, int32_t positionTenths,
    int32_t targetTenths) {
    enableAndFeedStationary(rig, id, atMs, positionTenths);
    setMillis(atMs + 60);
    injectRx(makeTarget(id, targetTenths));
    rig.mc.poll();
}

// The seven 4C homing-parameter bytes are positional, so the frame is built
// here instead of being repeated in every test.
static void buildHomeParams(uint8_t* frame, uint8_t mode, uint16_t velocityRpm,
                            uint32_t timeoutMs, uint16_t stallMa, uint8_t powerOn) {
    frame[0] = 1; frame[1] = 0x4C; frame[2] = 0xAE; frame[3] = 1;
    frame[4] = mode; frame[5] = 0;
    frame[6] = static_cast<uint8_t>(velocityRpm >> 8);
    frame[7] = static_cast<uint8_t>(velocityRpm & 0xFF);
    frame[8] = static_cast<uint8_t>((timeoutMs >> 24) & 0xFF);
    frame[9] = static_cast<uint8_t>((timeoutMs >> 16) & 0xFF);
    frame[10] = static_cast<uint8_t>((timeoutMs >> 8) & 0xFF);
    frame[11] = static_cast<uint8_t>(timeoutMs & 0xFF);
    frame[12] = 0; frame[13] = 100;
    frame[14] = static_cast<uint8_t>(stallMa >> 8);
    frame[15] = static_cast<uint8_t>(stallMa & 0xFF);
    frame[16] = 0; frame[17] = 60;
    frame[18] = powerOn; frame[19] = 0x6B;
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
        t += 3000; // previous page lease and in-flight timeout have expired
        setMillis(t);
        rig.mc.watch(static_cast<uint8_t>(id));
        rig.mc.poll();
        const TxRecord last = lastTx();
        CHECK(last.kind == TxKind::ReadSysParam);
        CHECK(last.addr == static_cast<uint8_t>(id));
    }

    // Cycling back round must keep working: selecting never exhausts a slot.
    t += 3000;
    setMillis(t);
    rig.mc.watch(1);
    rig.mc.poll();
    CHECK(lastTx().kind == TxKind::ReadSysParam);
    CHECK(lastTx().addr == 1);
}

static void test_query_global_budget_and_repeats_past_600ms() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    rig.mc.watch(1);

    setMillis(0);
    rig.mc.poll();
    CHECK(countTx(TxKind::ReadSysParam) == 1);  // first demand has no old traffic

    setMillis(29);
    rig.mc.poll();
    CHECK(countTx(TxKind::ReadSysParam) == 1); // budget does not refill per poll

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

static void test_query_budget_selected_and_active_without_current() {
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
    for (int i = 0; i < 20; ++i) {
        t += 100;
        setMillis(t);
        // Keep the active job's feedback fresh but moving, so it never completes.
        injectRx(makePosition(2, 50));
        injectRx(makeVelocity(2, 100));
        injectRx(makeFlags(2, 1));
        injectRx(makePosition(1, 0));
        injectRx(makeVelocity(1, 0));
        injectRx(makeFlags(1, 1));
        rig.mc.watch(1);
        rig.mc.poll();

        const TxRecord last = lastTx();
        CHECK(last.kind == TxKind::ReadSysParam);
        seen.insert(std::make_pair(static_cast<int>(last.addr),
                                   static_cast<int>(last.param)));
    }

    // Position, velocity and flags share a budget. Current is opt-in, not a
    // periodic tax on every motor. Aging prevents low-priority starvation.
    CHECK(seen.size() == 6);
    for (int addr = 1; addr <= 2; ++addr) {
        int fields = 0;
        if (seen.count(std::make_pair(addr, static_cast<int>(X42sSysParam::Cpos)))) ++fields;
        if (seen.count(std::make_pair(addr, static_cast<int>(X42sSysParam::Vel)))) ++fields;
        if (seen.count(std::make_pair(addr, static_cast<int>(X42sSysParam::Cpha)))) ++fields;
        if (seen.count(std::make_pair(addr, static_cast<int>(X42sSysParam::Flag)))) ++fields;
        CHECK(fields == 3);
    }
    CHECK(countTx(TxKind::ReadSysParam) <= 20);
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

    CHECK(status(rig, 1).find("\"enabled\":false") != std::string::npos);
    injectRx(makeFlags(1, 1));
    setMillis(90);
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
    // The trial timer ends the trial with FE; 0xFE halts the motor and does not
    // disable the driver, so the already-confirmed enable survives it.
    CHECK(status(rig,1).find("\"enabled\":true") != std::string::npos);
    // A new independent controller exercises failed multi-frame command TX.
    Rig failed; failed.mc.begin(4,5,500000); enableAndConfirm(failed,1,0);
    setMillis(40); injectRx(makePosition(1,0)); injectRx(makeVelocity(1,0)); failed.mc.poll();
    failNextMoveTx = true;
    CHECK(failed.mc.command(velocity,sizeof(velocity)).code == 503);
    CHECK(sawStopFor(1));
    CHECK(status(failed,1).find("command_tx_failed") != std::string::npos);
}

static void test_configurable_debug_limits() {
    const uint8_t screenshot[] = {1,0xF6,0,3,0xE8,3,0xE8,0,0x6B};
    DebugLimits limits;
    CHECK(validateCommand(screenshot,sizeof(screenshot),limits) == CommandKind::Invalid);
    limits.maxAccelRpmS=2000; limits.experimentDurationMs=10000;
    CHECK(validateCommand(screenshot,sizeof(screenshot),limits) == CommandKind::Experiment);
    MovePlan plan; const char* error=nullptr;
    MoveRequest move{1,90,100,1000,1000,800};
    CHECK(!buildMovePlan(move,plan,&error));
    CHECK(buildMovePlan(move,plan,&error,limits));
    CHECK(plan.accelWire==1000 && plan.speedTenths==1000);
    for (uint32_t timeout : {10000u,0u}) {
        Rig rig; rig.mc.begin(4,5,500000);
        limits.experimentDurationMs=timeout;
        CHECK(rig.mc.setDebugLimits(limits));
        CHECK(!sawStopFor(1)); // setting limits emits no motor operation
        enableAndConfirm(rig,1,0);
        setMillis(40); injectRx(makePosition(1,0)); injectRx(makeVelocity(1,0)); rig.mc.poll();
        CHECK(rig.mc.command(screenshot,sizeof(screenshot)).code==202);
        CHECK(!rig.mc.setDebugLimits(DebugLimits{})); // no timer change mid-flight
        for(uint32_t t=140;t<10040;t+=100) {
            setMillis(t); injectRx(makePosition(1,100)); injectRx(makeVelocity(1,1000)); rig.mc.poll();
        }
        CHECK(!sawStopFor(1)); // specifically survives old 5s cutoff
        setMillis(10040); injectRx(makePosition(1,100)); injectRx(makeVelocity(1,1000)); rig.mc.poll();
        CHECK(sawStopFor(1)==(timeout!=0));
        if(timeout==0) {
            setMillis(11000); rig.mc.poll();
            CHECK(sawStopFor(1)); // continuous still stops on stale feedback
        }
    }
    DebugLimits bad; bad.maxAccelRpmS=65536;
    CHECK(!validDebugLimits(bad));
    bad=DebugLimits{}; bad.maxSpeedTenths=30001; CHECK(!validDebugLimits(bad));
    bad=DebugLimits{}; bad.maxCurrentMa=5001; CHECK(!validDebugLimits(bad));
    bad=DebugLimits{}; bad.experimentDurationMs=1; CHECK(!validDebugLimits(bad));
}

static void test_protocol_validation_bounds() {
    // Manual V1.0.5 read-only extensions must not accept a payload or broadcast.
    for (uint8_t op : {0x1A, 0x26, 0x32, 0x34, 0x39, 0x3C, 0x3D}) {
        uint8_t query[] = {1, op, 0x6B, 0x6B};
        CHECK(validateCommand(query, 3) == CommandKind::Read);
        CHECK(validateCommand(query, 4) == CommandKind::Invalid);
        query[0] = 0;
        CHECK(validateCommand(query, 3) == CommandKind::Invalid);
    }
    uint8_t controlMode[] = {1, 0x46, 0x69, 0, 2, 0x6B};
    CHECK(validateCommand(controlMode, sizeof(controlMode)) == CommandKind::Invalid);
    controlMode[4] = 1;
    CHECK(validateCommand(controlMode, sizeof(controlMode)) == CommandKind::Configure);
    uint8_t home[] = {1,0x4C,0xAE,0,5,0,0,30,0,0,0x27,0x10,0,10,3,0xE8,0,100,0,0x6B};
    CHECK(validateCommand(home, sizeof(home)) == CommandKind::Configure);
    home[4] = 6;
    CHECK(validateCommand(home, sizeof(home)) == CommandKind::Invalid);
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

// --- Stop semantics: a stop halts motion, it does not disable the driver ----

static void test_stop_keeps_confirmed_enable() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    enableAndConfirm(rig, 1, 0);
    CHECK(status(rig, 1).find("\"enabled\":true") != std::string::npos);

    setMillis(40);
    CHECK(rig.mc.stop(1).code == kCodeQueued);
    CHECK(sawStopFor(1));
    const std::string stopped = status(rig, 1);
    CHECK(stopped.find("\"state\":\"stop_requested\"") != std::string::npos);
    // 0xFE halts the motor; it does not disable the driver, so the operator does
    // not have to re-enable after every stop.
    CHECK(stopped.find("\"enabled\":true") != std::string::npos);

    // The stop cancelled any enable still in flight: a late F3 ack changes
    // nothing (the enable was already confirmed before the stop).
    injectRx(makeAck(1, kFrameEnable, 0x02));
    setMillis(60);
    rig.mc.poll();
    CHECK(status(rig, 1).find("\"enabled\":true") != std::string::npos);

    // Fresh stationary feedback confirms the stop while staying enabled.
    setMillis(80);
    injectRx(makePosition(1, 100));
    injectRx(makeVelocity(1, 0));
    rig.mc.poll();
    const std::string settled = status(rig, 1);
    CHECK(settled.find("\"state\":\"idle\"") != std::string::npos);
    CHECK(settled.find("\"enabled\":true") != std::string::npos);

    // An explicit disable is still what drops the enable.
    setMillis(100);
    CHECK(rig.mc.enable(1, false).code == kCodeQueued);
    injectRx(makeAck(1, kFrameEnable, 0x02));
    setMillis(120);
    rig.mc.poll();
    CHECK(status(rig, 1).find("\"enabled\":false") != std::string::npos);
}

static void test_stop_all_keeps_confirmed_enable() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    enableAndConfirm(rig, 1, 0);
    setMillis(40);

    CHECK(rig.mc.stopAll().code == kCodeQueued);
    CHECK(countTxTo(0, TxKind::Stop) == 1);  // id 0 broadcast

    const std::string j = status(rig, 1);
    CHECK(j.find("\"state\":\"stop_requested\"") != std::string::npos);
    CHECK(j.find("\"enabled\":true") != std::string::npos);

    // The broadcast cancelled an in-flight enable: a late ack cannot re-enable.
    setMillis(60);
    rig.mc.enable(1, false);  // explicit disable is what drops the enable
    injectRx(makeAck(1, kFrameEnable, 0x02));
    setMillis(80);
    rig.mc.poll();
    CHECK(status(rig, 1).find("\"enabled\":false") != std::string::npos);
}

static void test_trial_timeout_stop_keeps_confirmed_enable() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    DebugLimits limits;
    limits.experimentDurationMs = 1000;  // a short trial window for the test
    CHECK(rig.mc.setDebugLimits(limits));
    enableAndConfirm(rig, 1, 0);

    uint8_t velocity[] = {1, 0xC6, 0, 0, 60, 0, 100, 0, 3, 32, 0x6B};
    setMillis(40);
    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    rig.mc.poll();
    CHECK(rig.mc.command(velocity, sizeof(velocity)).code == 202);

    // Fresh, moving feedback until the configured trial deadline expires.
    for (uint32_t t = 140; t <= 1040; t += 100) {
        setMillis(t);
        injectRx(makePosition(1, 100));
        injectRx(makeVelocity(1, 100));
        rig.mc.poll();
    }

    CHECK(sawStopFor(1));
    const std::string j = status(rig, 1);
    CHECK(j.find("\"state\":\"stop_requested\"") != std::string::npos);
    // The trial timer stops the trial with FE; the driver stays enabled and no
    // fault is latched (a stale-feedback abort is the case that faults).
    CHECK(j.find("\"enabled\":true") != std::string::npos);
    CHECK(j.find("\"fault\":\"none\"") != std::string::npos);
}

// --- Homing ----------------------------------------------------------------

static void test_home_trigger_gates_and_wire_frame() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);

    // Not enabled yet: a homing trigger is gated exactly like a move.
    setMillis(0);
    const Result notEnabled = rig.mc.home(1, 0);
    CHECK(notEnabled.code == kCodeBusy);
    CHECK(std::string(notEnabled.message) == "not_enabled");

    // An unusable mode is rejected by the controller and by the protocol gate.
    enableAndConfirm(rig, 1, 0);
    setMillis(40);
    CHECK(rig.mc.home(1, 6).code == kCodeInvalid);
    const uint8_t badMode[] = {1, 0x9A, 6, 0, 0x6B};
    CHECK(validateCommand(badMode, sizeof(badMode)) == CommandKind::Invalid);
    const uint8_t cachedHome[] = {1, 0x9A, 0, 1, 0x6B};
    CHECK(validateCommand(cachedHome, sizeof(cachedHome)) == CommandKind::Invalid);
    CHECK(validateCommand(badMode, 4) == CommandKind::Invalid);

    // Confirmed enable but no fresh feedback yet.
    setMillis(60);
    const Result noFeedback = rig.mc.home(1, 0);
    CHECK(noFeedback.code == kCodeUnavailable);
    CHECK(std::string(noFeedback.message) == "feedback_unavailable");

    // Fresh stationary feedback: the trigger goes out as [1][0x9A][2][0][0x6B].
    setMillis(80);
    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    rig.mc.poll();
    setMillis(80);
    capturedTX.clear();
    const Result queued = rig.mc.home(1, 2);
    CHECK(queued.code == kCodeQueued);
    CHECK(std::string(queued.message) == "queued_home");
    CHECK(activeIs(rig, 1));
    CHECK(outcomeIs(rig, 1, "running"));
    CHECK(status(rig, 1).find("\"state\":\"homing\"") != std::string::npos);

    bool triggerSeen = false;
    for (size_t i = 0; i < capturedTX.size(); ++i) {
        const CanRawFrame& f = capturedTX[i];
        if (f.length == 4 && f.data[0] == 0x9A && f.data[1] == 2 && f.data[2] == 0 &&
            f.data[3] == 0x6B && x42sCanAddress(f.identifier) == 1) {
            triggerSeen = true;
        }
    }
    CHECK(triggerSeen);

    // One supervised action at a time: move, a second home and an experiment are
    // all rejected while the homing run is live.
    CHECK(rig.mc.home(1, 0).code == kCodeBusy);
    CHECK(rig.mc.home(2, 0).code == kCodeBusy);
    CHECK(rig.mc.move(moveRequest(1, 10.0f)).code == kCodeBusy);
    CHECK(rig.mc.enable(1, true).code == kCodeBusy);
    uint8_t velocity[] = {1, 0xC6, 0, 0, 60, 0, 100, 0, 3, 32, 0x6B};
    CHECK(rig.mc.command(velocity, sizeof(velocity)).code == 409);
}

static void test_home_success_needs_running_bit_then_clear() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    enableAndFeedStationary(rig, 1, 0);

    setMillis(40);
    CHECK(rig.mc.home(1, 0).code == kCodeQueued);

    // 0x02 only says the trigger arrived.
    injectRx(makeAck(1, kFrameHome, 0x02));
    setMillis(60);
    rig.mc.poll();
    CHECK(outcomeIs(rig, 1, "running"));

    // A "not homing, no failure" byte BEFORE the running bit was ever observed
    // proves nothing: 00 is also the power-on default.
    setMillis(80);
    injectRx(makeHomeStatus(1, 0x03));
    rig.mc.poll();
    CHECK(outcomeIs(rig, 1, "running"));
    CHECK(activeIs(rig, 1));

    // Bit2 observed set: the run is now genuinely in progress.
    setMillis(100);
    injectRx(makeHomeStatus(1, 0x07));
    rig.mc.poll();
    CHECK(outcomeIs(rig, 1, "running"));

    // ...then cleared with no failure bit: the inferred completion proof. The
    // proof alone completes nothing, and the pre-start sample does not count.
    setMillis(120);
    injectRx(makeHomeStatus(1, 0x03));
    rig.mc.poll();
    CHECK(outcomeIs(rig, 1, "running"));

    setMillis(140);
    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    rig.mc.poll();
    CHECK(outcomeIs(rig, 1, "running"));  // one distinct pair is not enough

    setMillis(160);
    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    rig.mc.poll();

    CHECK(outcomeIs(rig, 1, "done"));
    CHECK(!rig.mc.homeActive());
    CHECK(std::string(MotorControl::homeOutcomeName(rig.mc.homeOutcome())) == "done");
    CHECK(rig.mc.homeId() == 1);
    CHECK(status(rig, 1).find("\"state\":\"idle\"") != std::string::npos);
    CHECK(status(rig, 1).find("\"enabled\":true") != std::string::npos);
    // Homing can move the origin, so the pre-homing position is not reused.
    CHECK(status(rig, 1).find("\"positionDeg\":null") != std::string::npos);
}

static void test_home_explicit_9f_completion_is_enough() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    enableAndFeedStationary(rig, 1, 0);
    setMillis(40);
    CHECK(rig.mc.home(1, 0).code == kCodeQueued);

    // The documented active completion reply (manual p40) stands in for the
    // running-bit proof, still only with fresh post-start stationary feedback.
    injectRx(makeAck(1, kFrameHome, 0x9F));
    setMillis(60);
    rig.mc.poll();
    CHECK(outcomeIs(rig, 1, "running"));

    setMillis(80);
    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    rig.mc.poll();

    CHECK(outcomeIs(rig, 1, "done"));
    CHECK(!rig.mc.homeActive());
}

static void test_home_idle_status_alone_times_out() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    DebugLimits limits;
    limits.maxMoveDurationMs = 1000;  // the configured budget drives homing
    CHECK(rig.mc.setDebugLimits(limits));
    enableAndFeedStationary(rig, 1, 0);

    setMillis(40);
    CHECK(rig.mc.home(1, 0).code == kCodeQueued);
    injectRx(makeAck(1, kFrameHome, 0x02));
    setMillis(60);
    rig.mc.poll();

    // Only "not homing, no failure" ever arrives. That must never be reported as
    // a completion, however long it is fed.
    for (uint32_t t = 100; t <= 900; t += 100) {
        setMillis(t);
        injectRx(makeHomeStatus(1, 0x03));
        injectRx(makePosition(1, 0));
        injectRx(makeVelocity(1, 0));
        rig.mc.poll();
        CHECK(outcomeIs(rig, 1, "running"));
    }

    setMillis(1041);  // start (40 ms) + the 1000 ms configured budget
    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    rig.mc.poll();

    CHECK(outcomeIs(rig, 1, "failed"));
    CHECK(status(rig, 1).find("home_timeout") != std::string::npos);
    CHECK(sawStopFor(1));
    CHECK(countTxOpcode(0x9C, 1) == 1);  // 9C aborts the homing run
    CHECK(status(rig, 1).find("\"enabled\":false") != std::string::npos);
}

static void test_home_failure_bits_and_rejected_ack_fail() {
    // bit3: the device reports the homing run itself failed.
    Rig failed;
    failed.mc.begin(4, 5, 500000);
    enableAndFeedStationary(failed, 1, 0);
    setMillis(40);
    CHECK(failed.mc.home(1, 0).code == kCodeQueued);
    injectRx(makeAck(1, kFrameHome, 0x02));
    setMillis(60);
    failed.mc.poll();
    setMillis(80);
    injectRx(makeHomeStatus(1, 0x0C));  // running + homing failed
    failed.mc.poll();
    CHECK(outcomeIs(failed, 1, "failed"));
    CHECK(status(failed, 1).find("home_failed") != std::string::npos);
    CHECK(sawStopFor(1));
    CHECK(countTxOpcode(0x9C, 1) == 1);

    // bit4/bit5: an over-temp / over-current protection flag during the run.
    Rig hot;
    hot.mc.begin(4, 5, 500000);
    enableAndFeedStationary(hot, 1, 0);
    setMillis(40);
    CHECK(hot.mc.home(1, 0).code == kCodeQueued);
    injectRx(makeAck(1, kFrameHome, 0x02));
    setMillis(60);
    hot.mc.poll();
    setMillis(80);
    injectRx(makeHomeStatus(1, 0x14));  // running + over-temp
    hot.mc.poll();
    CHECK(outcomeIs(hot, 1, "failed"));
    CHECK(status(hot, 1).find("home_protection") != std::string::npos);

    // A rejected trigger answer faults immediately.
    Rig rejected;
    rejected.mc.begin(4, 5, 500000);
    enableAndFeedStationary(rejected, 1, 0);
    setMillis(40);
    CHECK(rejected.mc.home(1, 0).code == kCodeQueued);
    injectRx(makeAck(1, kFrameHome, 0xE2));
    setMillis(60);
    rejected.mc.poll();
    CHECK(outcomeIs(rejected, 1, "failed"));
    CHECK(status(rejected, 1).find("ack_rejected") != std::string::npos);
    CHECK(sawStopFor(1));
}

static void test_home_ack_timeout_fails() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    enableAndFeedStationary(rig, 1, 0);
    setMillis(40);
    CHECK(rig.mc.home(1, 0).code == kCodeQueued);

    // Fresh feedback, but the trigger is never answered.
    for (uint32_t t = 100; t <= 1500; t += 200) {
        setMillis(t);
        injectRx(makePosition(1, 0));
        injectRx(makeVelocity(1, 0));
        rig.mc.poll();
    }
    setMillis(1600);
    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    rig.mc.poll();

    CHECK(outcomeIs(rig, 1, "failed"));
    CHECK(status(rig, 1).find("home_ack_timeout") != std::string::npos);
    CHECK(sawStopFor(1));
}

static void test_home_no_motion_answer_is_its_own_outcome() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    enableAndFeedStationary(rig, 1, 0);
    setMillis(40);
    CHECK(rig.mc.home(1, 0).code == kCodeQueued);

    // Manual p40: 12/22 mean "already at the origin or the limit is already
    // triggered, the motor does not move". That is neither done nor a fault.
    injectRx(makeAck(1, kFrameHome, 0x12));
    setMillis(60);
    rig.mc.poll();

    CHECK(outcomeIs(rig, 1, "no_motion"));
    CHECK(!rig.mc.homeActive());
    CHECK(status(rig, 1).find("\"fault\":\"none\"") != std::string::npos);
    CHECK(status(rig, 1).find("home_no_motion") != std::string::npos);
    CHECK(!sawStopFor(1));  // nothing moved, so no stop is invented
    CHECK(countTxOpcode(0x9C, 1) == 0);
}

static void test_home_cancel_paths() {
    // stop(): 9C interrupts homing, FE halts the motor, outcome is cancelled.
    Rig stopped;
    stopped.mc.begin(4, 5, 500000);
    enableAndFeedStationary(stopped, 1, 0);
    setMillis(40);
    CHECK(stopped.mc.home(1, 0).code == kCodeQueued);
    injectRx(makeAck(1, kFrameHome, 0x02));
    setMillis(60);
    stopped.mc.poll();
    setMillis(80);
    CHECK(stopped.mc.stop(1).code == kCodeQueued);
    CHECK(outcomeIs(stopped, 1, "cancelled"));
    CHECK(!stopped.mc.homeActive());
    CHECK(countTxOpcode(0x9C, 1) == 1);
    CHECK(sawStopFor(1));
    // A late completion-style status byte cannot revive the cancelled run.
    setMillis(100);
    injectRx(makeHomeStatus(1, 0x03));
    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    stopped.mc.poll();
    CHECK(outcomeIs(stopped, 1, "cancelled"));

    // Explicit disable aborts the run too.
    Rig disabled;
    disabled.mc.begin(4, 5, 500000);
    enableAndFeedStationary(disabled, 1, 0);
    setMillis(40);
    CHECK(disabled.mc.home(1, 0).code == kCodeQueued);
    injectRx(makeAck(1, kFrameHome, 0x02));
    setMillis(60);
    disabled.mc.poll();
    setMillis(80);
    CHECK(disabled.mc.enable(1, false).code == kCodeQueued);
    CHECK(outcomeIs(disabled, 1, "cancelled"));
    CHECK(countTxOpcode(0x9C, 1) == 1);

    // The operator's own 0x9C frame aborts without a duplicate interrupt.
    Rig interrupted;
    interrupted.mc.begin(4, 5, 500000);
    enableAndFeedStationary(interrupted, 1, 0);
    setMillis(40);
    CHECK(interrupted.mc.home(1, 0).code == kCodeQueued);
    injectRx(makeAck(1, kFrameHome, 0x02));
    setMillis(60);
    interrupted.mc.poll();
    uint8_t interrupt[] = {1, 0x9C, 0x48, 0x6B};
    setMillis(80);
    CHECK(interrupted.mc.command(interrupt, sizeof(interrupt)).code == 202);
    CHECK(outcomeIs(interrupted, 1, "cancelled"));
    CHECK(countTxOpcode(0x9C, 1) == 1);  // exactly the operator's own frame
    CHECK(sawStopFor(1));

    // stopAll() aborts the run and still sends the id 0 broadcast.
    Rig all;
    all.mc.begin(4, 5, 500000);
    enableAndFeedStationary(all, 1, 0);
    setMillis(40);
    CHECK(all.mc.home(1, 0).code == kCodeQueued);
    injectRx(makeAck(1, kFrameHome, 0x02));
    setMillis(60);
    all.mc.poll();
    setMillis(80);
    CHECK(all.mc.stopAll().code == kCodeQueued);
    CHECK(outcomeIs(all, 1, "cancelled"));
    CHECK(countTxOpcode(0x9C, 1) == 1);
    CHECK(countTxTo(0, TxKind::Stop) == 1);
}

static void test_home_queries_3b_only_while_active() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    enableAndFeedStationary(rig, 1, 0);

    // No homing run: the rotation stays the four feedback fields.
    for (uint32_t t = 60; t <= 300; t += 40) {
        setMillis(t);
        rig.mc.poll();
    }
    CHECK(countTxOpcode(0x3B, 0) == 0);

    setMillis(340);
    CHECK(rig.mc.home(1, 0).code == kCodeQueued);
    injectRx(makeAck(1, kFrameHome, 0x02));

    for (uint32_t t = 360; t <= 700; t += 40) {
        setMillis(t);
        injectRx(makePosition(1, 0));
        injectRx(makeVelocity(1, 0));
        rig.mc.poll();
    }
    CHECK(countTxOpcode(0x3B, 1) > 0);  // homing status is polled for that node

    // Cancelling stops the extra poll: the existing cadence is not permanently
    // widened by the 3B field.
    setMillis(720);
    CHECK(rig.mc.stop(1).code == kCodeQueued);
    const uint32_t afterCancel = countTxOpcode(0x3B, 0);
    for (uint32_t t = 740; t <= 1100; t += 40) {
        setMillis(t);
        rig.mc.poll();
    }
    CHECK(countTxOpcode(0x3B, 0) == afterCancel);
}

static void test_home_param_bounds_and_power_on_report() {
    uint8_t frame[20];

    // Documented protocol ranges: 3000 RPM and a full uint32 timeout are legal
    // wire values, and the collision current follows the configured policy.
    buildHomeParams(frame, 0, 3000, 0xFFFFFFFFu, 1000, 0);
    DebugLimits permissive;
    permissive.maxSpeedTenths = 30000;
    CHECK(validateCommand(frame, 20, permissive) == CommandKind::Configure);
    CHECK(homeParamRefusal(frame, 20, permissive) == nullptr);

    // 3001 RPM is outside the manual's 0000-0BB8 and can never be written.
    buildHomeParams(frame, 0, 3001, 10000u, 1000, 0);
    CHECK(validateCommand(frame, 20, permissive) == CommandKind::Invalid);
    CHECK(std::string(homeParamRefusal(frame, 20, permissive)) == "home_velocity_out_of_range");

    // The same 3000 RPM write is refused while the configured speed policy is
    // lower (default 120 RPM), with its own reason rather than "unsupported".
    buildHomeParams(frame, 0, 3000, 10000u, 1000, 0);
    CHECK(validateCommand(frame, 20, DebugLimits{}) == CommandKind::Invalid);
    CHECK(std::string(homeParamRefusal(frame, 20, DebugLimits{})) == "home_velocity_out_of_policy");

    // The collision-detection current follows the configured current policy.
    DebugLimits lowCurrent;
    lowCurrent.maxCurrentMa = 500;
    buildHomeParams(frame, 0, 30, 10000u, 800, 0);
    CHECK(validateCommand(frame, 20, lowCurrent) == CommandKind::Invalid);
    CHECK(std::string(homeParamRefusal(frame, 20, lowCurrent)) == "home_current_out_of_range");

    // The power-on trigger is still unarmed, and now says so explicitly instead
    // of collapsing into the generic invalid-command answer.
    buildHomeParams(frame, 0, 30, 10000u, 800, 1);
    CHECK(validateCommand(frame, 20, DebugLimits{}) == CommandKind::Invalid);
    CHECK(std::string(homeParamRefusal(frame, 20, DebugLimits{})) == "power_on_homing_not_supported");
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    const Result powerOn = rig.mc.command(frame, 20);
    CHECK(powerOn.code == 400);
    CHECK(std::string(powerOn.message) == "power_on_homing_not_supported");

    // A layout error (mode 6) keeps the generic answer: it is malformed, not a
    // policy decision.
    buildHomeParams(frame, 6, 30, 10000u, 800, 0);
    CHECK(validateCommand(frame, 20, DebugLimits{}) == CommandKind::Invalid);
    CHECK(homeParamRefusal(frame, 20, DebugLimits{}) == nullptr);

    // The documented write path still works end to end on a stationary,
    // disabled node (0x4C is a configuration write, not a motion command).
    // The node must be SELECTED first: feedback is only tracked for nodes of
    // interest (selected / active job / pending enable or stop), so injecting
    // frames for an unselected node stores nothing and the gate then rejects
    // the write with "disable_and_wait_for_stationary_feedback".
    buildHomeParams(frame, 5, 30, 10000u, 800, 0);
    Rig writer;
    writer.mc.begin(4, 5, 500000);
    writer.mc.watch(1);
    const uint8_t flagData[] = {0x3A, 0x00, 0x6B};
    setMillis(0);
    injectRx(makeFrame(1, flagData, 3));
    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    writer.mc.poll();
    setMillis(40);
    const Result configured = writer.mc.command(frame, 20);
    CHECK(configured.code == 202);
    CHECK(std::string(configured.message) == "queued");
    // 18 payload bytes, split over three packets that each repeat the function
    // code (the vendor sendCommand packet layout).
    CHECK(countTxOpcode(0x4C, 1) == 3);
}

// --- Review regressions: phantom enable desire and stale homing proof -------

static void test_stop_clears_phantom_enable_desire() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);

    // An enable that was only REQUESTED (never confirmed) and then stopped.
    setMillis(0);
    CHECK(rig.mc.enable(1, true).code == kCodeQueued);
    setMillis(10);
    CHECK(rig.mc.stop(1).code == kCodeQueued);
    injectRx(makeAck(1, kFrameEnable, 0x02));  // late ack must not confirm it
    setMillis(40);
    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    rig.mc.poll();
    CHECK(status(rig, 1).find("\"enabled\":false") != std::string::npos);
    CHECK(status(rig, 1).find("\"state\":\"stop_requested\"") == std::string::npos);

    // Nothing is enabled and nothing is pending, so the node must not stay
    // "active": a phantom desire used to keep hasActiveMotion() true forever and
    // refused every parameter write with disable_and_wait_for_stationary_feedback.
    CHECK(!rig.mc.hasActiveMotion());

    const uint8_t flagData[] = {0x3A, 0x00, 0x6B};
    setMillis(60);
    injectRx(makeFrame(1, flagData, 3));
    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    rig.mc.poll();
    const uint8_t config[] = {1, 0x0A, 0x6D, 0x6B};
    setMillis(80);
    CHECK(rig.mc.command(config, sizeof(config)).code == 202);

    // A stop on a CONFIRMED enable keeps the desire (the driver stays enabled).
    Rig enabled;
    enabled.mc.begin(4, 5, 500000);
    enableAndConfirm(enabled, 1, 0);
    setMillis(40);
    CHECK(enabled.mc.stop(1).code == kCodeQueued);
    CHECK(enabled.mc.hasActiveMotion());
    CHECK(status(enabled, 1).find("\"enabled\":true") != std::string::npos);
}

static void test_home_inferred_proof_is_withdrawn_when_running_returns() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    enableAndFeedStationary(rig, 1, 0);
    setMillis(40);
    CHECK(rig.mc.home(1, 0).code == kCodeQueued);
    injectRx(makeAck(1, kFrameHome, 0x02));
    setMillis(60);
    rig.mc.poll();

    // Bit2 observed set, then cleared: one inferred proof, one counted pair.
    setMillis(80);
    injectRx(makeHomeStatus(1, 0x07));
    rig.mc.poll();
    setMillis(100);
    injectRx(makeHomeStatus(1, 0x03));
    rig.mc.poll();
    setMillis(120);
    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    rig.mc.poll();
    CHECK(outcomeIs(rig, 1, "running"));  // one pair is not completion

    // A NEWER status reports homing running again: the earlier "stopped"
    // observation is withdrawn instead of latching, so the accumulated pair can
    // never complete the run.
    setMillis(140);
    injectRx(makeHomeStatus(1, 0x07));
    rig.mc.poll();
    setMillis(160);
    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    rig.mc.poll();
    CHECK(outcomeIs(rig, 1, "running"));

    // A fresh clear plus two fresh post-clear pairs is what completes it.
    setMillis(180);
    injectRx(makeHomeStatus(1, 0x03));
    rig.mc.poll();
    setMillis(200);
    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    rig.mc.poll();
    CHECK(outcomeIs(rig, 1, "running"));
    setMillis(220);
    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    rig.mc.poll();
    CHECK(outcomeIs(rig, 1, "done"));
}

static void test_home_missing_status_fails_run() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    enableAndFeedStationary(rig, 1, 0);
    setMillis(40);
    CHECK(rig.mc.home(1, 0).code == kCodeQueued);
    injectRx(makeAck(1, kFrameHome, 0x02));
    setMillis(60);
    rig.mc.poll();
    CHECK(outcomeIs(rig, 1, "running"));

    // Position and velocity stay fresh, but the node never answers the 0x3B
    // probe: past the freshness grace the run fails instead of riding the long
    // deadline out with no evidence at all.
    for (uint32_t t = 100; t <= 700; t += 100) {
        setMillis(t);
        injectRx(makePosition(1, 0));
        injectRx(makeVelocity(1, 0));
        rig.mc.poll();
    }
    setMillis(720);
    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    rig.mc.poll();

    CHECK(outcomeIs(rig, 1, "failed"));
    CHECK(status(rig, 1).find("home_status_missing") != std::string::npos);
    CHECK(sawStopFor(1));

    // The explicit 9A/9F completion path is the documented exception: it needs no
    // 0x3B at all.
    Rig explicit_ok;
    explicit_ok.mc.begin(4, 5, 500000);
    enableAndFeedStationary(explicit_ok, 1, 0);
    setMillis(40);
    CHECK(explicit_ok.mc.home(1, 0).code == kCodeQueued);
    for (uint32_t t = 100; t <= 900; t += 200) {
        setMillis(t);
        injectRx(makePosition(1, 0));
        injectRx(makeVelocity(1, 0));
        explicit_ok.mc.poll();
        CHECK(outcomeIs(explicit_ok, 1, "running"));
    }
    injectRx(makeAck(1, kFrameHome, 0x9F));
    setMillis(1000);
    explicit_ok.mc.poll();
    setMillis(1020);
    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    explicit_ok.mc.poll();
    CHECK(outcomeIs(explicit_ok, 1, "done"));
}

// --- Direct (FB/CB) position -----------------------------------------------

static void test_direct_position_gate_layout_and_example_bytes() {
    uint8_t fb[12];
    buildDirect(fb, 2, 0, 300, 900, 2, 0);
    for (int i = 0; i < 12; ++i) CHECK(fb[i] == kDirectExample[i]);
    CHECK(validateCommand(fb, sizeof(fb)) == CommandKind::DirectMove);
    // Every truncation is rejected, exactly like the other motion commands.
    for (uint8_t n = 0; n < 12; ++n) {
        CHECK(validateCommand(fb, n) == CommandKind::Invalid);
    }
    // Leftover bytes are not "close enough" either.
    uint8_t tooLong[13];
    for (int i = 0; i < 12; ++i) tooLong[i] = fb[i];
    tooLong[12] = 0x6B;
    CHECK(validateCommand(tooLong, sizeof(tooLong)) == CommandKind::Invalid);

    uint8_t cb[14];
    buildDirectLimit(cb, 2, 0, 300, 900, 2, 0, 800);
    for (int i = 0; i < 14; ++i) CHECK(cb[i] == kDirectLimitExample[i]);
    CHECK(validateCommand(cb, sizeof(cb)) == CommandKind::DirectMove);
    CHECK(validateCommand(cb, 12) == CommandKind::Invalid);

    // Direction, motion mode and the sync flag are the board's gate: the cached
    // (sync 1) form stays explicitly unsupported.
    uint8_t bad[12];
    buildDirect(bad, 1, 2, 300, 900, 2, 0);
    CHECK(validateCommand(bad, sizeof(bad)) == CommandKind::Invalid);
    for (uint8_t mode = 0; mode <= 2; ++mode) {
        buildDirect(bad, 1, 0, 300, 900, mode, 0);
        CHECK(validateCommand(bad, sizeof(bad)) == CommandKind::DirectMove);
    }
    buildDirect(bad, 1, 0, 300, 900, 3, 0);
    CHECK(validateCommand(bad, sizeof(bad)) == CommandKind::Invalid);
    buildDirect(bad, 1, 0, 300, 900, 2, 1);
    CHECK(validateCommand(bad, sizeof(bad)) == CommandKind::Invalid);

    // Through the command gateway the cached form gets its own honest reason and
    // never reaches the bus.
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    const Result cached = rig.mc.command(bad, sizeof(bad));
    CHECK(cached.code == kCodeInvalid);
    CHECK(std::string(cached.message) == "direct_sync_not_supported");
    CHECK(countTx(TxKind::Direct) == 0);
}

static void test_direct_position_wire_frame_and_modes() {
    // Mode 2 (relative to the current position): the frame goes out unchanged.
    {
        Rig rig;
        rig.mc.begin(4, 5, 500000);
        enableAndFeedStationary(rig, 2, 0, 100);
        setMillis(60);
        CHECK(rig.mc.command(kDirectExample, sizeof(kDirectExample)).code == kCodeQueued);
        const TxRecord* sent = lastDirect();
        CHECK(sent != nullptr);
        CHECK(sent->bytes == std::vector<uint8_t>(
            kDirectExample, kDirectExample + sizeof(kDirectExample)));
        CHECK(sent->withCurrentLimit == false);
        CHECK(sent->dir == 0 && sent->vel == 300 && sent->magnitude == 900 &&
              sent->motionMode == 2 && sent->sync == false);
        // FB stays FB on the wire: the two CAN packets of this command both
        // repeat the function code (manual p42-43) and never become CD.
        CHECK(countTxOpcode(0xFB, 2) == 2);
        CHECK(countTxOpcode(0xCD, 2) == 0);
        CHECK(activeIs(rig, 2));
    }
    // Mode 0 (relative to the previous INPUT target): 2000 + 900 = 2900, never
    // the actual position plus 900.
    {
        Rig rig;
        rig.mc.begin(4, 5, 500000);
        enableWithTarget(rig, 2, 0, 100, 2000);
        uint8_t frame[12];
        buildDirect(frame, 2, 0, 300, 900, 0, 0);
        setMillis(60);
        CHECK(rig.mc.command(frame, sizeof(frame)).code == kCodeQueued);
        injectRx(makeAck(2, 0xFB, 0x02));
        setMillis(80);
        rig.mc.poll();
        // The position the mode-2 reading would have produced is NOT the target.
        injectRx(makeTarget(2, 1000));
        injectRx(makePosition(2, 1000));
        injectRx(makeVelocity(2, 0));
        setMillis(100);
        rig.mc.poll();
        CHECK(activeIs(rig, 2));
        // 2900 is.
        for (uint32_t t = 120; t <= 140; t += 20) {
            injectRx(makeTarget(2, 2900));
            injectRx(makePosition(2, 2900));
            injectRx(makeVelocity(2, 0));
            setMillis(t);
            rig.mc.poll();
        }
        CHECK(!activeIs(rig, 2));
    }
    // Mode 1 (absolute coordinate): CCW 90.0 degrees is -900, and the travel is
    // measured from the current position, not from the coordinate magnitude.
    {
        Rig rig;
        rig.mc.begin(4, 5, 500000);
        enableAndFeedStationary(rig, 1, 0, 100);
        uint8_t frame[12];
        buildDirect(frame, 1, 1, 300, 900, 1, 0);
        setMillis(60);
        CHECK(rig.mc.command(frame, sizeof(frame)).code == kCodeQueued);
        injectRx(makeAck(1, 0xFB, 0x02));
        setMillis(80);
        rig.mc.poll();
        for (uint32_t t = 100; t <= 120; t += 20) {
            injectRx(makeTarget(1, -900));
            injectRx(makePosition(1, -900));
            injectRx(makeVelocity(1, 0));
            setMillis(t);
            rig.mc.poll();
        }
        CHECK(!activeIs(rig, 1));
        CHECK(status(rig, 1).find("\"state\":\"idle\"") != std::string::npos);
    }
    // CB keeps its own opcode and adds the current field after sync.
    {
        Rig rig;
        rig.mc.begin(4, 5, 500000);
        enableAndFeedStationary(rig, 2, 0, 0);
        setMillis(60);
        CHECK(rig.mc.command(kDirectLimitExample, sizeof(kDirectLimitExample)).code == kCodeQueued);
        const TxRecord* sent = lastDirect();
        CHECK(sent != nullptr && sent->withCurrentLimit);
        CHECK(sent->bytes == std::vector<uint8_t>(
            kDirectLimitExample, kDirectLimitExample + sizeof(kDirectLimitExample)));
        CHECK(sent->currentMa == 800);
        CHECK(countTxOpcode(0xCB, 2) == 2);
        CHECK(countTxOpcode(0xFB, 2) == 0);
    }
}

static void test_direct_position_mode0_needs_fresh_target() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    enableAndFeedStationary(rig, 2, 0, 100);
    uint8_t frame[12];
    buildDirect(frame, 2, 0, 300, 900, 0, 0);

    // No 0x33 sample yet: the request is refused, nothing is sent, and a bounded
    // refresh is started instead of a target being guessed.
    CHECK(status(rig, 2).find("\"targetDeg\":null") != std::string::npos);
    setMillis(60);
    const Result missing = rig.mc.command(frame, sizeof(frame));
    CHECK(missing.code == kCodeUnavailable);
    CHECK(std::string(missing.message) == "target_not_fresh");
    CHECK(countTx(TxKind::Direct) == 0);
    CHECK(countTxOpcode(0x33, 2) == 0); // refresh is queued under the global budget
    CHECK(countTxOpcode(0x34, 2) == 0);  // p71's setpoint is never asked for

    // A fresh actual position is not a substitute, and a second attempt without
    // the answer is refused again.
    setMillis(80);
    injectRx(makePosition(2, 100));
    injectRx(makeVelocity(2, 0));
    rig.mc.poll();
    setMillis(80);
    CHECK(std::string(rig.mc.command(frame, sizeof(frame)).message) == "target_not_fresh");
    CHECK(countTx(TxKind::Direct) == 0);

    // 0x34 (p71, the real-time setpoint) is not a substitute: even a matching
    // setpoint sample leaves mode 0 without its baseline, and the board never
    // even asks for that read.
    injectRx(makeSetpoint(2, 2000));
    setMillis(90);
    rig.mc.poll();
    CHECK(std::string(rig.mc.command(frame, sizeof(frame)).message) == "target_not_fresh");
    CHECK(countTx(TxKind::Direct) == 0);
    CHECK(status(rig, 2).find("\"targetDeg\":null") != std::string::npos);
    CHECK(countTxOpcode(0x34, 2) == 0);

    // The answer arrives (0x33, the driver's target position): the retry is
    // executed against it.
    injectRx(makeTarget(2, 2000));
    setMillis(100);
    rig.mc.poll();
    CHECK(status(rig, 2).find("\"targetDeg\":200.0") != std::string::npos);
    setMillis(100);
    CHECK(rig.mc.command(frame, sizeof(frame)).code == kCodeQueued);
    CHECK(countTx(TxKind::Direct) == 1);

    // A stale sample is not used either, even while the position stays fresh.
    Rig stale;
    stale.mc.begin(4, 5, 500000);
    enableWithTarget(stale, 2, 0, 100, 2000);
    setMillis(700);
    injectRx(makePosition(2, 100));
    injectRx(makeVelocity(2, 0));
    stale.mc.poll();
    setMillis(700);
    const Result old = stale.mc.command(frame, sizeof(frame));
    CHECK(old.code == kCodeUnavailable);
    CHECK(std::string(old.message) == "target_not_fresh");
    // A separate rig, so this counts its own traffic: nothing was sent.
    CHECK(countTx(TxKind::Direct) == 0);
}

static void test_direct_position_completion_proof() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    enableAndFeedStationary(rig, 2, 0, 100);
    uint8_t frame[12];
    buildDirect(frame, 2, 0, 300, 900, 2, 0);  // mode 2 -> target = 100 + 900
    setMillis(60);
    CHECK(rig.mc.command(frame, sizeof(frame)).code == kCodeQueued);

    // 02 alone never completes a direct move.
    injectRx(makeAck(2, 0xFB, 0x02));
    setMillis(80);
    rig.mc.poll();
    CHECK(activeIs(rig, 2));

    // Position and velocity at the target are not enough either: the driver's own
    // target sample is part of the proof.
    injectRx(makePosition(2, 1000));
    injectRx(makeVelocity(2, 0));
    setMillis(100);
    rig.mc.poll();
    CHECK(activeIs(rig, 2));

    // One complete pair is still only one.
    injectRx(makeTarget(2, 1000));
    injectRx(makePosition(2, 1000));
    injectRx(makeVelocity(2, 0));
    setMillis(120);
    rig.mc.poll();
    CHECK(activeIs(rig, 2));

    // A driver target that is not this job's target resets the count.
    injectRx(makeTarget(2, 5000));
    injectRx(makePosition(2, 1000));
    injectRx(makeVelocity(2, 0));
    setMillis(140);
    rig.mc.poll();
    CHECK(activeIs(rig, 2));

    // Two distinct complete pairs with the resolved target finish it.
    for (uint32_t t = 160; t <= 180; t += 20) {
        injectRx(makeTarget(2, 1000));
        injectRx(makePosition(2, 1000));
        injectRx(makeVelocity(2, 0));
        setMillis(t);
        rig.mc.poll();
    }
    CHECK(!activeIs(rig, 2));
    CHECK(status(rig, 2).find("\"state\":\"idle\"") != std::string::npos);
}

static void test_direct_position_wrong_opcode_ack_never_confirms() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    enableAndFeedStationary(rig, 1, 0, 0);
    uint8_t frame[12];
    buildDirect(frame, 1, 0, 300, 900, 2, 0);
    setMillis(60);
    CHECK(rig.mc.command(frame, sizeof(frame)).code == kCodeQueued);

    // An ack of another opcode - the sibling CB form included - never sets
    // ackSeen for this job.
    injectRx(makeAck(1, kFrameMove, 0x02));
    injectRx(makeAck(1, 0xCB, 0x02));
    setMillis(80);
    rig.mc.poll();
    CHECK(activeIs(rig, 1));

    // Even with complete-looking feedback the job must time out rather than be
    // reported as acknowledged.
    for (uint32_t t = 100; t <= 1600; t += 100) {
        injectRx(makeTarget(1, 900));
        injectRx(makePosition(1, 900));
        injectRx(makeVelocity(1, 0));
        setMillis(t);
        rig.mc.poll();
    }
    CHECK(!activeIs(rig, 1));
    CHECK(sawStopFor(1));
    CHECK(status(rig, 1).find("\"fault\":\"move_ack_timeout\"") != std::string::npos);
    CHECK(status(rig, 1).find("\"enabled\":false") != std::string::npos);

    // The matching FB ack does confirm it, and the same feedback then finishes
    // the move.
    Rig ok;
    ok.mc.begin(4, 5, 500000);
    enableAndFeedStationary(ok, 1, 0, 0);
    setMillis(60);
    CHECK(ok.mc.command(frame, sizeof(frame)).code == kCodeQueued);
    injectRx(makeAck(1, 0xFB, 0x02));
    setMillis(80);
    ok.mc.poll();
    for (uint32_t t = 100; t <= 120; t += 20) {
        injectRx(makeTarget(1, 900));
        injectRx(makePosition(1, 900));
        injectRx(makeVelocity(1, 0));
        setMillis(t);
        ok.mc.poll();
    }
    CHECK(!activeIs(ok, 1));
}

static void test_direct_position_travel_limits_and_overflow() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    enableAndFeedStationary(rig, 1, 0, 0);
    uint8_t frame[12];
    const auto refuses = [&rig, &frame](const char* reason) {
        const Result r = rig.mc.command(frame, sizeof(frame));
        return r.code == kCodeInvalid && std::string(r.message) == reason;
    };

    setMillis(60);
    // Speed follows the configured policy (1200 tenths = 120.0 RPM by default).
    buildDirect(frame, 1, 0, 1201, 900, 2, 0);
    CHECK(refuses("speed_out_of_range"));
    // Modes 0 and 2 carry a displacement: it is bounded like the CD travel field.
    buildDirect(frame, 1, 0, 300, 36001, 2, 0);
    CHECK(refuses("angle_out_of_range"));
    buildDirect(frame, 1, 0, 300, 36001, 0, 0);
    CHECK(refuses("angle_out_of_range"));
    // Mode 1 is an absolute coordinate and is not clipped by the travel policy,
    // but it still has to fit the int32 the driver can report back.
    buildDirect(frame, 1, 0, 300, 0x80000000u, 1, 0);
    CHECK(refuses("angle_out_of_range"));
    // Its REAL travel is |target - current position|: a 3600.1 degree coordinate
    // from the origin is a 3600.1 degree travel and is refused as such.
    buildDirect(frame, 1, 0, 300, 36001, 1, 0);
    CHECK(refuses("travel_out_of_range"));
    // A non-zero travel needs a speed to be timed at all.
    buildDirect(frame, 1, 0, 0, 900, 2, 0);
    CHECK(refuses("speed_required"));
    // 3600.0 degrees at 0.1 RPM is 6000 s: past the configured duration policy
    // (the travel itself is still inside the travel policy).
    buildDirect(frame, 1, 0, 1, 36000, 2, 0);
    CHECK(refuses("duration_too_long"));
    // CB current: the documented 0..5000 mA subject to the policy, with no
    // arbitrary 100 mA floor.
    uint8_t cb[14];
    buildDirectLimit(cb, 1, 0, 300, 900, 2, 0, 5001);
    const Result over = rig.mc.command(cb, sizeof(cb));
    CHECK(over.code == kCodeInvalid);
    CHECK(std::string(over.message) == "current_out_of_range");
    // Nothing above ever reached the bus.
    CHECK(countTx(TxKind::Direct) == 0);
    CHECK(!activeIs(rig, 1));

    // A resolved target that no longer fits the int32 feedback bound is refused
    // instead of wrapping: position INT32_MAX + 900 tenths.
    Rig high;
    high.mc.begin(4, 5, 500000);
    enableAndFeedStationary(high, 1, 0, 0x7FFFFFFF);
    buildDirect(frame, 1, 0, 300, 900, 2, 0);
    setMillis(60);
    const Result overflow = high.mc.command(frame, sizeof(frame));
    CHECK(overflow.code == kCodeInvalid);
    CHECK(std::string(overflow.message) == "target_out_of_range");
    CHECK(countTx(TxKind::Direct) == 0);

    // A large absolute coordinate with a small travel is legal: 3000.0 degrees
    // of coordinate from a position 3000.0 degrees away is no travel at all.
    Rig far;
    far.mc.begin(4, 5, 500000);
    enableAndFeedStationary(far, 1, 0, 30000);
    buildDirect(frame, 1, 0, 300, 36000, 1, 0);
    setMillis(60);
    CHECK(far.mc.command(frame, sizeof(frame)).code == kCodeQueued);
    CHECK(lastDirect() != nullptr && lastDirect()->magnitude == 36000);
}

static void test_direct_position_zero_travel_noop_and_cb_current() {
    // An absolute zero target is a valid coordinate; with the motor already
    // there it is a zero-travel no-op that needs no speed.
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    enableAndFeedStationary(rig, 1, 0, 0);
    uint8_t frame[12];
    buildDirect(frame, 1, 1, 0, 0, 1, 0);
    setMillis(60);
    CHECK(rig.mc.command(frame, sizeof(frame)).code == kCodeQueued);
    // It is still supervised by actual feedback: ack plus two complete pairs.
    injectRx(makeAck(1, 0xFB, 0x02));
    setMillis(80);
    rig.mc.poll();
    CHECK(activeIs(rig, 1));
    for (uint32_t t = 100; t <= 120; t += 20) {
        injectRx(makeTarget(1, 0));
        injectRx(makePosition(1, 0));
        injectRx(makeVelocity(1, 0));
        setMillis(t);
        rig.mc.poll();
    }
    CHECK(!activeIs(rig, 1));
    CHECK(status(rig, 1).find("\"state\":\"idle\"") != std::string::npos);

    // The same zero coordinate with the motor elsewhere is real travel.
    Rig away;
    away.mc.begin(4, 5, 500000);
    enableAndFeedStationary(away, 1, 0, 100);
    setMillis(60);
    CHECK(std::string(away.mc.command(frame, sizeof(frame)).message) == "speed_required");
    buildDirect(frame, 1, 1, 10, 0, 1, 0);  // 1.0 RPM
    CHECK(away.mc.command(frame, sizeof(frame)).code == kCodeQueued);

    // CB accepts the documented 0..5000 mA subject to the policy.
    Rig cb;
    cb.mc.begin(4, 5, 500000);
    enableAndFeedStationary(cb, 1, 0, 0);
    uint8_t cbframe[14];
    buildDirectLimit(cbframe, 1, 0, 300, 900, 2, 0, 0);
    setMillis(60);
    CHECK(cb.mc.command(cbframe, sizeof(cbframe)).code == kCodeQueued);
    CHECK(lastDirect() != nullptr && lastDirect()->currentMa == 0);

    Rig low;
    low.mc.begin(4, 5, 500000);
    enableAndFeedStationary(low, 1, 0, 0);
    buildDirectLimit(cbframe, 1, 0, 300, 900, 2, 0, 50);  // no 100 mA floor
    setMillis(60);
    CHECK(low.mc.command(cbframe, sizeof(cbframe)).code == kCodeQueued);
    CHECK(lastDirect() != nullptr && lastDirect()->currentMa == 50);

    // FB cannot carry a current limit at all: the 12-byte form has no such field
    // and the board never invents one.
    Rig alone;
    alone.mc.begin(4, 5, 500000);
    DebugLimits tight;
    tight.maxCurrentMa = 100;
    CHECK(alone.mc.setDebugLimits(tight));
    enableAndFeedStationary(alone, 2, 0, 0);
    setMillis(60);
    CHECK(alone.mc.command(kDirectExample, sizeof(kDirectExample)).code == kCodeQueued);
    const TxRecord* sent = lastDirect();
    CHECK(sent != nullptr && !sent->withCurrentLimit && sent->currentMa == 0);
    CHECK(sent->bytes.size() == 12);
}

static void test_direct_position_cancel_timeout_tx_fail_and_invalidation() {
    // A stop cancels the supervised job without disabling the driver.
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    enableAndFeedStationary(rig, 1, 0, 0);
    uint8_t frame[12];
    buildDirect(frame, 1, 0, 300, 900, 2, 0);
    setMillis(60);
    CHECK(rig.mc.command(frame, sizeof(frame)).code == kCodeQueued);
    setMillis(80);
    CHECK(rig.mc.stop(1).code == kCodeQueued);
    CHECK(!activeIs(rig, 1));
    CHECK(status(rig, 1).find("\"enabled\":true") != std::string::npos);
    // Fresh stationary feedback confirms the stop; the enable survives it.
    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    setMillis(100);
    rig.mc.poll();
    CHECK(status(rig, 1).find("\"state\":\"idle\"") != std::string::npos);
    CHECK(status(rig, 1).find("\"enabled\":true") != std::string::npos);

    // The deadline is the configured maxMoveDurationMs, not an invented ramp.
    Rig slow;
    slow.mc.begin(4, 5, 500000);
    DebugLimits limits;
    limits.maxMoveDurationMs = 2000;
    CHECK(slow.mc.setDebugLimits(limits));
    enableAndFeedStationary(slow, 1, 0, 0);
    setMillis(60);
    CHECK(slow.mc.command(frame, sizeof(frame)).code == kCodeQueued);
    injectRx(makeAck(1, 0xFB, 0x02));
    setMillis(80);
    slow.mc.poll();
    for (uint32_t t = 100; t <= 2000; t += 100) {
        // Fresh feedback that never reaches the target keeps the job running.
        injectRx(makeTarget(1, 0));
        injectRx(makePosition(1, 0));
        injectRx(makeVelocity(1, 100));
        setMillis(t);
        slow.mc.poll();
    }
    CHECK(activeIs(slow, 1));
    injectRx(makeTarget(1, 0));
    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 100));
    setMillis(2200);
    slow.mc.poll();
    CHECK(!activeIs(slow, 1));
    CHECK(sawStopFor(1));
    CHECK(status(slow, 1).find("\"fault\":\"move_timeout\"") != std::string::npos);

    // A partial transmission stops the motor and latches; nothing is assumed.
    Rig tx;
    tx.mc.begin(4, 5, 500000);
    enableAndFeedStationary(tx, 1, 0, 0);
    setMillis(60);
    failNextMoveTx = true;
    const Result failed = tx.mc.command(frame, sizeof(frame));
    CHECK(failed.code == kCodeUnavailable);
    CHECK(std::string(failed.message) == "can_tx_failed");
    CHECK(sawStopFor(1));
    CHECK(status(tx, 1).find("\"fault\":\"move_tx_failed\"") != std::string::npos);
    CHECK(!activeIs(tx, 1));

    // Losing the feedback faults the job instead of riding the deadline out.
    Rig gone;
    gone.mc.begin(4, 5, 500000);
    enableAndFeedStationary(gone, 1, 0, 0);
    setMillis(60);
    CHECK(gone.mc.command(frame, sizeof(frame)).code == kCodeQueued);
    injectRx(makeAck(1, 0xFB, 0x02));
    setMillis(80);
    gone.mc.poll();
    setMillis(800);
    gone.mc.poll();
    CHECK(!activeIs(gone, 1));
    CHECK(status(gone, 1).find("\"fault\":\"feedback_stale\"") != std::string::npos);

    // The target sample does not survive a stop: mode 0 needs a NEW 0x33 read.
    Rig inv;
    inv.mc.begin(4, 5, 500000);
    enableWithTarget(inv, 2, 0, 100, 2000);
    uint8_t mode0[12];
    buildDirect(mode0, 2, 0, 300, 900, 0, 0);
    setMillis(60);
    CHECK(inv.mc.command(mode0, sizeof(mode0)).code == kCodeQueued);
    setMillis(80);
    CHECK(inv.mc.stop(2).code == kCodeQueued);
    injectRx(makePosition(2, 100));
    injectRx(makeVelocity(2, 0));
    setMillis(100);
    inv.mc.poll();
    setMillis(100);
    const Result afterStop = inv.mc.command(mode0, sizeof(mode0));
    CHECK(afterStop.code == kCodeUnavailable);
    CHECK(std::string(afterStop.message) == "target_not_fresh");
    injectRx(makeTarget(2, 2000));
    setMillis(120);
    inv.mc.poll();
    setMillis(120);
    CHECK(inv.mc.command(mode0, sizeof(mode0)).code == kCodeQueued);

    // Nor does it survive a raw frame the board did not supervise.
    Rig raw;
    raw.mc.begin(4, 5, 500000);
    enableWithTarget(raw, 2, 0, 100, 2000);
    setMillis(60);
    CHECK(raw.mc.command(mode0, sizeof(mode0)).code == kCodeQueued);
    raw.mc.noteRawTransmission(2);
    CHECK(!activeIs(raw, 2));
    enableAndFeedStationary(raw, 2, 100, 100);
    setMillis(160);
    const Result afterRaw = raw.mc.command(mode0, sizeof(mode0));
    CHECK(afterRaw.code == kCodeUnavailable);
    CHECK(std::string(afterRaw.message) == "target_not_fresh");
}

static void test_direct_position_dispatch_api_and_cb_modes() {
    // The public API is dispatched without the byte gate, so it is exercised
    // directly - and it refuses the cached form itself instead of relying on the
    // gate. Every case gets its own rig, because one supervised job at a time is
    // the rule being respected.
    for (uint8_t mode = 0; mode <= 2; ++mode) {
        Rig rig;
        rig.mc.begin(4, 5, 500000);
        enableWithTarget(rig, 3, 0, 100, 2000);

        DirectPositionRequest request;
        request.id = 3;
        request.direction = mode == 1 ? 1 : 0;  // CCW for the absolute case
        request.angleTenths = 900;
        request.motionMode = mode;
        request.speedTenths = 300;
        // CB for every mode, so the current field travels with all three.
        request.withCurrentLimit = true;
        request.currentMa = 0;  // 0 mA is inside the documented 0..5000 range

        setMillis(60);
        const Result queued = rig.mc.directPosition(request);
        CHECK(queued.code == kCodeQueued);
        const TxRecord* sent = lastDirect();
        CHECK(sent != nullptr);
        CHECK(sent->withCurrentLimit && sent->bytes.size() == 14);
        CHECK(sent->bytes[1] == 0xCB);
        CHECK(sent->motionMode == mode && sent->currentMa == 0);
        CHECK(sent->bytes[11] == 0 && sent->bytes[12] == 0);
        CHECK(activeIs(rig, 3));

        // The cached form is refused by the API itself, and nothing is sent.
        DirectPositionRequest cached = request;
        cached.sync = true;
        const Result refused = rig.mc.directPosition(cached);
        CHECK(refused.code == kCodeInvalid);
        CHECK(std::string(refused.message) == "direct_sync_not_supported");
        CHECK(countTx(TxKind::Direct) == 1);

        // The value policy is applied on this path too.
        DirectPositionRequest over = request;
        over.currentMa = 5001;
        const Result tooMuch = rig.mc.directPosition(over);
        CHECK(tooMuch.code == kCodeInvalid);
        CHECK(std::string(tooMuch.message) == "current_out_of_range");
        CHECK(countTx(TxKind::Direct) == 1);
    }

    // The byte path parses the same fields for CB, including mode 0 with a real
    // current limit, and the frame reaches the wire byte for byte.
    Rig bytes;
    bytes.mc.begin(4, 5, 500000);
    enableWithTarget(bytes, 2, 0, 100, 2000);
    uint8_t frame[14];
    buildDirectLimit(frame, 2, 0, 300, 900, 0, 0, 800);
    setMillis(60);
    CHECK(bytes.mc.command(frame, sizeof(frame)).code == kCodeQueued);
    const TxRecord* sent = lastDirect();
    CHECK(sent != nullptr && sent->withCurrentLimit && sent->currentMa == 800);
    CHECK(sent->motionMode == 0 && sent->magnitude == 900);
    CHECK(sent->bytes == std::vector<uint8_t>(frame, frame + sizeof(frame)));

    // The cached CB form is refused with its own reason through the gateway too.
    uint8_t cachedCb[14];
    buildDirectLimit(cachedCb, 2, 0, 300, 900, 0, 1, 800);
    const Result refusedCb = bytes.mc.command(cachedCb, sizeof(cachedCb));
    CHECK(refusedCb.code == kCodeInvalid);
    CHECK(std::string(refusedCb.message) == "direct_sync_not_supported");
    CHECK(countTx(TxKind::Direct) == 1);

    // A malformed frame keeps the generic answer: a bad checksum, a broadcast
    // address and a truncation are not reported as a sync-flag decision.
    uint8_t badChecksum[12];
    buildDirect(badChecksum, 2, 0, 300, 900, 2, 1);
    badChecksum[11] = 0x00;
    CHECK(std::string(bytes.mc.command(badChecksum, sizeof(badChecksum)).message) ==
          "unsupported_or_invalid_command");
    uint8_t broadcast[12];
    buildDirect(broadcast, 0, 0, 300, 900, 2, 1);
    CHECK(std::string(bytes.mc.command(broadcast, sizeof(broadcast)).message) ==
          "unsupported_or_invalid_command");

    // The refusal helper itself: it never reads past the bytes it was given, and
    // a structural rejection (address, checksum, length) is never reported as a
    // sync-flag decision.
    const uint8_t oneByte[1] = {0xFB};
    CHECK(directPositionRefusal(oneByte, 0) == nullptr);
    CHECK(directPositionRefusal(oneByte, 1) == nullptr);
    CHECK(directPositionRefusal(nullptr, 12) == nullptr);
    CHECK(directPositionRefusal(cachedCb, 13) == nullptr);
    CHECK(directPositionRefusal(badChecksum, sizeof(badChecksum)) == nullptr);
    CHECK(directPositionRefusal(broadcast, sizeof(broadcast)) == nullptr);
    // A well-formed cached frame still gets its own reason.
    CHECK(std::string(directPositionRefusal(cachedCb, sizeof(cachedCb))) ==
          "direct_sync_not_supported");
}

static void test_paused_page_polling_keeps_direct_move_feedback() {
    Rig rig;
    rig.mc.begin(4, 5, 500000);
    enableAndFeedStationary(rig, 1, 0);

    setMillis(60);
    rig.mc.setAutoQueriesEnabled(false);
    rig.mc.watch(2);  // A selected idle page must not generate traffic.
    capturedTX.clear();
    txLog.clear();
    setMillis(700);
    rig.mc.poll();
    CHECK(countTx(TxKind::ReadSysParam) == 0);

    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    setMillis(720);
    rig.mc.poll();
    uint8_t move[14];
    buildDirectLimit(move, 1, 0, 300, 900, 2, 0, 800);
    setMillis(740);
    CHECK(rig.mc.command(move, sizeof(move)).code == kCodeQueued);
    injectRx(makeAck(1, 0xCB, 0x02));
    setMillis(760);
    rig.mc.poll();

    uint32_t targetQueryAt = 0;
    for (uint32_t t = 860; t <= 1760; t += 100) {
        injectRx(makePosition(1, 0));
        injectRx(makeVelocity(1, 0));
        setMillis(t);
        rig.mc.poll();
        if (countTxOpcode(0x33, 1) != 0) { targetQueryAt = t; break; }
    }
    CHECK(targetQueryAt != 0);
    if (!targetQueryAt) return;

    injectRx(makeTarget(1, 900));
    injectRx(makePosition(1, 900));
    injectRx(makeVelocity(1, 0));
    setMillis(targetQueryAt + 10);
    rig.mc.poll();
    injectRx(makePosition(1, 900));
    injectRx(makeVelocity(1, 0));
    setMillis(targetQueryAt + 30);
    rig.mc.poll();
    CHECK(!activeIs(rig, 1));
    CHECK(status(rig, 1).find("\"fault\":\"none\"") != std::string::npos);
    CHECK(status(rig, 1).find("\"enabled\":true") != std::string::npos);
}

static void test_direct_position_target_proof_uses_33_not_34() {
    // A direct job whose target is 100 + 900 = 1000 (mode 2).
    // A matching 0x34 real-time setpoint is never the proof: the position sits
    // exactly on the target, the motor is stopped, and the job still must not be
    // reported as done - it ends on the configured deadline instead.
    {
        Rig rig;
        rig.mc.begin(4, 5, 500000);
        DebugLimits limits;
        limits.maxMoveDurationMs = 2000;
        CHECK(rig.mc.setDebugLimits(limits));
        enableAndFeedStationary(rig, 2, 0, 100);
        uint8_t frame[12];
        buildDirect(frame, 2, 0, 300, 900, 2, 0);
        setMillis(60);
        CHECK(rig.mc.command(frame, sizeof(frame)).code == kCodeQueued);
        injectRx(makeAck(2, 0xFB, 0x02));
        setMillis(80);
        rig.mc.poll();
        for (uint32_t t = 100; t <= 2100; t += 100) {
            injectRx(makeSetpoint(2, 1000));  // p71, ignored on purpose
            injectRx(makePosition(2, 1000));
            injectRx(makeVelocity(2, 0));
            setMillis(t);
            rig.mc.poll();
        }
        CHECK(!activeIs(rig, 2));
        CHECK(sawStopFor(2));
        CHECK(status(rig, 2).find("\"fault\":\"move_timeout\"") != std::string::npos);
    }

    // A 0x33 that reports a different target position is not this job's target,
    // even while the actual position is exactly where the job asked it to be.
    {
        Rig rig;
        rig.mc.begin(4, 5, 500000);
        enableAndFeedStationary(rig, 2, 0, 100);
        uint8_t frame[12];
        buildDirect(frame, 2, 0, 300, 900, 2, 0);
        setMillis(60);
        CHECK(rig.mc.command(frame, sizeof(frame)).code == kCodeQueued);
        injectRx(makeAck(2, 0xFB, 0x02));
        setMillis(80);
        rig.mc.poll();
        for (uint32_t t = 100; t <= 160; t += 20) {
            injectRx(makeTarget(2, 4000));  // distant target position
            injectRx(makePosition(2, 1000));  // exactly on the expected position
            injectRx(makeVelocity(2, 0));
            setMillis(t);
            rig.mc.poll();
            CHECK(activeIs(rig, 2));
        }
        // Two distinct pairs that report the resolved target finish it.
        for (uint32_t t = 180; t <= 200; t += 20) {
            injectRx(makeTarget(2, 1000));
            injectRx(makePosition(2, 1000));
            injectRx(makeVelocity(2, 0));
            setMillis(t);
            rig.mc.poll();
        }
        CHECK(!activeIs(rig, 2));
        CHECK(status(rig, 2).find("\"state\":\"idle\"") != std::string::npos);
    }
}

// One 0x3A driver flag byte: bit0 is the driver's own enable state.
static CanRawFrame makeFlags(uint8_t addr, uint8_t flags) {
    const uint8_t data[3] = {0x3A, flags, 0x6B};
    return makeFrame(addr, data, sizeof(data));
}

// 0x45: global closed-loop maximum phase current (manual V1.0.5 p82, 5.6.13).
static void buildClosedLoopCurrent(uint8_t* frame, uint8_t addr, uint8_t save,
                                   uint16_t currentMa) {
    frame[0] = addr;
    frame[1] = 0x45;
    frame[2] = 0x66;
    frame[3] = save;
    frame[4] = static_cast<uint8_t>(currentMa >> 8);
    frame[5] = static_cast<uint8_t>(currentMa & 0xFF);
    frame[6] = 0x6B;
}

// Enable, then disable again with a fresh stationary pair and a driver flag byte
// that reports "not enabled": the state a 0x45 parameter write requires.
static void disableAndFeedStationary(Rig& rig, uint8_t id, uint32_t atMs) {
    enableAndConfirm(rig, id, atMs);
    setMillis(atMs + 40);
    rig.mc.enable(id, false);
    injectRx(makeAck(id, kFrameEnable, 0x02));
    setMillis(atMs + 60);
    rig.mc.poll();
    setMillis(atMs + 80);
    injectRx(makePosition(id, 0));
    injectRx(makeVelocity(id, 0));
    injectRx(makeFlags(id, 0x00));
    rig.mc.poll();
}

static void test_closed_loop_current_limit_write() {
    // The manual's own examples: 120 mA unsaved and 100 mA saved.
    uint8_t frame[7];
    buildClosedLoopCurrent(frame, 1, 0, 120);
    const uint8_t example[7] = {1, 0x45, 0x66, 0x00, 0x00, 0x78, 0x6B};
    for (int i = 0; i < 7; ++i) CHECK(frame[i] == example[i]);
    buildClosedLoopCurrent(frame, 1, 1, 100);
    const uint8_t saved[7] = {1, 0x45, 0x66, 0x01, 0x00, 0x64, 0x6B};
    for (int i = 0; i < 7; ++i) CHECK(frame[i] == saved[i]);

    // Layout: exact length, auxiliary byte 66, save 00/01, documented 0..5000 mA.
    buildClosedLoopCurrent(frame, 1, 0, 120);
    CHECK(validateCommand(frame, 7) == CommandKind::Configure);
    for (uint8_t n = 0; n < 7; ++n) CHECK(validateCommand(frame, n) == CommandKind::Invalid);
    uint8_t tooLong[8];
    for (int i = 0; i < 7; ++i) tooLong[i] = frame[i];
    tooLong[7] = 0x6B;
    CHECK(validateCommand(tooLong, 8) == CommandKind::Invalid);
    uint8_t bad[7];
    buildClosedLoopCurrent(bad, 1, 0, 120);
    bad[2] = 0x33;  // 0x44's auxiliary byte is not 0x45's
    CHECK(validateCommand(bad, 7) == CommandKind::Invalid);
    buildClosedLoopCurrent(bad, 1, 2, 120);
    CHECK(validateCommand(bad, 7) == CommandKind::Invalid);
    buildClosedLoopCurrent(bad, 1, 0, 5001);
    CHECK(validateCommand(bad, 7) == CommandKind::Invalid);
    buildClosedLoopCurrent(bad, 1, 0, 5000);
    CHECK(validateCommand(bad, 7) == CommandKind::Configure);
    // A broadcast write is never accepted, and neither is a missing checksum.
    buildClosedLoopCurrent(bad, 0, 0, 120);
    CHECK(validateCommand(bad, 7) == CommandKind::Invalid);
    buildClosedLoopCurrent(bad, 1, 0, 120);
    bad[6] = 0x00;
    CHECK(validateCommand(bad, 7) == CommandKind::Invalid);

    // Policy: the configured current ceiling applies to this write too, and the
    // reason is the honest one instead of a generic layout error.
    DebugLimits tight;
    tight.maxCurrentMa = 100;
    buildClosedLoopCurrent(frame, 1, 0, 120);
    CHECK(validateCommand(frame, 7, tight) == CommandKind::Invalid);
    CHECK(std::string(closedLoopCurrentRefusal(frame, 7, tight)) == "current_out_of_range");
    buildClosedLoopCurrent(frame, 1, 0, 100);
    CHECK(validateCommand(frame, 7, tight) == CommandKind::Configure);
    CHECK(closedLoopCurrentRefusal(frame, 7, tight) == nullptr);
    // Layout problems and the documented 0..5000 range keep the generic answer.
    buildClosedLoopCurrent(frame, 1, 0, 5001);
    CHECK(closedLoopCurrentRefusal(frame, 7, tight) == nullptr);
    CHECK(closedLoopCurrentRefusal(frame, 6, tight) == nullptr);
    CHECK(closedLoopCurrentRefusal(nullptr, 7, tight) == nullptr);

    // Happy path: disabled driver + fresh stationary feedback, so the exact
    // manual frame reaches the wire once and nothing else is sent.
    {
        Rig rig;
        rig.mc.begin(4, 5, 500000);
        disableAndFeedStationary(rig, 1, 0);

        setMillis(80);
        buildClosedLoopCurrent(frame, 1, 0, 120);
        capturedTX.clear();
        txLog.clear();
        const Result queued = rig.mc.command(frame, sizeof(frame));
        CHECK(queued.code == kCodeQueued);
        CHECK(txLog.empty());                 // a configure write is not a Move
        CHECK(countTxOpcode(0x45, 1) == 1);   // 7 bytes: one CAN packet
        CHECK(capturedTX.size() == 1);
        CHECK(capturedTX[0].data[0] == 0x45 && capturedTX[0].data[1] == 0x66 &&
              capturedTX[0].data[2] == 0x00 && capturedTX[0].data[3] == 0x00 &&
              capturedTX[0].data[4] == 0x78 && capturedTX[0].data[5] == 0x6B);
        // The write drops the position/velocity/flags confirmations, so it can
        // never be mistaken for a state that survived the parameter change.
        CHECK(status(rig, 1).find("\"online\":false") != std::string::npos);
    }

    // Enabled driver: the board refuses with the existing configure gate and
    // sends nothing.
    {
        Rig rig;
        rig.mc.begin(4, 5, 500000);
        enableAndFeedStationary(rig, 1, 0, 0);
        setMillis(60);
        injectRx(makeFlags(1, 0x01));  // driver reports "enabled"
        rig.mc.poll();
        buildClosedLoopCurrent(frame, 1, 0, 120);
        setMillis(80);
        const Result busy = rig.mc.command(frame, sizeof(frame));
        CHECK(busy.code == kCodeBusy);
        CHECK(std::string(busy.message) == "disable_and_wait_for_stationary_feedback");
        CHECK(countTxOpcode(0x45, 1) == 0);
    }

    // Still moving, with the enable already dropped: the stationary gate is what
    // refuses (the position stays fresh, only the speed is outside the band).
    {
        Rig rig;
        rig.mc.begin(4, 5, 500000);
        disableAndFeedStationary(rig, 1, 0);
        setMillis(120);
        injectRx(makeVelocity(1, 300));  // 30 RPM: not stationary
        rig.mc.poll();
        setMillis(140);
        buildClosedLoopCurrent(frame, 1, 1, 100);
        const Result moving = rig.mc.command(frame, sizeof(frame));
        CHECK(moving.code == kCodeBusy);
        CHECK(std::string(moving.message) == "disable_and_wait_for_stationary_feedback");
        CHECK(countTxOpcode(0x45, 1) == 0);
    }
    {
        Rig rig;
        rig.mc.begin(4, 5, 500000);
        DebugLimits tightLimits;
        tightLimits.maxCurrentMa = 100;
        CHECK(rig.mc.setDebugLimits(tightLimits));
        disableAndFeedStationary(rig, 1, 0);
        setMillis(80);
        buildClosedLoopCurrent(frame, 1, 0, 120);
        const Result refused = rig.mc.command(frame, sizeof(frame));
        CHECK(refused.code == kCodeInvalid);
        CHECK(std::string(refused.message) == "current_out_of_range");
        CHECK(countTxOpcode(0x45, 1) == 0);
    }
}

static void test_stop_all_does_not_invent_unseen_target() {
    Rig rig;
    CHECK(rig.mc.begin(4, 5, 500000));
    // A browser selection is not evidence of a connected or commanded motor.
    rig.mc.watch(99);
    CHECK(rig.mc.stopAll().code == kCodeQueued);
    CHECK(countTx(TxKind::Stop) > 0);
    CHECK(!rig.mc.snapshot(99).stopPending);
    CHECK(!rig.mc.operationBusy());
    enableAndConfirm(rig, 2, 100);
    CHECK(rig.mc.snapshot(2).enabled);
    // A real, confirmed target must still provide fresh stationary feedback.
    rig.mc.watch(99);
    setMillis(200);
    CHECK(rig.mc.stopAll().code == kCodeQueued);
    CHECK(rig.mc.snapshot(2).stopPending);
    CHECK(!rig.mc.snapshot(99).stopPending);
    CHECK(rig.mc.operationBusy());
    injectRx(makePosition(2, 0));
    injectRx(makeVelocity(2, 0));
    setMillis(220);
    rig.mc.poll();
    CHECK(!rig.mc.operationBusy());
    CHECK(rig.mc.snapshot(2).enabled);
    // An explicit stop to an unresponsive node remains unconfirmed.
    CHECK(rig.mc.stop(99).code == kCodeQueued);
    CHECK(rig.mc.snapshot(99).stopPending);
    CHECK(rig.mc.operationBusy());
    CHECK(status(rig, 2).find("\"id\":99,\"reason\":\"stop_pending\"") != std::string::npos);
    const auto savedLimits = rig.mc.debugLimits();
    const size_t beforeClear = capturedTX.size();
    rig.mc.clearControlState();
    CHECK(!rig.mc.operationBusy());
    CHECK(!rig.mc.hasFault());
    CHECK(!rig.mc.snapshot(2).enabled);
    CHECK(!rig.mc.snapshot(2).positionValid);
    CHECK(rig.mc.debugLimits().maxCurrentMa == savedLimits.maxCurrentMa);
    CHECK(capturedTX.size() == beforeClear); // no enable, move, retry or CAN reinit
    injectRx(makeAck(2, kFrameEnable, 0x02));
    setMillis(240);
    rig.mc.poll();
    CHECK(!rig.mc.snapshot(2).enabled); // stale ACK cannot resurrect confirmation
    rig.mc.watch(2);
    injectRx(makeAck(2, kFrameEnable, 0xE2));
    injectRx(makeAck(2, kFrameMove, 0xEE));
    injectRx(makeAck(2, kFrameStop, 0xE2));
    setMillis(260);
    rig.mc.poll();
    CHECK(!rig.mc.hasFault());
    CHECK(!rig.mc.operationBusy());
}

struct TestCase {
    const char* name;
    void (*fn)();
};

int main() {
    const TestCase tests[] = {
        {"broadcast stop does not lock the board on an unseen UI selection", test_stop_all_does_not_invent_unseen_target},
        {"configurable limits and trial timeout, continuous freshness guard", test_configurable_debug_limits},
        {"protocol truncation, limits and sync rejection", test_protocol_validation_bounds},
        {"experiment 5s deadline and failed TX stop", test_experiment_deadline_and_partial_failure},
        {"protocol whitelist, trace, guarded experiment and WiFi gate", test_protocol_gateway},
        {"raw diagnostics preserve unexpected replies and actual flags", test_diagnostics_capture_unrecognized_reply_and_flags},
        {"begin sends no motion", test_begin_sends_no_motion},
        {"watch/select 1..255 not exhausted", test_watch_select_1_to_255_not_exhausted},
        {"global query budget and repeated demands past 600ms", test_query_global_budget_and_repeats_past_600ms},
        {"shared query budget: selected + active, no periodic current", test_query_budget_selected_and_active_without_current},
        {"enable confirmed only after F3 02", test_enable_confirmed_only_after_f3_02},
        {"late F3 ack after stop does not enable", test_late_f3_ack_after_stop_does_not_enable},
        {"move requires fresh feedback + enable", test_move_requires_fresh_feedback_and_enable},
        {"move completion: CD ack + two distinct pairs", test_move_completion_needs_cd_ack_and_two_distinct_pairs},
        {"repeated poll does not complete move", test_repeated_poll_does_not_complete_move},
        {"loss of velocity faults and stops", test_loss_of_velocity_faults_and_stops},
        {"bus off invalidates enable", test_bus_off_invalidates_enable},
        {"partial move TX failure sends stop", test_partial_move_tx_failure_sends_stop},
        {"stopAll works with active job", test_stop_all_works_with_active_job},
        {"stop keeps the confirmed enable and cancels a pending one", test_stop_keeps_confirmed_enable},
        {"stopAll keeps the confirmed enable", test_stop_all_keeps_confirmed_enable},
        {"trial timeout stop keeps the confirmed enable", test_trial_timeout_stop_keeps_confirmed_enable},
        {"home trigger gating, wire frame and conflict rejection", test_home_trigger_gates_and_wire_frame},
        {"home success needs running bit observed then cleared", test_home_success_needs_running_bit_then_clear},
        {"explicit 9A 9F completion with stationary feedback", test_home_explicit_9f_completion_is_enough},
        {"home idle 3B status alone is never completion", test_home_idle_status_alone_times_out},
        {"home failure bits and rejected ack abort and fault", test_home_failure_bits_and_rejected_ack_fail},
        {"home ack timeout fails and stops", test_home_ack_timeout_fails},
        {"home 12/22 no-motion answer is its own outcome", test_home_no_motion_answer_is_its_own_outcome},
        {"home cancel via stop, disable, 9C and stopAll", test_home_cancel_paths},
        {"3B homing status polled only while homing", test_home_queries_3b_only_while_active},
        {"4C homing parameter bounds and honest refusal reasons", test_home_param_bounds_and_power_on_report},
        {"stop clears a phantom enable desire", test_stop_clears_phantom_enable_desire},
        {"home inferred proof is withdrawn when running returns", test_home_inferred_proof_is_withdrawn_when_running_returns},
        {"home missing 3B status fails the supervised run", test_home_missing_status_fails_run},
        {"direct FB/CB gate layout and documented example bytes", test_direct_position_gate_layout_and_example_bytes},
        {"direct FB/CB wire frame and modes 0/1/2", test_direct_position_wire_frame_and_modes},
        {"direct mode 0 needs a fresh 0x33 driver target, not 0x34", test_direct_position_mode0_needs_fresh_target},
        {"direct dispatch via the API and the CB form in every mode", test_direct_position_dispatch_api_and_cb_modes},
        {"paused page polling retains CB target proof", test_paused_page_polling_keeps_direct_move_feedback},
        {"direct completion proof: 0x33 only, distant target rejected", test_direct_position_target_proof_uses_33_not_34},
        {"direct completion needs matched ack, fresh target and two pairs", test_direct_position_completion_proof},
        {"direct wrong-opcode ack never confirms the job", test_direct_position_wrong_opcode_ack_never_confirms},
        {"direct travel policy, overflow and CB current bounds", test_direct_position_travel_limits_and_overflow},
        {"direct zero-travel no-op, absolute zero and CB current", test_direct_position_zero_travel_noop_and_cb_current},
        {"direct cancel, deadline, TX failure and target invalidation", test_direct_position_cancel_timeout_tx_fail_and_invalidation},
        {"closed-loop current limit 0x45 layout, policy and configure gate", test_closed_loop_current_limit_write},
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
