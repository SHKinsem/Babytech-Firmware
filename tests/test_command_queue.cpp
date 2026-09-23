// Host tests for the board queue (device-controller/src/CommandQueue.cpp) and the readable
// program DSL (device-controller/include/QueueProgram.h).
//
// The queue runs against the real MotorControl over the fake X42sProtocol bus,
// so every step is asserted on what actually reached the wire and on the
// controller's own supervised outcomes. Nothing here needs hardware.
//
// Tiny hand-rolled harness on purpose: no external test framework.

#include "Arduino.h"
#include "CommandQueue.h"
#include "MotorControl.h"
#include "QueueProgram.h"
#include "fake_x42s.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

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

// --- Rig -------------------------------------------------------------------

// Per-ID rotation distances, as main.cpp stores them in NVS.
struct TestRotation : public QueueRotationSource {
    double value[256] = {};
    bool valid[256] = {};

    void set(uint8_t id, double mm) { value[id] = mm; valid[id] = true; }

    bool rotationMm(uint8_t id, double& out) const override {
        if (!valid[id]) return false;
        out = value[id];
        return true;
    }
};

struct QueueRig {
    MotorControl motor;
    CommandQueue queue;
    TestRotation rotation;

    QueueRig() : queue(motor) { fakeReset(); }
    void begin() { motor.begin(4, 5, 500000); }
};

static void tick(QueueRig& rig, uint32_t now) {
    setMillis(now);
    rig.motor.poll();
    rig.queue.poll(now);
}

static std::string status(const QueueRig& rig) { return rig.queue.statusJson().str(); }
static std::string motorStatus(const QueueRig& rig, uint8_t id) {
    return rig.motor.statusJson(id).str();
}

static bool has(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

static Result startQueue(QueueRig& rig, const char* program, long repeat = 1, uint32_t now = 0) {
    setMillis(now);
    return rig.queue.start(program, std::strlen(program), repeat, rig.rotation, now);
}

// Ends the current run without sending anything else, so an unrelated assertion
// can start the next program (a running queue rejects a concurrent start).
static void cancelQueue(QueueRig& rig) {
    capturedTX.clear();
    txLog.clear();
    rig.queue.cancel("test_cancel");
}

static CanRawFrame driverFlags(uint8_t id, bool enabled) {
    const uint8_t data[] = {0x3A, uint8_t(enabled ? 1 : 0), 0x6B};
    return makeFrame(id, data, sizeof(data));
}

// Confirmed enable plus one fresh stationary position/velocity pair.
static void enableAndFeed(QueueRig& rig, uint8_t id, uint32_t atMs) {
    setMillis(atMs);
    rig.motor.enable(id, true);
    injectRx(makeAck(id, 0xF3, 0x02));
    injectRx(driverFlags(id, true));
    setMillis(atMs + 20);
    injectRx(makePosition(id, 0));
    injectRx(makeVelocity(id, 0));
    rig.motor.poll();
}

static void feedStationary(QueueRig& rig, uint8_t id, uint32_t atMs, int32_t tenths = 0) {
    setMillis(atMs);
    injectRx(makePosition(id, tenths));
    injectRx(makeVelocity(id, 0));
    rig.motor.poll();
}

// One 0x3B homing status reply (manual V1.0.5 p62-63).
static CanRawFrame makeHomeStatus(uint8_t addr, uint8_t flags) {
    const uint8_t data[3] = {0x3B, flags, 0x6B};
    return makeFrame(addr, data, sizeof(data));
}
constexpr uint8_t kHomeOrgRunning = 0x04;        // bit2 Org_SF
constexpr uint8_t kHomeOrgIdleNoFailure = 0x03;  // encoder + calibration ready only

// Last transmitted move, or false when none was sent.
static bool logicalPayload(uint8_t opcode, uint8_t* out, size_t& length);
static bool lastMoveRecord(TxRecord& out) {
    uint8_t p[30]={}; size_t n=0;
    if(!logicalPayload(0xCD,p,n) || n!=17) return false;
    out.dir=p[1]; out.motionMode=p[12];
    out.magnitude=(uint32_t(p[8])<<24)|(uint32_t(p[9])<<16)|(uint32_t(p[10])<<8)|p[11];
    return true;
}

// Reassembles one transmitted logical command. The address lives in the CAN
// identifier and every packet repeats the function code, so the result is that
// function code ONCE followed by the payload bytes of every packet: exactly the
// logical command without its address byte.
static bool logicalPayload(uint8_t opcode, uint8_t* out, size_t& length) {
    length = 0;
    bool found = false;
    for (size_t i = 0; i < capturedTX.size(); ++i) {
        const CanRawFrame& f = capturedTX[i];
        if (f.length == 0 || f.data[0] != opcode) continue;
        if(found && (f.identifier & 255)==0) break;
        if (!found) {
            if (length >= 30) return false;
            out[length++] = opcode;  // the function code, kept once
            found = true;
        }
        for (uint8_t j = 1; j < f.length && length < 30; ++j) out[length++] = f.data[j];
    }
    return found;
}

static uint32_t countTxOpcode(uint8_t opcode) {
    uint32_t n = 0;
    for (size_t i = 0; i < capturedTX.size(); ++i) {
        if (capturedTX[i].length > 0 && capturedTX[i].data[0] == opcode) ++n;
    }
    return n;
}

static bool sawTxId(uint32_t id, const uint8_t* data, uint8_t length) {
    for (size_t i = 0; i < capturedTX.size(); ++i) {
        const CanRawFrame& f = capturedTX[i];
        if (f.identifier != id || f.length != length) continue;
        if (std::memcmp(f.data, data, length) == 0) return true;
    }
    return false;
}

// --- Program validation -----------------------------------------------------

static void test_program_validation_is_atomic_with_source_lines() {
    QueueRig rig;
    rig.begin();

    // A valid first line and an invalid second one: nothing may be transmitted.
    const Result bad = startQueue(rig, "enable 1\nmove 1 abc\n");
    CHECK(bad.code == 400);
    CHECK(std::string(bad.message) == "invalid_number");
    CHECK(rig.queue.lastErrorLine() == 2);
    CHECK(capturedTX.empty());
    CHECK(!rig.queue.active());

    CHECK(startQueue(rig, "enable 1\njump 2\n").code == 400);
    CHECK(rig.queue.lastErrorLine() == 2);
    CHECK(startQueue(rig, "# only a comment\n").code == 400);
    CHECK(std::string(startQueue(rig, "").message) == "empty_program");
    CHECK(std::string(startQueue(rig, "wobble 1").message) == "unknown_action");

    // A comment after a valid action is ignored. This runs on its own rig so the
    // runId of the rig below stays 1 (a refused start does not consume one, but
    // an accepted one does).
    QueueRig commentRig;
    commentRig.begin();
    CHECK(startQueue(commentRig, "wait 0 # trailing note\n").code == 202);

    // 64 non-blank actions are the maximum.
    std::string many;
    for (int i = 0; i < 65; ++i) many += "wait 0\n";
    const Result tooMany = startQueue(rig, many.c_str());
    CHECK(tooMany.code == 400);
    CHECK(std::string(tooMany.message) == "too_many_actions");
    CHECK(rig.queue.lastErrorLine() == 65);

    // The whole text is bounded.
    std::string huge(8193, '#');
    CHECK(startQueue(rig, huge.c_str()).code == 400);
    CHECK(rig.queue.lastErrorLine() == 0);

    // A valid program of the maximum step count is accepted.
    std::string full;
    for (int i = 0; i < 64; ++i) full += "wait 0\n";
    const Result ok = startQueue(rig, full.c_str());
    CHECK(ok.code == 202);
    CHECK(has(status(rig), "\"total\":64"));
    CHECK(has(status(rig), "\"state\":\"running\""));
    CHECK(has(status(rig), "\"runId\":1"));
}

static void test_strict_numeric_and_raw_bounds() {
    QueueRig rig;
    rig.begin();
    const char* invalid[] = {
        "move 1 10deg",      // the unit must be its own token
        "move 1 10.5.5",
        "move 1 1e3",
        "move 1 --5",
        "move 1 5 5x",
        "wait 1.5",
        "wait 3600001",
        "wait -1",
        "enable 0",
        "enable 256",
        "enable 1 2",
        "home 1 6",
        "home 1 -1",
        "hex 01",
        "hex 0102",
        "can std 800 DE",
        "can ext 20000000 DE",
        "can qq 12 DE",
        "can std 1 11 22 33 44 55 66 77 88 99",

        "velocity 1 0 100",
        "velocity 1 30 0",

        "torque 1 6000 100",
    };
    for (const char* program : invalid) {
        CHECK(startQueue(rig, program).code == 400);
        CHECK(!rig.queue.active());
    }
    CHECK(std::string(startQueue(rig, "wait 1.5").message) == "invalid_integer");
    CHECK(std::string(startQueue(rig, "can std 800 DE").message) == "can_id_out_of_range");
    CHECK(std::string(startQueue(rig, "can std 1 11 22 33 44 55 66 77 88 99").message) ==
          "can_length_out_of_range");
    // "0102" is one token holding two bytes: that is a malformed byte token, not
    // a byte-count problem. Two valid byte tokens are what underruns the length.
    CHECK(std::string(startQueue(rig, "hex 0102").message) == "hex_digit_invalid");
    CHECK(std::string(startQueue(rig, "hex 01 02").message) == "hex_length_out_of_range");

    CHECK(std::string(startQueue(rig, "move 1 10deg").message) == "invalid_number");

    // Valid raw frames: the broadcast trigger the protected whitelist rejects is
    // legal here, and a standard id is an actual CAN id, not a motor address.
    CHECK(startQueue(rig, "hex 00 FF 66 6B\ncan std 123 DE AD\n").code == 202);
    CHECK(has(status(rig), "\"raw\":true"));
}

static void test_rotation_distance_and_direction_conversion() {
    // 10 mm on an 8 mm/rev screw is 450 degrees, i.e. 4500 tenths.
    QueueRig rig;
    rig.begin();
    rig.rotation.set(1, 8.0);
    enableAndFeed(rig, 1, 0);
    CHECK(startQueue(rig, "move 1 10 mm\n", 1, 40).code == 202);
    // One run at a time.
    CHECK(startQueue(rig, "move 1 20 mm\n", 1, 40).code == 409);
    CHECK(std::string(startQueue(rig, "move 1 20 mm\n", 1, 40).message) == "queue_busy");
    tick(rig, 60);                        // the step selects its target first
    feedStationary(rig, 1, 80, 0);        // ...and needs a fresh pair to dispatch
    tick(rig, 100);
    TxRecord record;
    CHECK(lastMoveRecord(record));
    CHECK(record.magnitude == 4500);
    CHECK(record.dir == 0);
    CHECK(record.motionMode == 2);

    // A negative distance changes the direction, never the unsigned field.
    QueueRig negative;
    negative.begin();
    negative.rotation.set(1, 8.0);
    enableAndFeed(negative, 1, 0);
    CHECK(startQueue(negative, "move 1 -10 mm\n", 1, 40).code == 202);
    tick(negative, 60);
    feedStationary(negative, 1, 80, 0);
    tick(negative, 100);
    CHECK(lastMoveRecord(record));
    CHECK(record.magnitude == 4500);
    CHECK(record.dir == 1);

    // rev is always 360 degrees, whatever the screw does.
    QueueRig revolutions;
    revolutions.begin();
    revolutions.rotation.set(1, 40.0);
    enableAndFeed(revolutions, 1, 0);
    CHECK(startQueue(revolutions, "move 1 1 rev\n", 1, 40).code == 202);
    tick(revolutions, 60);
    feedStationary(revolutions, 1, 80, 0);
    tick(revolutions, 100);
    CHECK(lastMoveRecord(record));
    CHECK(record.magnitude == 3600);

    // An id without a stored distance is rejected, never guessed.
    QueueRig missing;
    missing.begin();
    enableAndFeed(missing, 1, 0);
    const Result noDistance = startQueue(missing, "move 3 10 mm\n", 1, 40);
    CHECK(noDistance.code == 400);
    CHECK(std::string(noDistance.message) == "rotation_distance_missing");
}

// The queue is a sender: the board's editable debug policy (DebugLimits) must
// never refuse a program that the documented wire fields can carry.
static void test_saved_board_policy_does_not_gate_the_queue() {
    QueueRig rig;
    rig.begin();
    DebugLimits tight;
    tight.maxSpeedTenths = 300;         // 30.0 RPM
    tight.maxAccelRpmS = 1;
    tight.maxCurrentMa = 100;
    tight.maxAngleTenths = 100;         // 10 deg
    tight.maxMoveDurationMs = 1000;
    tight.experimentDurationMs = 1000;
    CHECK(rig.motor.setDebugLimits(tight));

    CHECK(startQueue(rig, "move 1 10 deg 300\n").code == 202);          // 300 RPM: allowed
    cancelQueue(rig);
    CHECK(startQueue(rig, "move 1 4000 deg\n").code == 202);            // 4000 deg: allowed
    cancelQueue(rig);
    CHECK(startQueue(rig, "move 1 10 deg 30 500 600 50\n").code == 202);  // accel/current beyond policy
    cancelQueue(rig);
    CHECK(startQueue(rig, "velocity 1 300 100\n").code == 202);
    cancelQueue(rig);
    CHECK(startQueue(rig, "torque 1 800 100 300\n").code == 202);
    cancelQueue(rig);
    CHECK(startQueue(rig, "velocity 1 30 6000\n").code == 202);         // long trial: user timing
    cancelQueue(rig);

    // The documented protocol field widths are still enforced.
    CHECK(startQueue(rig, "move 1 10 deg 3001\n").code == 400);         // > 3000.0 RPM
    CHECK(startQueue(rig, "move 1 10 deg 30 60 60 5001\n").code == 400);  // > 5000 mA
    CHECK(startQueue(rig, "torque 1 6000 100\n").code == 400);
    CHECK(startQueue(rig, "velocity 1 3001 100\n").code == 400);
}

// --- Structured step execution ---------------------------------------------

static void test_structured_program_progresses_on_real_evidence() {
    QueueRig rig;
    rig.begin();
    enableAndFeed(rig, 1, 0);
    CHECK(startQueue(rig, "enable 1\nmove 1 10 deg\nstop 1\n", 1, 40).code == 202);

    // Step 1: select the target, then wait for fresh feedback before dispatching.
    tick(rig, 60);
    CHECK(has(status(rig), "\"step\":1"));
    enableAndFeed(rig, 1, 80);
    tick(rig, 100);  // dispatch the enable (the node is already confirmed)
    tick(rig, 120);  // confirmed enable + stationary sample advance the step
    CHECK(has(status(rig), "\"step\":2"));
    CHECK(has(status(rig), "\"action\":\"move\""));

    // Step 2: dispatch the move, then require its own supervised outcome. The
    // move is relative to the position at dispatch (0), so the target is 100
    // tenths and the samples below are near it.
    feedStationary(rig, 1, 140, 96);
    tick(rig, 140);
    TxRecord record;
    CHECK(lastMoveRecord(record));
    CHECK(record.magnitude == 100);
    injectRx(makeAck(1, 0xCD, 0x02));
    tick(rig, 160);
    CHECK(has(status(rig), "\"step\":2"));
    feedStationary(rig, 1, 180, 196);
    tick(rig, 200);
    CHECK(has(status(rig), "\"step\":2"));  // one distinct pair is not completion
    feedStationary(rig, 1, 220, 200);
    tick(rig, 240);
    CHECK(has(status(rig), "\"step\":3"));
    CHECK(has(status(rig), "\"action\":\"stop\""));

    // Step 3: the stop waits for a fresh stationary confirmation.
    tick(rig, 260);  // dispatch the stop
    CHECK(has(status(rig), "\"step\":3"));
    feedStationary(rig, 1, 280, 200);
    tick(rig, 300);  // advances past the last step
    tick(rig, 320);  // the run finishes on the next poll
    CHECK(has(status(rig), "\"state\":\"done\""));
    CHECK(has(status(rig), "\"step\":3"));
    CHECK(has(status(rig), "\"message\":\"done\""));
    // A stop keeps the confirmed enable.
    CHECK(has(motorStatus(rig, 1), "\"enabled\":true"));
}

static void test_new_move_never_consumes_the_previous_outcome() {
    QueueRig rig;
    rig.begin();
    enableAndFeed(rig, 1, 0);
    CHECK(startQueue(rig, "move 1 10 deg\nmove 1 20 deg\n", 1, 40).code == 202);

    // First move: dispatched relative to 100, so its target is 200 tenths.
    tick(rig, 60);
    feedStationary(rig, 1, 80, 100);
    tick(rig, 100);
    injectRx(makeAck(1, 0xCD, 0x02));
    feedStationary(rig, 1, 120, 200);
    tick(rig, 140);
    CHECK(has(status(rig), "\"step\":1"));
    feedStationary(rig, 1, 160, 200);
    tick(rig, 180);
    CHECK(has(status(rig), "\"step\":2"));

    // The second move acquires its own fresh stationary pair before it is
    // dispatched, and the previous Done must not complete it: only a NEW
    // supervised outcome counts. This move is relative to 400 and travels 200
    // tenths, so its target is 600.
    tick(rig, 200);                       // select the target
    CHECK(has(status(rig), "\"step\":2"));
    feedStationary(rig, 1, 220, 400);
    tick(rig, 240);                       // dispatch the second move
    CHECK(has(status(rig), "\"step\":2"));
    CHECK(rig.queue.active());            // the stale Done did not complete it
    injectRx(makeAck(1, 0xCD, 0x02));
    feedStationary(rig, 1, 260, 600);
    tick(rig, 280);
    CHECK(has(status(rig), "\"step\":2"));
    feedStationary(rig, 1, 300, 600);
    tick(rig, 320);
    tick(rig, 340);
    CHECK(has(status(rig), "\"state\":\"done\""));
}

static void test_enable_step_waits_ack_and_stationary() {
    QueueRig rig;
    rig.begin();
    CHECK(startQueue(rig, "enable 1\nwait 0\n").code == 202);

    // The first dispatch needs fresh feedback before the action is sent.
    tick(rig, 0);
    CHECK(has(status(rig), "\"step\":1"));
    feedStationary(rig, 1, 20);
    tick(rig, 40);
    CHECK(countTxTo(1, TxKind::Enable) == 1);

    // An acknowledgement alone is not enough: the node must also look stationary.
    injectRx(makeAck(1, 0xF3, 0x02));
    injectRx(driverFlags(1, true));
    setMillis(60);
    injectRx(makeVelocity(1, 100));
    rig.motor.poll();
    rig.queue.poll(60);
    CHECK(has(status(rig), "\"step\":1"));
    CHECK(rig.queue.active());

    feedStationary(rig, 1, 80);
    tick(rig, 100);   // advances to the wait step
    for (uint32_t t = 120; t <= 200; t += 20) tick(rig, t);
    CHECK(has(status(rig), "\"state\":\"done\""));

    // No acknowledgement at all: the controller's own ack timeout fails the step.
    QueueRig silent;
    silent.begin();
    CHECK(startQueue(silent, "enable 1\n").code == 202);
    feedStationary(silent, 1, 20);
    tick(silent, 40);
    for (uint32_t t = 200; t <= 1700; t += 200) {
        feedStationary(silent, 1, t);
        tick(silent, t + 20);
    }
    CHECK(has(status(silent), "\"state\":\"failed\""));
    CHECK(has(status(silent), "enable_ack_timeout"));
    CHECK(countTxTo(0, TxKind::Stop) == 1);  // the failure stops everything
}

static void test_home_step_outcomes() {
    // Inferred completion: running observed, then cleared, then two stationary
    // samples that are strictly newer than the clear.
    QueueRig rig;
    rig.begin();
    enableAndFeed(rig, 1, 0);
    CHECK(startQueue(rig, "home 1 2\n", 1, 40).code == 202);
    tick(rig, 60);
    CHECK(has(status(rig), "\"action\":\"home\""));
    feedStationary(rig, 1, 80);
    tick(rig, 100);  // dispatch the supervised trigger
    CHECK(has(status(rig), "\"step\":1"));

    injectRx(makeAck(1, 0x9A, 0x02));
    tick(rig, 120);
    CHECK(rig.queue.active());
    setMillis(140);
    injectRx(makeHomeStatus(1, kHomeOrgRunning));
    tick(rig, 140);
    setMillis(160);
    injectRx(makeHomeStatus(1, kHomeOrgIdleNoFailure));
    feedStationary(rig, 1, 160, 0);
    tick(rig, 180);
    CHECK(rig.queue.active());  // the proof alone completes nothing
    feedStationary(rig, 1, 200);
    tick(rig, 220);
    CHECK(rig.queue.active());
    feedStationary(rig, 1, 240);
    tick(rig, 260);
    tick(rig, 280);
    CHECK(has(status(rig), "\"state\":\"done\""));

    // A lone "not homing" byte never advances the step; the controller fails the
    // run and the queue fails with it instead of silently moving on.
    QueueRig idle;
    idle.begin();
    DebugLimits limits;
    limits.maxMoveDurationMs = 1000;
    CHECK(idle.motor.setDebugLimits(limits));
    enableAndFeed(idle, 1, 0);
    CHECK(startQueue(idle, "home 1 0\n", 1, 40).code == 202);
    tick(idle, 60);
    feedStationary(idle, 1, 80);
    tick(idle, 100);
    injectRx(makeAck(1, 0x9A, 0x02));
    tick(idle, 120);
    for (uint32_t t = 140; t <= 900; t += 100) {
        setMillis(t);
        injectRx(makeHomeStatus(1, kHomeOrgIdleNoFailure));
        injectRx(makePosition(1, 0));
        injectRx(makeVelocity(1, 0));
        idle.motor.poll();
        idle.queue.poll(t);
        CHECK(idle.queue.active());
    }
    for (uint32_t t = 1000; t <= 1400; t += 200) {
        feedStationary(idle, 1, t);
        tick(idle, t + 20);
    }
    // The controller failed the supervised run and the queue reports its verdict
    // instead of quietly moving on.
    CHECK(has(status(idle), "\"state\":\"failed\""));
    CHECK(has(status(idle), "home_timeout"));

    // Manual 12/22: reported distinctly, the queue continues, no homing claimed.
    // The answer is only honoured with a stationary pair that arrived AFTER it.
    QueueRig origin;
    origin.begin();
    enableAndFeed(origin, 1, 0);
    CHECK(startQueue(origin, "home 1 0\nwait 0\n", 1, 40).code == 202);
    tick(origin, 60);            // select the target
    feedStationary(origin, 1, 80);
    tick(origin, 100);           // dispatch the trigger
    injectRx(makeAck(1, 0x9A, 0x12));
    tick(origin, 120);           // the 12 answer is consumed
    tick(origin, 140);
    CHECK(origin.queue.active());                    // nothing advanced yet
    CHECK(!has(status(origin), "home_no_motion"));    // and it is not reported
    tick(origin, 160);           // the pre-answer pair is still too old
    CHECK(origin.queue.active());
    CHECK(!has(status(origin), "home_no_motion"));
    feedStationary(origin, 1, 180);
    tick(origin, 200);           // post-answer stationary pair -> continue
    CHECK(has(status(origin), "home_no_motion"));
    CHECK(origin.queue.active());
    for (uint32_t t = 220; t <= 300; t += 20) tick(origin, t);
    CHECK(has(status(origin), "\"state\":\"done\""));
}

static void test_timed_steps_send_once_then_stop_and_wait() {
    // velocity 3 -30 RPM 200 ms 60 800 is the documented logical command; the
    // two CAN packets match the protocol review exactly.
    QueueRig velocity;
    velocity.begin();
    enableAndFeed(velocity, 3, 0);
    CHECK(startQueue(velocity, "velocity 3 -30 200 60 800\n", 1, 40).code == 202);
    tick(velocity, 60);                      // select the target
    feedStationary(velocity, 3, 80);
    tick(velocity, 100);                     // dispatch the C6 frame once

    uint8_t payload[30] = {};
    size_t length = 0;
    CHECK(logicalPayload(0xC6, payload, length));
    const uint8_t expectedVelocity[10] = {0xC6, 1, 0, 60, 1, 0x2C, 0, 3, 0x20, 0x6B};
    CHECK(length == sizeof(expectedVelocity));
    CHECK(std::memcmp(payload, expectedVelocity, sizeof(expectedVelocity)) == 0);
    const uint8_t firstPacket[8] = {0xC6, 1, 0, 60, 1, 0x2C, 0, 3};
    CHECK(sawTxId(0x0300, firstPacket, sizeof(firstPacket)));
    const uint8_t secondPacket[3] = {0xC6, 0x20, 0x6B};
    CHECK(sawTxId(0x0301, secondPacket, sizeof(secondPacket)));

    // The queue times the run, stops it and waits for a fresh stationary sample.
    for (uint32_t t = 120; t < 300; t += 40) {
        feedStationary(velocity, 3, t, 100);
        tick(velocity, t + 10);
        CHECK(velocity.queue.active());
    }
    tick(velocity, 320);                     // the 200 ms timer elapsed
    CHECK(sawStopFor(3));
    CHECK(velocity.queue.active());          // stop sent, confirmation still missing
    feedStationary(velocity, 3, 340, 100);
    tick(velocity, 360);
    tick(velocity, 380);
    CHECK(has(status(velocity), "\"state\":\"done\""));

    // torque 3 -800 mA 200 ms, max 30 RPM, ramp 1000 mA/s.
    QueueRig torque;
    torque.begin();
    enableAndFeed(torque, 3, 0);
    CHECK(startQueue(torque, "torque 3 -800 200 30 1000\n", 1, 40).code == 202);
    tick(torque, 60);
    feedStationary(torque, 3, 80);
    tick(torque, 100);
    length = 0;
    CHECK(logicalPayload(0xC5, payload, length));
    const uint8_t expectedTorque[10] = {0xC5, 1, 3, 0xE8, 3, 0x20, 0, 1, 0x2C, 0x6B};
    CHECK(length == sizeof(expectedTorque));
    CHECK(std::memcmp(payload, expectedTorque, sizeof(expectedTorque)) == 0);
    const uint8_t torqueFirst[8] = {0xC5, 1, 3, 0xE8, 3, 0x20, 0, 1};
    CHECK(sawTxId(0x0300, torqueFirst, sizeof(torqueFirst)));
    const uint8_t torqueSecond[3] = {0xC5, 0x2C, 0x6B};
    CHECK(sawTxId(0x0301, torqueSecond, sizeof(torqueSecond)));
}

static void test_raw_steps_report_sent_only() {
    QueueRig rig;
    rig.begin();
    enableAndFeed(rig, 1, 0);
    CHECK(has(motorStatus(rig, 1), "\"enabled\":true"));

    CHECK(startQueue(rig, "hex 00 FF 66 6B\ncan std 123 DE AD\n", 1, 40).code == 202);
    tick(rig, 60);
    CHECK(rig.queue.active());
    tick(rig, 80);
    tick(rig, 100);  // both raw steps are done, the run finishes

    // The raw broadcast trigger went out byte for byte on id 0x00000000.
    const uint8_t broadcast[3] = {0xFF, 0x66, 0x6B};
    CHECK(sawTxId(0x00000000u, broadcast, sizeof(broadcast)));
    // The std frame is actual CAN id 0x123 with exactly two bytes.
    const uint8_t canData[2] = {0xDE, 0xAD};
    CHECK(sawTxId(0x123u, canData, sizeof(canData)));
    bool standardFrame = false;
    for (size_t i = 0; i < capturedTX.size(); ++i) {
        if (capturedTX[i].identifier == 0x123u) standardFrame = !capturedTX[i].extended;
    }
    CHECK(standardFrame);

    // Both steps report sent only: no fabricated completion and no implicit stop.
    CHECK(has(status(rig), "\"state\":\"done\""));
    CHECK(!sawStopFor(1));
    CHECK(countTxTo(0, TxKind::Stop) == 0);
    // The raw frame invalidated the software enable, so a later structured step
    // needs a fresh explicit enable.
    CHECK(has(motorStatus(rig, 1), "\"enabled\":false"));

    // A raw transmission failure fails the run immediately and stops everything.
    QueueRig failing;
    failing.begin();
    CHECK(startQueue(failing, "hex 00 FF 66 6B\n").code == 202);
    failNextMoveTx = true;
    tick(failing, 20);
    CHECK(has(status(failing), "\"state\":\"failed\""));
    CHECK(has(status(failing), "raw_tx_failed"));
    CHECK(countTxTo(0, TxKind::Stop) == 1);
}

static void test_repeat_bounds_and_cancel() {
    QueueRig rig;
    rig.begin();
    CHECK(startQueue(rig, "wait 0\n", 2).code == 202);
    for (uint32_t t = 0; t <= 200; t += 20) tick(rig, t);
    CHECK(has(status(rig), "\"state\":\"done\""));
    CHECK(has(status(rig), "\"iteration\":2"));
    CHECK(has(status(rig), "\"repeat\":2"));
    CHECK(has(status(rig), "\"total\":1"));
    CHECK(has(status(rig), "\"step\":1"));

    const Result badRepeat = startQueue(rig, "wait 0\n", 1001);
    CHECK(badRepeat.code == 400);
    CHECK(std::string(badRepeat.message) == "repeat_out_of_range");
    CHECK(startQueue(rig, "wait 0\n", 0).code == 400);

    // Cancel stops the run, prevents the next step and broadcasts 9C + FE.
    QueueRig cancelled;
    cancelled.begin();
    enableAndFeed(cancelled, 1, 0);
    CHECK(startQueue(cancelled, "wait 1000\nwait 0\n", 1, 40).code == 202);
    tick(cancelled, 60);
    CHECK(cancelled.queue.active());
    CHECK(cancelled.queue.cancel("test_cancel").code == 202);
    CHECK(!cancelled.queue.active());
    CHECK(has(status(cancelled), "\"state\":\"cancelled\""));
    CHECK(countTxOpcode(0x9C) == 1);
    CHECK(countTxTo(0, TxKind::Stop) == 1);
    // A stop keeps the confirmed enable of an untouched node.
    CHECK(has(motorStatus(cancelled, 1), "\"enabled\":true"));
    for (uint32_t t = 200; t <= 2000; t += 200) tick(cancelled, t);
    CHECK(has(status(cancelled), "\"state\":\"cancelled\""));
    CHECK(has(status(cancelled), "\"step\":1"));  // the second step never started
    CHECK(countTxOpcode(0x9C) == 1);              // and nothing was sent again
}

static void test_fail_fast_and_bus_loss() {
    // A supervised step that faults fails the run and stops everything.
    QueueRig faulted;
    faulted.begin();
    enableAndFeed(faulted, 1, 0);
    CHECK(startQueue(faulted, "move 1 10 deg\nwait 0\n", 1, 40).code == 202);
    tick(faulted, 60);                     // select the target
    feedStationary(faulted, 1, 80, 0);
    tick(faulted, 100);                    // dispatch the move
    injectRx(makeAck(1, 0xCD, 0xE2));      // the controller rejects it
    tick(faulted, 120);
    // The controller's own fault tag is reported, not a generic label.
    CHECK(has(status(faulted), "\"state\":\"failed\""));
    CHECK(has(status(faulted), "ack_rejected"));
    CHECK(has(motorStatus(faulted, 1), "\"fault\":\"ack_rejected\""));
    CHECK(countTxTo(0, TxKind::Stop) == 1);

    // A target that never produces feedback is never dispatched blindly.
    QueueRig waiting;
    waiting.begin();
    CHECK(startQueue(waiting, "move 5 10 deg\n").code == 202);
    tick(waiting, 0);
    for (uint32_t t = 200; t <= 3200; t += 400) tick(waiting, t);
    CHECK(has(status(waiting), "\"state\":\"failed\""));
    CHECK(has(status(waiting), "target_feedback_timeout"));
    TxRecord none;
    CHECK(!lastMoveRecord(none));

    // A bus loss ends the run instead of riding it out.
    QueueRig lost;
    lost.begin();
    enableAndFeed(lost, 1, 0);
    CHECK(startQueue(lost, "wait 5000\n", 1, 40).code == 202);
    tick(lost, 60);
    CHECK(lost.queue.active());
    busState = CanControllerState::BusOff;
    tick(lost, 100);
    CHECK(has(status(lost), "\"state\":\"failed\""));
    CHECK(has(status(lost), "can_unavailable"));

    // A supervised step that times out without evidence fails the same way.
    QueueRig overdue;
    overdue.begin();
    DebugLimits limits;
    limits.maxMoveDurationMs = 1000;
    CHECK(overdue.motor.setDebugLimits(limits));
    enableAndFeed(overdue, 1, 0);
    CHECK(startQueue(overdue, "move 1 10 deg\n", 1, 40).code == 202);
    tick(overdue, 60);                     // select the target
    feedStationary(overdue, 1, 80, 0);
    tick(overdue, 100);                    // dispatch the move (target = 100)
    injectRx(makeAck(1, 0xCD, 0x02));
    for (uint32_t t = 120; t <= 4000; t += 200) {
        feedStationary(overdue, 1, t, 50);  // fresh, but never near the target
        tick(overdue, t + 20);
    }
    CHECK(has(status(overdue), "\"state\":\"failed\""));
    CHECK(has(status(overdue), "move_timeout"));  // the controller's own verdict
}

// --- Review regressions ----------------------------------------------------

static void test_hostile_tokens_are_rejected_without_sending() {
    // Pure parse fixtures: these checks must not depend on, or disturb, any
    // queue run state (an accepted start would make later cases answer 409).
    QueueRig rig;
    rig.begin();
    const auto parse = [&rig](const char* text, size_t length) {
        QueueProgram program;
        QueueError error;
        if (parseQueueProgram(text, length, rig.rotation, program, error)) {
            return Result{202, "parsed"};
        }
        return Result{400, error.message};
    };
    const auto parseText = [&parse](const char* text) {
        return parse(text, std::strlen(text));
    };

    // A token longer than a 255-byte length field must be rejected, not silently
    // truncated into something that happens to parse.
    std::string longToken(257, '1');
    std::string longProgram = "move 1 " + longToken + "\n";
    CHECK(parse(longProgram.c_str(), longProgram.size()).code == 400);

    std::string longTail(300, 'A');
    std::string longHex = "hex 01 02 " + longTail + "\n";
    CHECK(parse(longHex.c_str(), longHex.size()).code == 400);

    // A can id with more than eight hex digits can never wrap into a valid one.
    CHECK(parseText("can ext 100000000 DE").code == 400);
    CHECK(std::string(parseText("can ext 100000000 DE").message) == "can_id_invalid");
    CHECK(parseText("can ext 123456789 DE").code == 400);
    CHECK(parseText("can std 12345678 DE").code == 400);
    CHECK(parseText("can ext 22345678 DE").code == 400);   // 8 digits, above 29 bits
    CHECK(std::string(parseText("can ext 22345678 DE").message) == "can_id_out_of_range");
    // ...while a genuine 29-bit id is fine.
    CHECK(parseText("can ext 12345678 DE").code == 202);
    CHECK(parseText("can ext 7FF DE").code == 202);

    // The signed grammar is exactly "-" optional: no "+", no exponent.
    CHECK(parseText("move 1 +10").code == 400);
    CHECK(parseText("velocity 1 +30 100").code == 400);
    CHECK(parseText("torque 1 +800 100").code == 400);
    // An id, a duration and a count are plain integers: "1.0" is not one.
    CHECK(parseText("enable 1.0").code == 400);
    CHECK(std::string(parseText("enable 1.0").message) == "invalid_integer");
    CHECK(parseText("home 1.0").code == 400);
    CHECK(parseText("wait 100.0").code == 400);
    CHECK(parseText("velocity 1 30 100.0").code == 400);

    // An embedded NUL is a byte like any other: it invalidates its token instead
    // of silently splitting the line.
    const char withNul[] = {'w', 'a', 'i', 't', ' ', '1', '\0', '0', '\n'};
    CHECK(parse(withNul, sizeof(withNul)).code == 400);
    const char nulLine[] = {'w', 'a', 'i', 't', ' ', '1', '\n', '\0', '\n'};
    CHECK(parse(nulLine, sizeof(nulLine)).code == 400);

    // The long token forms the DSL documents are accepted, and a value that
    // would wrap on the wire is rejected before any narrowing.
    CHECK(parseText("move 1 90 deg 30 60 60 800").code == 202);
    CHECK(parseText("velocity 3 -30 200 60 800").code == 202);
    CHECK(parseText("torque 3 -800 200 30 1000").code == 202);
    CHECK(parseText("torque 3 800 100").code == 202);          // legacy timed form
    CHECK(parseText("torque 3 800").code == 202);              // short form: send only
    CHECK(parseText("velocity 3 -30").code == 202);            // short form: send only
    CHECK(parseText("move 1 10 deg 30 60 60 66336").code == 400);
    CHECK(std::string(parseText("move 1 10 deg 30 60 60 66336").message) ==
          "move_current_out_of_range");
    CHECK(parseText("torque 3 800 100 30 0").code == 202);      // documented ramp 0
    CHECK(parseText("torque 3 800 100 30 65536").code == 400);
    CHECK(parseText("velocity 3 0.05 100").code == 400);        // rounds to zero RPM
    CHECK(parseText("torque 3 800 100 -1 1000").code == 400);

    // Nothing above reached the bus: the only accepted programs were parsed, not
    // started.
    CHECK(capturedTX.empty());

    // The C5 speed limit keeps the 0.1 RPM resolution the UI offers.
    CHECK(startQueue(rig, "torque 1 800 100 30.5 1000").code == 202);
}

static void test_short_forms_and_protocol_bounds() {
    QueueRig rig;
    rig.begin();
    const auto parse = [&rig](const char* text) {
        QueueProgram program;
        QueueError error;
        if (parseQueueProgram(text, std::strlen(text), rig.rotation, program, error)) {
            return Result{202, "parsed"};
        }
        return Result{400, error.message};
    };
    // Short forms: send only, no duration, no timer, no implicit stop.
    CHECK(parse("torque 1 300").code == 202);
    CHECK(parse("torque 1 -300").code == 202);
    CHECK(parse("velocity 1 60").code == 202);
    CHECK(parse("velocity 1 -60").code == 202);
    // The legacy timed forms still work, and the duration is the user's.
    CHECK(parse("torque 1 300 1500").code == 202);
    CHECK(parse("velocity 1 60 2000").code == 202);
    CHECK(parse("velocity 1 60 2000 60 800").code == 202);

    // Documented wire ranges, and no policy: an omitted MAX_RPM uses the default
    // even when the board policy asks for less. 0 is legal on the wire.
    CHECK(parse("torque 1 800 100").code == 202);
    CHECK(parse("torque 1 0 100").code == 202);                  // 0 mA is a wire value
    CHECK(parse("torque 1 800 100 0 0").code == 202);            // no speed limit, no ramp
    CHECK(parse("velocity 1 30 100 0 0").code == 202);           // accel 0, current 0
    CHECK(parse("move 1 10 deg 30 0 0 0").code == 202);          // 0 accel / 0 current
    // Above the documented protocol range is still refused.
    CHECK(parse("torque 1 5001").code == 400);
    CHECK(parse("torque 1 800 100 3001 1000").code == 400);
    CHECK(parse("velocity 1 3001 100").code == 400);
    CHECK(parse("move 1 10 deg 30 60 60 5001").code == 400);
    CHECK(parse("move 1 10 deg 30 65536").code == 400);
    // The only duration bound left is the DSL resource bound.
    CHECK(parse("torque 1 800 0").code == 400);
    CHECK(parse("torque 1 800 3600001").code == 400);
}

static void test_enable_needs_post_request_evidence() {
    QueueRig rig;
    rig.begin();
    // A node that was confirmed and fed BEFORE the program starts.
    enableAndFeed(rig, 1, 0);
    CHECK(startQueue(rig, "enable 1\nwait 0\n", 1, 40).code == 202);

    // The enable is dispatched promptly (no target wait) and the old pair must
    // not complete it.
    tick(rig, 60);
    CHECK(countTxTo(1, TxKind::Enable) >= 1);
    CHECK(has(status(rig), "\"step\":1"));
    tick(rig, 70);                       // the pre-request pair is still too old
    CHECK(has(status(rig), "\"step\":1"));
    CHECK(rig.queue.active());

    // Old F3 evidence cannot finish a new explicit enable: it sends again.
    CHECK(countTxTo(1, TxKind::Enable) == 2);
    injectRx(makeAck(1, 0xF3, 0x02));
    injectRx(driverFlags(1, true));
    // A fresh pair after the request advances it.
    feedStationary(rig, 1, 80);
    tick(rig, 100);
    for (uint32_t t = 120; t <= 200; t += 20) tick(rig, t);
    CHECK(has(status(rig), "\"state\":\"done\""));
}

static void test_stop_and_disable_dispatch_promptly_on_a_quiet_node() {
    // A stop must go out even when the target never produced feedback.
    QueueRig stopRig;
    stopRig.begin();
    CHECK(startQueue(stopRig, "stop 7\n").code == 202);
    tick(stopRig, 10);
    CHECK(sawStopFor(7));
    CHECK(has(status(stopRig), "\"step\":1"));  // now waiting for the confirmation
    for (uint32_t t = 100; t <= 6000; t += 500) tick(stopRig, t);
    CHECK(has(status(stopRig), "\"state\":\"failed\""));
    CHECK(has(status(stopRig), "stop_unconfirmed"));

    // A disable likewise: it is an authorised cancellation, not a motion step.
    QueueRig disableRig;
    disableRig.begin();
    enableAndFeed(disableRig, 1, 0);
    CHECK(startQueue(disableRig, "disable 1\n", 1, 40).code == 202);
    tick(disableRig, 60);
    CHECK(countTxTo(1, TxKind::Enable) == 2);  // the enable plus the disable
    injectRx(makeAck(1, 0xF3, 0x02));
    injectRx(driverFlags(1, false));
    // The disable's own stop confirmation is timestamped at its ACK, so only a
    // pair from AFTER that can clear it.
    feedStationary(disableRig, 1, 80);
    tick(disableRig, 100);
    CHECK(disableRig.queue.active());
    feedStationary(disableRig, 1, 120);
    tick(disableRig, 140);
    tick(disableRig, 160);
    CHECK(has(status(disableRig), "\"state\":\"done\""));
    CHECK(has(motorStatus(disableRig, 1), "\"enabled\":false"));
}

static void test_consecutive_home_then_move_acquires_a_new_pair() {
    QueueRig rig;
    rig.begin();
    enableAndFeed(rig, 1, 0);
    CHECK(startQueue(rig, "home 1 0\nmove 1 10 deg\n", 1, 40).code == 202);
    tick(rig, 60);
    feedStationary(rig, 1, 80);
    tick(rig, 100);                       // dispatch the home trigger
    injectRx(makeAck(1, 0x9A, 0x02));
    tick(rig, 120);
    setMillis(140);
    injectRx(makeHomeStatus(1, kHomeOrgRunning));
    tick(rig, 140);
    setMillis(160);
    injectRx(makeHomeStatus(1, kHomeOrgIdleNoFailure));
    feedStationary(rig, 1, 160, 0);
    tick(rig, 180);
    feedStationary(rig, 1, 200, 0);
    tick(rig, 220);
    feedStationary(rig, 1, 240, 0);
    tick(rig, 260);
    CHECK(has(status(rig), "\"step\":2"));  // the home completed

    // The home invalidated position/velocity, so the move must WAIT for a new
    // pair instead of being dispatched (and rejected) with the old one.
    tick(rig, 280);
    CHECK(rig.queue.active());
    TxRecord record;
    CHECK(!lastMoveRecord(record));
    feedStationary(rig, 1, 300, 100);
    tick(rig, 320);
    CHECK(lastMoveRecord(record));
    CHECK(record.magnitude == 100);
}

static void test_raw_then_structured_needs_new_feedback() {
    // A raw-only program reports frames submitted, never a mechanical completion.
    QueueRig rawOnly;
    rawOnly.begin();
    CHECK(startQueue(rawOnly, "hex 01 F3 AB 01 00 6B\n", 1, 0).code == 202);
    tick(rawOnly, 20);
    tick(rawOnly, 40);
    CHECK(has(status(rawOnly), "\"state\":\"done\""));
    CHECK(has(status(rawOnly), "\"message\":\"raw_frames_submitted\""));
    CHECK(has(status(rawOnly), "\"raw\":true"));

    // After a raw frame the software enable and feedback are gone, so a
    // following structured move needs a fresh pair AND a fresh enable: the
    // controller refuses it instead of it running on stale state.
    QueueRig follow;
    follow.begin();
    enableAndFeed(follow, 1, 0);
    CHECK(startQueue(follow, "hex 01 F3 AB 01 00 6B\nmove 1 10 deg\n", 1, 40).code == 202);
    tick(follow, 60);                       // the raw frame goes out
    CHECK(has(motorStatus(follow, 1), "\"enabled\":false"));
    tick(follow, 80);
    CHECK(has(status(follow), "\"step\":2"));  // waiting for its own fresh pair
    feedStationary(follow, 1, 100, 0);
    tick(follow, 120);
    CHECK(has(status(follow), "\"state\":\"failed\""));
    CHECK(has(status(follow), "not_enabled"));
}

// ---------------------------------------------------------------------------
// Direct send contract (operator decision 2026-09-22)
//
// The queue is a sender: these tests drive the real MotorControl through the
// fake bus and assert what goes OUT - never that anything was awaited.
// ---------------------------------------------------------------------------

// Frame count of one opcode on the wire (each packet repeats the function code).
static uint32_t opcodeFrames(uint8_t opcode) { uint32_t n=0; for(const auto& f:capturedTX) if(f.length && f.data[0]==opcode && (f.identifier & 255)==0) ++n; return n; }

static void test_direct_program_sends_without_any_rx() {
    QueueRig rig;
    rig.begin();
    // Historical state that used to block everything: a latched fault and a
    // pending stop on the selected node. Neither is a reason to refuse.
    injectRx(makeAck(1, 0xF3, 0x02));
    setMillis(10);
    rig.motor.poll();
    CHECK(startQueue(rig, "move 1 10 deg\nhome 2 2\nmove 1 -10 deg\nstop 1\n", 1, 20).code == 202);

    // Four steps, one poll apart, with ZERO received frames after start.
    tick(rig, 40);   // move
    tick(rig, 60);   // home
    tick(rig, 80);   // move
    tick(rig, 100);  // stop
    tick(rig, 120);
    CHECK(opcodeFrames(0xCD) >= 2);        // both moves went out
    CHECK(opcodeFrames(0x9A) == 1);        // the home trigger went out
    CHECK(opcodeFrames(0xFE) >= 1);        // the explicit stop went out
    CHECK(has(status(rig), "\"state\":\"done\"") || has(status(rig), "\"state\":\"running\""));

    // No unsolicited enable and no extra stop broadcast: only the program's own
    // frames are on the bus (plus the controller's own poll queries).
    CHECK(opcodeFrames(0xF3) == 0);
    CHECK(capturedTX.size() > 0);
    bool broadcastStop = false;
    for (const CanRawFrame& frame : capturedTX) {
        if (frame.length && frame.data[0] == 0xFE && frame.identifier == 0) broadcastStop = true;
    }
    CHECK(!broadcastStop);

    // The move frame carries mode 2 (relative to the current position) and the
    // relative direction bit, exactly like the manual CD example.
    uint8_t payload[30];
    size_t length = 0;
    CHECK(logicalPayload(0xCD, payload, length));
    CHECK(length == 17);
    CHECK(payload[12] == 2);   // mode
    CHECK(payload[13] == 0);   // immediate execution
    CHECK(payload[16] == 0x6B);
}

static void test_move_frames_keep_big_endian_layout() {
    QueueRig rig;
    rig.begin();
    CHECK(startQueue(rig, "move 2 -90 deg 300 0 0 800\n", 1, 0).code == 202);
    capturedTX.clear();
    tick(rig, 20);
    uint8_t payload[30];
    size_t length = 0;
    CHECK(logicalPayload(0xCD, payload, length));
    // [CD][dir=1 CCW][accel 0000][decel 0000][speed 012C][angle 00000384][02][00][0320][6B]
    CHECK(payload[1] == 1);
    CHECK(payload[2] == 0 && payload[3] == 0 && payload[4] == 0 && payload[5] == 0);
    CHECK(payload[6] == 0x0B && payload[7] == 0xB8);
    CHECK(payload[8] == 0 && payload[9] == 0 && payload[10] == 3 && payload[11] == 0x84);
    CHECK(payload[14] == 3 && payload[15] == 0x20);
}

static void test_user_wait_is_the_only_wait() {
    QueueRig rig;
    rig.begin();
    CHECK(startQueue(rig, "move 1 10 deg\nwait 500\nmove 1 10 deg\n", 1, 0).code == 202);
    tick(rig, 20);
    CHECK(opcodeFrames(0xCD) == 1);
    // Inside the wait window nothing else is sent.
    tick(rig, 40);
    tick(rig, 200);
    tick(rig, 400);
    CHECK(opcodeFrames(0xCD) == 1);
    // Past it, the next step goes out on the next poll.
    tick(rig, 540);
    CHECK(opcodeFrames(0xCD) == 1);   // one step per poll: dispatched next tick
    tick(rig, 560);
    CHECK(opcodeFrames(0xCD) == 2);
}

static void test_legacy_timed_forms_stop_after_the_duration_only() {
    QueueRig rig;
    rig.begin();
    CHECK(startQueue(rig, "torque 1 300 400\nvelocity 1 60\n", 1, 0).code == 202);
    tick(rig, 20);
    CHECK(opcodeFrames(0xC5) == 1);
    CHECK(opcodeFrames(0xFE) == 0);        // nothing stopped before the duration
    tick(rig, 300);
    CHECK(opcodeFrames(0xFE) == 0);
    tick(rig, 440);                        // the user's duration elapsed
    CHECK(opcodeFrames(0xFE) == 1);        // exactly one FE, and no waiting
    tick(rig, 460);
    CHECK(opcodeFrames(0xC6) == 1);        // the short velocity form was sent
    tick(rig, 600);
    CHECK(opcodeFrames(0xFE) == 1);        // and it never stopped it by itself
    tick(rig, 700);
    CHECK(has(status(rig), "\"state\":\"done\""));
}

static void test_repeat_and_cancel_still_work() {
    QueueRig rig;
    rig.begin();
    CHECK(startQueue(rig, "enable 1\n", 2, 0).code == 202);
    tick(rig, 20);
    tick(rig, 40);
    tick(rig, 60);
    tick(rig, 80);
    CHECK(opcodeFrames(0xF3) == 2);        // both iterations sent the line
    CHECK(has(status(rig), "\"state\":\"done\""));

    QueueRig cancelled;
    cancelled.begin();
    CHECK(startQueue(cancelled, "move 1 10 deg\nmove 1 10 deg\n", 1, 0).code == 202);
    tick(cancelled, 20);
    CHECK(opcodeFrames(0xCD) == 1);
    cancelled.queue.cancel("test_cancel");
    tick(cancelled, 40);
    tick(cancelled, 60);
    CHECK(opcodeFrames(0xCD) == 1);        // no further step ran
    CHECK(has(status(cancelled), "\"state\":\"cancelled\""));
}

static void test_tx_failure_stops_the_run_without_extra_can() {
    QueueRig rig;
    rig.begin();
    CHECK(startQueue(rig, "move 1 10 deg\nmove 1 10 deg\n", 1, 0).code == 202);
    setMillis(20);
    failNextMoveTx = true;
    rig.motor.poll();
    rig.queue.poll(20);
    CHECK(has(status(rig), "\"state\":\"failed\""));
    CHECK(has(status(rig), "tx_failed"));
    CHECK(has(status(rig), "\"line\":1"));
    // No automatic stop broadcast and no retry were added by the queue.
    CHECK(countTxOpcode(0xFE) == 0);
    tick(rig, 60);
    CHECK(opcodeFrames(0xCD) == 0);
}

static void test_config_4c_then_home_has_no_readback_barrier() {
    QueueRig rig;
    rig.begin();
    CHECK(startQueue(rig, "hex 01 4C AE 01 02 00 00 1E 00 00 27 10 01 2C 03 E8 00 3C 00 6B\nhome 1 2\n", 1, 0).code == 202);
    tick(rig, 20);
    CHECK(countTxOpcode(0x4C) >= 1);       // the 4C write went out
    tick(rig, 40);
    tick(rig, 60);
    CHECK(countTxOpcode(0x9A) == 1);       // the home trigger followed it directly
    CHECK(countTxOpcode(0x22) == 0);       // no 22 readback was ever requested
    tick(rig, 80);
    CHECK(has(status(rig), "\"state\":\"done\""));
}

static void test_syntax_failure_sends_nothing_and_keeps_the_old_run() {
    QueueRig rig;
    rig.begin();
    CHECK(startQueue(rig, "move 1 10 deg\n", 1, 0).code == 202);
    tick(rig, 20);
    tick(rig, 40);
    tick(rig, 60);
    const std::string done = status(rig);
    CHECK(has(done, "\"state\":\"done\""));
    capturedTX.clear();
    const Result bad = startQueue(rig, "move 1 10 deg\nbogus 1\n", 1, 100);
    CHECK(bad.code == 400);
    CHECK(has(status(rig), "\"line\":2"));
    CHECK(capturedTX.empty());             // an invalid program sends nothing
    CHECK(has(status(rig), "\"message\":\"unknown_action\""));
}


static void test_explicit_await() {
    QueueRig rig;
    rig.begin();
    QueueProgram parsed;
    QueueError error;
    const char* good = "move 1 90\nmove 1 90 deg 30 60 60 800 AwAiT # done\nhome 2 await\nhome 2 2\n";
    CHECK(parseQueueProgram(good, std::strlen(good), rig.rotation, parsed, error));
    CHECK(parsed.count == 4);
    CHECK(!parsed.steps[0].awaitCompletion && parsed.steps[1].awaitCompletion);
    CHECK(parsed.steps[2].awaitCompletion && !parsed.steps[3].awaitCompletion);
    for (const char* bad : {"move 1 await 90", "home 2 await await", "stop 1 await", "wait 1 await", "hex 01 FE 6B await"}) {
        CHECK(startQueue(rig, bad).code == 400);
    }
    CHECK(startQueue(rig, "move 1 90 await\nhome 2 2\n", 1, 10).code == 202);
    tick(rig, 20);
    tick(rig, 3000);
    CHECK(rig.queue.active());
    CHECK(countTxOpcode(0x9A) == 0);
    CHECK(has(status(rig), "waiting_feedback"));
    injectRx(makeAck(1, 0xCD, 2));
    tick(rig, 3010);
    injectRx(makeTarget(1, 900));
    feedStationary(rig, 1, 3020, 900);
    rig.queue.poll(3020);
    CHECK(countTxOpcode(0x9A) == 0);
    feedStationary(rig, 1, 3030, 900);
    rig.queue.poll(3030);
    tick(rig, 3040);
    CHECK(countTxOpcode(0x9A) == 1);
    tick(rig, 3050);
    CHECK(rig.queue.state() == QueueState::Done);

    CHECK(startQueue(rig, "home 2 2 await\nstop 1\n", 1, 4000).code == 202);
    tick(rig, 4010);
    injectRx(makeAck(2, 0x9A, 2));
    tick(rig, 4020);
    injectRx(makeHomeStatus(2, 0));
    feedStationary(rig, 2, 4030);
    rig.queue.poll(4030);
    feedStationary(rig, 2, 4040);
    rig.queue.poll(4040);
    CHECK(has(status(rig), "\"step\":1")); // idle is not completion
    injectRx(makeHomeStatus(2, kHomeOrgRunning));
    tick(rig, 4050);
    injectRx(makeHomeStatus(2, 0));
    tick(rig, 4060);
    feedStationary(rig, 2, 4070);
    rig.queue.poll(4070);
    // Real polling refreshes idle status between position/velocity pairs.
    injectRx(makeHomeStatus(2, 0));
    tick(rig, 4075);
    feedStationary(rig, 2, 4080);
    rig.queue.poll(4080);
    CHECK(has(status(rig), "\"step\":2"));
    tick(rig, 4090);
    tick(rig, 4100);
    CHECK(rig.queue.state() == QueueState::Done);

    CHECK(startQueue(rig, "move 1 90 await\nstop 2", 1, 5000).code == 202);
    tick(rig, 5010);
    // Cached completion from the preceding move cannot release the new step.
    tick(rig, 5020);
    CHECK(has(status(rig), "\"step\":1"));
    injectRx(makeAck(1, 0xCD, 0xE2));
    tick(rig, 5030);
    CHECK(rig.queue.state() == QueueState::Failed);
    CHECK(has(status(rig), "driver_rejected"));
    CHECK(startQueue(rig, "home 2 await", 1, 6000).code == 202);
    tick(rig, 6010);
    CHECK(rig.queue.cancel("cancelled").code == 202);
    tick(rig, 6020);
    CHECK(rig.queue.state() == QueueState::Cancelled);
}

// Sequential homing must switch observation to an otherwise unselected motor.
static void test_home_await_switches_to_motor_three() {
    QueueRig rig;
    rig.begin();
    rig.motor.watch(1);
    CHECK(startQueue(rig, "home 1 2 await\nhome 3 2 await\nstop 2", 1, 10).code == 202);
    for (uint8_t id : {1, 3}) {
        const uint32_t t = id == 1 ? 20 : 200;
        tick(rig, t);
        injectRx(makeAck(id, 0x9A, 2));
        tick(rig, t + 10);
        injectRx(makeHomeStatus(id, kHomeOrgRunning));
        tick(rig, t + 20);
        injectRx(makeHomeStatus(id, 0));
        tick(rig, t + 30);
        feedStationary(rig, id, t + 40);
        rig.queue.poll(t + 40);
        injectRx(makeHomeStatus(id, 0));
        tick(rig, t + 50);
        feedStationary(rig, id, t + 60);
        rig.queue.poll(t + 60);
        CHECK(has(status(rig), id == 1 ? "\"step\":2" : "\"step\":3"));
    }
    tick(rig, 300);
    tick(rig, 310);
    CHECK(rig.queue.state() == QueueState::Done);
}

static void test_pause_queries_keeps_rx_and_manual_tx() {
    QueueRig rig;
    rig.begin();
    rig.motor.setAutoQueriesEnabled(false);
    CHECK(startQueue(rig, "home 3 2 await", 1, 10).code == 202);
    tick(rig, 20);
    capturedTX.clear();
    tick(rig, 100);
    tick(rig, 200);
    CHECK(capturedTX.empty());
    CHECK(has(status(rig), "automatic_queries_paused"));
    const uint8_t query[] = {3, 0x3B, 0x6B};
    CHECK(rig.motor.queueSendLogical(query, sizeof(query)));
    CHECK(countTxOpcode(0x3B) == 1);
    injectRx(makeAck(3, 0x9A, 0x9F));
    tick(rig, 210);
    feedStationary(rig, 3, 220);
    rig.queue.poll(220);
    feedStationary(rig, 3, 230);
    rig.queue.poll(230);
    tick(rig, 240);
    CHECK(rig.queue.state() == QueueState::Done);
    CHECK(countTxOpcode(0x35) == 0 && countTxOpcode(0x36) == 0);
    rig.motor.watch(3);
    rig.motor.setAutoQueriesEnabled(true);
    tick(rig, 300);
    CHECK(countTx(TxKind::ReadSysParam) > 0);
}

static void test_pause_queries_keeps_receiving() {
    QueueRig rig;
    rig.begin();
    rig.motor.setAutoQueriesEnabled(false);
    CHECK(startQueue(rig, "home 3 2 await\nstop 2", 1, 10).code == 202);
    tick(rig, 20);
    const size_t sent = capturedTX.size();
    tick(rig, 100);
    tick(rig, 200);
    CHECK(capturedTX.size() == sent); // both query schedulers are paused
    CHECK(has(status(rig), "automatic_queries_paused"));
    injectRx(makeAck(3, 0x9A, 0x9F));
    tick(rig, 210);
    feedStationary(rig, 3, 220);
    rig.queue.poll(220);
    feedStationary(rig, 3, 230);
    rig.queue.poll(230);
    CHECK(has(status(rig), "\"step\":2")); // RX still consumed
    CHECK(capturedTX.size() == sent);
    tick(rig, 240);
    tick(rig, 250);
    CHECK(rig.queue.state() == QueueState::Done);
    rig.motor.watch(3);
    rig.motor.setAutoQueriesEnabled(true);
    tick(rig, 400);
    CHECK(countTx(TxKind::ReadSysParam) > 0);
}

static void test_home_batched_rx_keeps_transition() {
    QueueRig rig;
    rig.begin();
    CHECK(startQueue(rig, "home 3 2 await\nstop 2", 1, 10).code == 202);
    tick(rig, 20);
    // All queued RX is drained before CommandQueue observes the node.
    injectRx(makeAck(3, 0x9A, 2));
    injectRx(makeHomeStatus(3, 7));
    injectRx(makeHomeStatus(3, 3));
    injectRx(makeAck(3, 0xF3, 2));
    tick(rig, 30);
    feedStationary(rig, 3, 40); rig.queue.poll(40);
    feedStationary(rig, 3, 50); rig.queue.poll(50);
    CHECK(has(status(rig), "\"step\":2"));
    tick(rig, 60); tick(rig, 70);
    CHECK(rig.queue.state() == QueueState::Done);
}

static void test_home_rx_order_and_same_tick() {
    for (int scenario = 0; scenario < 3; ++scenario) {
        QueueRig rig; rig.begin();
        CHECK(startQueue(rig, "home 3 2 await", 1, 10).code == 202);
        tick(rig, 20);
        injectRx(makeAck(3, 0x9A, scenario == 2 ? 0xE2 : 2));
        injectRx(makeHomeStatus(3, 7));
        injectRx(makeHomeStatus(3, 3));
        if (scenario == 1) injectRx(makeHomeStatus(3, 7));
        if (scenario == 2) injectRx(makeAck(3, 0x9A, 2));
        tick(rig, 20); // evidence ordering must not rely on millisecond changes
        feedStationary(rig, 3, 30); rig.queue.poll(30);
        feedStationary(rig, 3, 40); rig.queue.poll(40);
        tick(rig, 50);
        if (scenario == 0) CHECK(rig.queue.state() == QueueState::Done);
        if (scenario == 1) {
            CHECK(rig.queue.active()); // renewed running revokes completion
            CHECK(has(status(rig), "home_wait_end"));
        }
        if (scenario == 2) CHECK(rig.queue.state() == QueueState::Failed);
    }
}

static void test_fast_home_without_running_sample() {
    QueueRig rig; rig.begin();
    CHECK(startQueue(rig, "home 3 2 await", 1, 10).code == 202);
    tick(rig, 20);
    injectRx(makeAck(3, 0x9A, 2)); tick(rig, 30);
    injectRx(makeHomeStatus(3, 3)); tick(rig, 40);
    feedStationary(rig, 3, 50); rig.queue.poll(50);
    feedStationary(rig, 3, 60); rig.queue.poll(60);
    CHECK(rig.queue.active()); // startup grace rejects early idle
    tick(rig, 1100);
    CHECK(rig.queue.active()); // old idle cannot release a later run
    injectRx(makeHomeStatus(3, 3)); tick(rig, 1110);
    feedStationary(rig, 3, 1120); rig.queue.poll(1120);
    injectRx(makeHomeStatus(3, 3)); tick(rig, 1130);
    feedStationary(rig, 3, 1140); rig.queue.poll(1140);
    tick(rig, 1150);
    CHECK(rig.queue.state() == QueueState::Done);
}

static void test_broadcast_enable_frames() {
    QueueRig rig; rig.begin();
    CHECK(rig.motor.broadcastEnable(true).code == 202);
    const uint8_t enable[] = {0xF3,0xAB,1,0,0x6B};
    CHECK(sawTxId(0, enable, sizeof(enable)));
    CHECK(rig.motor.broadcastEnable(false).code == 202);
    const uint8_t disable[] = {0xF3,0xAB,0,0,0x6B};
    CHECK(sawTxId(0, disable, sizeof(disable)));
}

struct TestCase {
    const char* name;
    void (*fn)();
};

int main() {
    const TestCase tests[] = {
        {"fast home idle after grace without running sample", test_fast_home_without_running_sample},
        {"broadcast enable and disable wire frames", test_broadcast_enable_frames},
        {"home RX ordering, same tick and rejection", test_home_rx_order_and_same_tick},
        {"batched homing RX retains ACK and running-to-idle transition", test_home_batched_rx_keeps_transition},
        {"pause query TX while continuing RX", test_pause_queries_keeps_receiving},
        {"pause automatic queries preserves RX and explicit TX", test_pause_queries_keeps_rx_and_manual_tx},
        {"await switches from motor 1 to unselected motor 3", test_home_await_switches_to_motor_three},
        {"explicit await syntax, completion, rejection and cancellation", test_explicit_await},
        {"program validation is atomic and reports source lines", test_program_validation_is_atomic_with_source_lines},
        {"strict numbers, units and raw id/DLC bounds", test_strict_numeric_and_raw_bounds},
        {"rotation distance, rev conversion and direction", test_rotation_distance_and_direction_conversion},
        {"saved board policy does not gate the queue", test_saved_board_policy_does_not_gate_the_queue},
        {"short torque/velocity forms and protocol bounds", test_short_forms_and_protocol_bounds},
        {"hostile tokens, hex ids and embedded NULs are rejected", test_hostile_tokens_are_rejected_without_sending},
        {"direct program sends without any RX", test_direct_program_sends_without_any_rx},
        {"move frames keep the big-endian wire layout", test_move_frames_keep_big_endian_layout},
        {"only a user wait waits", test_user_wait_is_the_only_wait},
        {"legacy timed forms stop after the duration only", test_legacy_timed_forms_stop_after_the_duration_only},
        {"repeat still repeats and cancel still cancels", test_repeat_and_cancel_still_work},
        {"TX failure ends the run without extra CAN", test_tx_failure_stops_the_run_without_extra_can},
        {"4C then home has no readback barrier", test_config_4c_then_home_has_no_readback_barrier},
        {"syntax failure sends nothing and keeps the old run", test_syntax_failure_sends_nothing_and_keeps_the_old_run},
        // PARKED (old supervised-queue contract, not run): the functions above
        // still compile but assert the previous waiter behaviour and must be
        // rewritten or deleted in the follow-up pass:
        //   test_structured_program_progresses_on_real_evidence,
        //   test_new_move_never_consumes_the_previous_outcome,
        //   test_enable_step_waits_ack_and_stationary, test_home_step_outcomes,
        //   test_timed_steps_send_once_then_stop_and_wait,
        //   test_raw_steps_report_sent_only, test_repeat_bounds_and_cancel,
        //   test_fail_fast_and_bus_loss, test_enable_needs_post_request_evidence,
        //   test_stop_and_disable_dispatch_promptly_on_a_quiet_node,
        //   test_consecutive_home_then_move_acquires_a_new_pair,
        //   test_raw_then_structured_needs_new_feedback.
    };

    // Parked supervised-queue tests: referenced only so -Wunused-function stays
    // quiet. They encode the OLD wait-for-evidence contract and are deliberately
    // not executed; the follow-up pass rewrites or deletes them.
    (void)&test_structured_program_progresses_on_real_evidence;
    (void)&test_new_move_never_consumes_the_previous_outcome;
    (void)&test_enable_step_waits_ack_and_stationary;
    (void)&test_home_step_outcomes;
    (void)&test_timed_steps_send_once_then_stop_and_wait;
    (void)&test_raw_steps_report_sent_only;
    (void)&test_repeat_bounds_and_cancel;
    (void)&test_fail_fast_and_bus_loss;
    (void)&test_enable_needs_post_request_evidence;
    (void)&test_stop_and_disable_dispatch_promptly_on_a_quiet_node;
    (void)&test_consecutive_home_then_move_acquires_a_new_pair;
    (void)&test_raw_then_structured_needs_new_feedback;

    const int count = static_cast<int>(sizeof(tests) / sizeof(tests[0]));
    for (int i = 0; i < count; ++i) {
        const int before = g_failures;
        tests[i].fn();
        std::printf("%s  %s\n", g_failures == before ? "PASS" : "FAIL", tests[i].name);
    }
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
