// Contract checks through DeviceAPI only. The real MotorControl/CommandQueue
// composition runs over a fake CAN bus and deterministic clock.
#include "DeviceAPI.h"
#include "fake_x42s.h"

#include <cstdio>
#include <cstring>

using namespace motion;
using namespace fakecan;

static int failures = 0;
static int checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)

struct Rotation : QueueRotationSource {
    bool rotationMm(uint8_t, double&) const override { return false; }
};

struct Rig {
    MotorControl motor;
    CommandQueue queue;
    DeviceAPI api;
    Rotation rotation;
    Rig() : queue(motor), api(motor, queue) { fakeReset(); }
    void begin() { CHECK(api.begin(4, 5, 500000)); }
    void poll(uint32_t now) { setMillis(now); api.poll(now); }
    DeviceSnapshot state(uint8_t id, uint32_t now) {
        setMillis(now);
        return api.readSnapshot(id);
    }
};

static CanRawFrame flags(uint8_t id, bool enabled) {
    const uint8_t data[] = {0x3A, uint8_t(enabled ? 1 : 0), 0x6B};
    return makeFrame(id, data, sizeof(data));
}

static MoveRequest move(uint8_t id) {
    MoveRequest request = {id, 10.0f, 30.0f, 100.0f, 100.0f, 1000};
    return request;
}

static size_t countWireOpcode(uint8_t id, uint8_t opcode) {
    size_t count = 0;
    for (size_t i = 0; i < capturedTX.size(); ++i) {
        const CanRawFrame& frame = capturedTX[i];
        if (x42sCanAddress(frame.identifier) == id && frame.length && frame.data[0] == opcode) ++count;
    }
    return count;
}

static size_t countWireEnableOn(uint8_t id) {
    size_t count = 0;
    for (size_t i = 0; i < capturedTX.size(); ++i) {
        const CanRawFrame& frame = capturedTX[i];
        if (x42sCanAddress(frame.identifier) == id && frame.length >= 4 &&
            frame.data[0] == kFrameEnable && frame.data[2] == 1) ++count;
    }
    return count;
}

static void enableAndFeed(Rig& rig, uint8_t id) {
    setMillis(0);
    CHECK(rig.api.requestEnable(id, true).accepted());
    injectRx(makeAck(id, kFrameEnable, 0x02));
    injectRx(flags(id, true));
    rig.poll(20);
    injectRx(makePosition(id, 0));
    injectRx(makeVelocity(id, 0));
    rig.poll(40);
    CHECK(rig.state(id, 40).motor.enabledConfirmed);
}

static void test_receipt_ack_and_observation_are_distinct() {
    Rig rig;
    rig.begin();
    const DeviceReceipt refused = rig.api.requestMove(move(1));
    CHECK(refused.admission == DeviceAdmission::Busy);
    CHECK(refused.code == 409);
    CHECK(countTx(TxKind::Move) == 0);

    setMillis(0);
    const DeviceReceipt admitted = rig.api.requestEnable(1, true);
    CHECK(admitted.admission == DeviceAdmission::Accepted);
    CHECK(admitted.code == 202);
    DeviceSnapshot state = rig.state(1, 0);
    CHECK(state.motor.enablePending);
    CHECK(!state.motor.enabledConfirmed);

    const size_t beforeRead = capturedTX.size();
    state = rig.state(1, 0);
    CHECK(capturedTX.size() == beforeRead);
    CHECK(state.sampledAtMs == 0);
    CHECK(!state.motor.positionValid);

    injectRx(makeAck(1, kFrameEnable, 0x02));
    rig.poll(20);
    state = rig.state(1, 20);
    CHECK(state.motor.enableProtocolAck);
    CHECK(!state.motor.enabledConfirmed);

    injectRx(flags(1, true));
    rig.poll(30);
    state = rig.state(1, 30);
    CHECK(state.motor.enabledConfirmed);
    CHECK(!state.motor.positionValid);
}

static void test_manual_move_reached_needs_new_evidence() {
    Rig rig;
    rig.begin();
    enableAndFeed(rig, 1);
    setMillis(40);
    const DeviceReceipt admitted = rig.api.requestMove(move(1));
    CHECK(admitted.accepted());
    CHECK(rig.state(1, 40).manualMove == DeviceMoveStage::Running);

    injectRx(makeAck(1, kFrameMove, 0x02));
    rig.poll(60);
    CHECK(rig.state(1, 60).manualMove == DeviceMoveStage::Running);

    injectRx(makePosition(1, 96));
    injectRx(makeVelocity(1, 0));
    rig.poll(80);
    CHECK(rig.state(1, 80).manualMove == DeviceMoveStage::Running);
    rig.poll(81); // Reusing the same feedback pair cannot complete the action.
    CHECK(rig.state(1, 81).manualMove == DeviceMoveStage::Running);

    injectRx(makePosition(1, 100));
    injectRx(makeVelocity(1, 0));
    rig.poll(100);
    const DeviceSnapshot state = rig.state(1, 100);
    CHECK(state.manualMove == DeviceMoveStage::Reached);
    CHECK(state.motor.positionValid);
    CHECK(state.motor.positionTenths == 100);
    CHECK(state.motor.positionAgeMs == 0);
    const size_t beforeRead = capturedTX.size();
    const DeviceSnapshot stale = rig.state(1, 701);
    CHECK(!stale.motor.positionValid);
    CHECK(!stale.motor.velocityValid);
    CHECK(stale.motor.positionAgeMs == 601);
    CHECK(capturedTX.size() == beforeRead);
}

static void test_home_response_modes_need_fresh_evidence() {
    Rig received;
    received.begin();
    enableAndFeed(received, 1);
    setMillis(40);
    CHECK(received.api.requestHome(1, 0).accepted());
    CHECK(received.state(1, 40).manualHome == DeviceHomeStage::Running);
    injectRx(makeAck(1, 0x9A, 0x02)); // Receive mode: only accepted by driver.
    received.poll(60);
    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    received.poll(80);
    CHECK(received.state(1, 80).manualHome == DeviceHomeStage::Running);

    Rig completed;
    completed.begin();
    enableAndFeed(completed, 1);
    setMillis(40);
    CHECK(completed.api.requestHome(1, 0).accepted());
    injectRx(makeAck(1, 0x9A, 0x9F)); // Homing completion reply alone is not reach proof.
    completed.poll(60);
    CHECK(completed.state(1, 60).manualHome == DeviceHomeStage::Running);
    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    completed.poll(80);
    CHECK(completed.state(1, 80).manualHome == DeviceHomeStage::Reached);
}

static void test_home_no_motion_is_distinct_from_reached() {
    Rig rig;
    rig.begin();
    enableAndFeed(rig, 1);
    setMillis(40);
    CHECK(rig.api.requestHome(1, 0).accepted());
    injectRx(makeAck(1, 0x9A, 0x12));
    rig.poll(60);
    CHECK(rig.state(1, 60).manualHome == DeviceHomeStage::NoMotion);
    CHECK(!rig.state(1, 60).fault);
}

static void test_program_done_is_not_motor_reached() {
    Rig rig;
    rig.begin();
    const char program[] = "move 1 10 deg\n"; // No await: delivery can finish before motion.
    setMillis(0);
    const DeviceReceipt started = rig.api.startProgram(program, sizeof(program) - 1,
                                                        1, rig.rotation, 0);
    CHECK(started.accepted());
    CHECK(started.runId != 0);
    CHECK(rig.state(1, 0).program == DeviceProgramStage::Running);
    for (uint32_t t = 1; t <= 10; ++t) rig.poll(t);
    const DeviceSnapshot state = rig.state(1, 10);
    CHECK(state.program == DeviceProgramStage::Done);
    CHECK(state.programRunId == started.runId);
    CHECK(state.manualMove == DeviceMoveStage::None);
    CHECK(!state.motor.positionValid); // No observed motion evidence arrived.
    CHECK(countWireOpcode(1, kFrameMove) >= 1);
}

static void test_bad_program_is_atomic_and_cancel_prevents_later_step() {
    Rig rig;
    rig.begin();
    const char invalid[] = "enable 1\nmove 1 abc\n";
    const size_t before = capturedTX.size();
    const DeviceReceipt rejected = rig.api.startProgram(invalid, sizeof(invalid) - 1,
                                                         1, rig.rotation, 0);
    CHECK(rejected.admission == DeviceAdmission::Invalid);
    CHECK(rejected.runId == 0);
    CHECK(capturedTX.size() == before);
    CHECK(rig.state(1, 0).program == DeviceProgramStage::Idle);

    const char waiting[] = "wait 1000\nenable 1\n";
    const DeviceReceipt started = rig.api.startProgram(waiting, sizeof(waiting) - 1,
                                                        1, rig.rotation, 0);
    CHECK(started.accepted());
    rig.poll(10);
    const DeviceReceipt cancelled = rig.api.cancelProgram("test_cancel");
    CHECK(cancelled.accepted());
    CHECK(rig.state(1, 10).program == DeviceProgramStage::Cancelled);
    for (uint32_t t = 20; t <= 1200; t += 20) rig.poll(t);
    CHECK(countWireEnableOn(1) == 0);
}

static void test_stop_is_a_request_until_feedback_confirms() {
    Rig rig;
    rig.begin();
    enableAndFeed(rig, 1);
    setMillis(50);
    const DeviceReceipt stopped = rig.api.requestStop(1);
    CHECK(stopped.accepted());
    CHECK(sawStopFor(1));
    const DeviceSnapshot state = rig.state(1, 50);
    CHECK(state.motor.stopPending);
    CHECK(state.manualMove != DeviceMoveStage::Reached);
}

static void test_all_public_stop_and_disable_paths_preempt_program() {
    const char waiting[] = "wait 1000\nenable 1\n";
    for (int path = 0; path < 7; ++path) {
        Rig rig;
        rig.begin();
        setMillis(0);
        CHECK(rig.api.startProgram(waiting, sizeof(waiting) - 1,
                                   1, rig.rotation, 0).accepted());
        rig.poll(10);
        CHECK(rig.state(1, 10).program == DeviceProgramStage::Running);
        setMillis(10);
        DeviceReceipt result = {DeviceAdmission::Failed, 0, "", 0};
        switch (path) {
            case 0: result = rig.api.requestStop(1); break;
            case 1: result = rig.api.requestStopAll(); break;
            case 2: result = rig.api.requestEnable(1, false); break;
            case 3: result = rig.api.requestBroadcastEnable(false); break;
            case 4: {
                const uint8_t raw[] = {1, 0xFE, 0x98, 0, 0x6B};
                result = rig.api.requestRawCommand(raw, sizeof(raw)); break;
            }
            case 5: {
                const uint8_t raw[] = {1, 0x9C, 0x48, 0x6B};
                result = rig.api.requestRawCommand(raw, sizeof(raw)); break;
            }
            case 6: {
                const uint8_t raw[] = {1, 0xF3, 0xAB, 0, 0, 0x6B};
                result = rig.api.requestRawCommand(raw, sizeof(raw)); break;
            }
        }
        CHECK(result.accepted());
        CHECK(rig.state(1, 10).program == DeviceProgramStage::Cancelled);
        for (uint32_t t = 20; t <= 1200; t += 20) rig.poll(t);
        CHECK(countWireEnableOn(1) == 0);
    }
}

static void test_invalid_target_never_preempts_program() {
    Rig rig;
    rig.begin();
    const char waiting[] = "wait 1000\nenable 1\n";
    CHECK(rig.api.startProgram(waiting, sizeof(waiting) - 1,
                               1, rig.rotation, 0).accepted());
    rig.poll(10);
    const size_t before = capturedTX.size();
    const DeviceReceipt invalidDisable = rig.api.requestEnable(0, false);
    const DeviceReceipt invalidStop = rig.api.requestStop(0);
    const uint8_t malformedStop[] = {1, 0xFE, 0x98, 0, 0};
    const DeviceReceipt invalidRaw = rig.api.requestRawCommand(malformedStop, sizeof(malformedStop));
    CHECK(invalidDisable.admission == DeviceAdmission::Invalid);
    CHECK(invalidStop.admission == DeviceAdmission::Invalid);
    CHECK(invalidRaw.admission == DeviceAdmission::Invalid);
    CHECK(invalidDisable.code == 400 && invalidStop.code == 400);
    CHECK(capturedTX.size() == before); // No 9C/FE broadcast was sent.
    CHECK(rig.state(1, 10).program == DeviceProgramStage::Running);
    for (uint32_t t = 20; t <= 1200; t += 20) rig.poll(t);
    CHECK(countWireEnableOn(1) == 1);
}

static void test_snapshot_reads_create_no_delayed_query_demand() {
    size_t baselineFrames = 0;
    {
        Rig baseline;
        baseline.begin();
        baseline.poll(100);
        baselineFrames = capturedTX.size();
    }
    Rig observed;
    observed.begin();
    for (int i = 0; i < 50; ++i) {
        const DeviceSnapshot state = observed.state(1, 100);
        CHECK(state.sampledAtMs == 100);
    }
    CHECK(capturedTX.empty());
    observed.poll(100);
    CHECK(capturedTX.size() == baselineFrames);
}

int main() {
    test_receipt_ack_and_observation_are_distinct();
    test_manual_move_reached_needs_new_evidence();
    test_home_response_modes_need_fresh_evidence();
    test_home_no_motion_is_distinct_from_reached();
    test_program_done_is_not_motor_reached();
    test_bad_program_is_atomic_and_cancel_prevents_later_step();
    test_stop_is_a_request_until_feedback_confirms();
    test_all_public_stop_and_disable_paths_preempt_program();
    test_invalid_target_never_preempts_program();
    test_snapshot_reads_create_no_delayed_query_demand();
    if (failures) std::printf("device-api: %d/%d checks failed\n", failures, checks);
    else std::printf("device-api: %d checks passed\n", checks);
    return failures ? 1 : 0;
}
