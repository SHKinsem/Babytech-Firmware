// Contract checks through DeviceAPI only. The real MotorControl/CommandQueue
// composition runs over a fake CAN bus and deterministic clock.
#include "DeviceAPI.h"
#include "fake_x42s.h"

#include <cstdio>
#include <cstring>
#include <string>

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
    const DeviceReceipt enabled = rig.api.requestEnable(id, true);
    CHECK(enabled.accepted());
    CHECK(enabled.operationId == 0);
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

static void test_manual_failure_json_keeps_diagnostic_snapshot() {
    {
        Rig rig;
        rig.begin();
        DebugLimits limits;
        limits.maxMoveDurationMs = 2000;
        CHECK(rig.motor.setDebugLimits(limits));
        enableAndFeed(rig, 1);
        setMillis(60);
        const DeviceReceipt started = rig.api.requestMove(move(1));
        CHECK(started.accepted());
        CHECK(started.operationId != 0);
        injectRx(makeAck(1, kFrameMove, 0x02));
        rig.poll(80);
        for (uint32_t now = 100; now <= 2100; now += 400) {
            injectRx(makePosition(1, 25));
            injectRx(makeVelocity(1, 100));
            rig.poll(now);
        }
        CHECK(rig.state(1, 2100).manualMove == DeviceMoveStage::Failed);
        const DeviceOperationResult failed = rig.api.readOperation(started.operationId);
        CHECK(failed.state == DeviceOperationState::Failed);
        CHECK(failed.fault == DeviceOperationFault::MoveTimeout);
        CHECK(failed.protocolAck);
        CHECK(!failed.reached);
        const std::string status = rig.motor.statusJson(1).str();
        CHECK(status.find("\"fault\":\"move_timeout\"") != std::string::npos);
        CHECK(status.find("\"lastMoveFailure\":{\"targetDeg\":10.0,\"positionDeg\":2.5,"
                          "\"speedRpm\":10.0,\"elapsedMs\":2040,\"deadlineMs\":2000,"
                          "\"enabled\":true}") != std::string::npos);
    }
    {
        Rig rig;
        rig.begin();
        enableAndFeed(rig, 1);
        setMillis(60);
        const DeviceReceipt started = rig.api.requestMove(move(1));
        CHECK(started.accepted());
        CHECK(started.operationId != 0);
        injectRx(makeAck(1, kFrameMove, 0x02));
        rig.poll(80);
        rig.poll(800); // No new position or velocity after command.
        CHECK(rig.state(1, 800).manualMove == DeviceMoveStage::Failed);
        const DeviceOperationResult failed = rig.api.readOperation(started.operationId);
        CHECK(failed.state == DeviceOperationState::Failed);
        CHECK(failed.fault == DeviceOperationFault::FeedbackStale);
        CHECK(failed.protocolAck);
        CHECK(!failed.reached);
        const std::string status = rig.motor.statusJson(1).str();
        CHECK(status.find("\"fault\":\"feedback_stale\"") != std::string::npos);
        CHECK(status.find("\"lastMoveFailure\":{\"targetDeg\":10.0,\"positionDeg\":null,"
                          "\"speedRpm\":null,\"elapsedMs\":740,\"deadlineMs\":60000,"
                          "\"enabled\":true}") != std::string::npos);
    }
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
    const DeviceReceipt home = completed.api.requestHome(1, 0);
    CHECK(home.accepted());
    injectRx(makeAck(1, 0x9A, 0x9F)); // Homing completion reply alone is not reach proof.
    completed.poll(60);
    CHECK(completed.state(1, 60).manualHome == DeviceHomeStage::Running);
    DeviceOperationResult result = completed.api.readOperation(home.operationId);
    CHECK(result.protocolAck);
    CHECK(!result.receiveAckObserved);
    CHECK(result.driverReachedObserved);
    CHECK(!result.reached);
    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    completed.poll(80);
    CHECK(completed.state(1, 80).manualHome == DeviceHomeStage::Reached);
    result = completed.api.readOperation(home.operationId);
    CHECK(result.state == DeviceOperationState::Reached);
    CHECK(!result.receiveAckObserved);
    CHECK(result.driverReachedObserved);
    CHECK(result.reached);
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
    // The driver's second no-motion code has the same terminal meaning, and a
    // no-motion result must release the supervised slot without latching fault.
    setMillis(60);
    CHECK(rig.api.requestHome(1, 0).accepted());
    injectRx(makeAck(1, 0x9A, 0x22));
    rig.poll(80);
    CHECK(rig.state(1, 80).manualHome == DeviceHomeStage::NoMotion);
    CHECK(!rig.state(1, 80).fault);
    setMillis(80);
    CHECK(rig.api.requestMove(move(1)).accepted());
    CHECK(rig.state(1, 80).manualMove == DeviceMoveStage::Running);
}

static void test_manual_move_and_home_share_one_slot() {
    Rig rig;
    rig.begin();
    enableAndFeed(rig, 1);
    setMillis(40);
    CHECK(rig.api.requestMove(move(1)).accepted());
    CHECK(rig.api.requestHome(1, 0).admission == DeviceAdmission::Busy);
    CHECK(rig.api.requestHome(2, 0).admission == DeviceAdmission::Busy);
    CHECK(rig.api.requestMove(move(2)).admission == DeviceAdmission::Busy);
    CHECK(countWireOpcode(1, 0x9A) == 0);
    CHECK(countWireOpcode(2, kFrameMove) == 0);
    CHECK(rig.state(1, 40).manualMove == DeviceMoveStage::Running);
    CHECK(rig.state(1, 40).manualHome == DeviceHomeStage::None);

    setMillis(50);
    CHECK(rig.api.requestStop(1).accepted());
    CHECK(rig.state(1, 50).manualMove == DeviceMoveStage::Cancelled);
    CHECK(rig.api.requestHome(1, 0).admission == DeviceAdmission::Busy);
    injectRx(makeAck(1, kFrameMove, 0x9F)); // Reply to the cancelled move.
    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    rig.poll(60);
    CHECK(rig.state(1, 60).manualMove == DeviceMoveStage::Cancelled);
    CHECK(!rig.state(1, 60).motor.stopPending);

    setMillis(60);
    CHECK(rig.api.requestHome(1, 0).accepted());
    CHECK(rig.api.requestMove(move(1)).admission == DeviceAdmission::Busy);
    CHECK(rig.state(1, 60).manualHome == DeviceHomeStage::Running);
    injectRx(makeAck(1, kFrameMove, 0x9F)); // Cannot complete a new Home.
    rig.poll(80);
    CHECK(rig.state(1, 80).manualHome == DeviceHomeStage::Running);
}

static void test_stop_requires_both_post_request_stationary_samples() {
    Rig rig;
    rig.begin();
    enableAndFeed(rig, 1);
    setMillis(50);
    CHECK(rig.api.requestStop(1).accepted());
    rig.poll(51); // Pre-stop samples are still fresh but cannot confirm the stop.
    CHECK(rig.state(1, 51).motor.stopPending);
    injectRx(makeAck(1, kFrameStop, 0x02));
    injectRx(makePosition(1, 0));
    rig.poll(60);
    CHECK(rig.state(1, 60).motor.stopPending); // Velocity is still pre-stop.
    injectRx(makeVelocity(1, 10));
    rig.poll(70);
    CHECK(rig.state(1, 70).motor.stopPending); // New feedback still moves.
    injectRx(makeVelocity(1, 0));
    rig.poll(80);
    CHECK(!rig.state(1, 80).motor.stopPending);
}

static void test_queue_takeover_ignores_late_manual_home_reply() {
    Rig rig;
    rig.begin();
    enableAndFeed(rig, 1);
    setMillis(40);
    CHECK(rig.api.requestHome(1, 0).accepted());
    CHECK(rig.state(1, 40).manualHome == DeviceHomeStage::Running);

    const char waiting[] = "wait 1000\n";
    setMillis(50);
    const DeviceReceipt run = rig.api.startProgram(waiting, sizeof(waiting) - 1,
                                                    1, rig.rotation, 50);
    CHECK(run.accepted());
    CHECK(run.runId != 0);
    CHECK(rig.state(1, 50).manualHome == DeviceHomeStage::None);
    CHECK(rig.state(1, 50).program == DeviceProgramStage::Running);
    injectRx(makeAck(1, 0x9A, 0x9F)); // Late completion for abandoned Home.
    injectRx(makePosition(1, 0));
    injectRx(makeVelocity(1, 0));
    rig.poll(60);
    CHECK(rig.state(1, 60).manualHome == DeviceHomeStage::None);
    CHECK(rig.state(1, 60).program == DeviceProgramStage::Running);
    CHECK(rig.state(1, 60).programRunId == run.runId);
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
        DeviceReceipt result = {DeviceAdmission::Failed, 0, "", 0, 0};
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
        CHECK(result.operationId == 0);
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

static void test_manual_operation_ids_and_results() {
    Rig rig;
    rig.begin();
    const DeviceReceipt early = rig.api.requestMove(move(1));
    CHECK(!early.accepted());
    CHECK(early.operationId == 0);
    CHECK(rig.api.requestEnable(0, true).operationId == 0);
    enableAndFeed(rig, 1);

    setMillis(40);
    const DeviceReceipt started = rig.api.requestMove(move(1));
    CHECK(started.accepted());
    CHECK(started.runId == 0);
    CHECK(started.operationId != 0);
    CHECK(rig.api.requestHome(1, 0).operationId == 0); // busy is not a new run
    DeviceOperationResult result = rig.api.readOperation(started.operationId);
    CHECK(result.operationId == started.operationId);
    CHECK(result.motorId == 1);
    CHECK(result.kind == DeviceOperationKind::Move);
    CHECK(result.state == DeviceOperationState::Running);
    CHECK(!result.protocolAck);
    CHECK(!result.receiveAckObserved);
    CHECK(!result.driverReachedObserved);
    CHECK(!result.reached);

    injectRx(makeAck(1, kFrameMove, 0x02));
    rig.poll(60);
    result = rig.api.readOperation(started.operationId);
    CHECK(result.state == DeviceOperationState::Running);
    CHECK(result.protocolAck);
    CHECK(result.receiveAckObserved);
    CHECK(!result.driverReachedObserved);
    CHECK(!result.reached);
    injectRx(makeAck(1, kFrameMove, 0x9F)); // Both mode's second reply.
    rig.poll(70);
    result = rig.api.readOperation(started.operationId);
    CHECK(result.state == DeviceOperationState::Running);
    CHECK(result.protocolAck);
    CHECK(result.receiveAckObserved);
    CHECK(result.driverReachedObserved);
    CHECK(!result.reached);
    injectRx(makePosition(1, 96));
    injectRx(makeVelocity(1, 0));
    rig.poll(80);
    CHECK(rig.api.readOperation(started.operationId).state == DeviceOperationState::Running);
    injectRx(makePosition(1, 100));
    injectRx(makeVelocity(1, 0));
    rig.poll(100);
    result = rig.api.readOperation(started.operationId);
    CHECK(result.state == DeviceOperationState::Reached);
    CHECK(result.receiveAckObserved);
    CHECK(result.driverReachedObserved);
    CHECK(result.reached);

    // Repeated history reads are memory-only; snapshot tests also check that
    // observation does not arm a query for a later poll.
    const size_t beforeRead = capturedTX.size();
    for (int i = 0; i < 20; ++i) {
        CHECK(rig.api.readOperation(started.operationId).state == DeviceOperationState::Reached);
    }
    CHECK(capturedTX.size() == beforeRead);

    setMillis(110);
    const DeviceReceipt homing = rig.api.requestHome(1, 0);
    CHECK(homing.accepted());
    CHECK(homing.operationId > started.operationId);
    CHECK(homing.runId == 0);
    CHECK(rig.api.readOperation(homing.operationId).kind == DeviceOperationKind::Home);
    injectRx(makeAck(1, 0x9A, 0x12));
    rig.poll(120);
    result = rig.api.readOperation(homing.operationId);
    CHECK(result.state == DeviceOperationState::NoMotion);
    CHECK(!result.protocolAck);
    CHECK(!result.receiveAckObserved);
    CHECK(!result.driverReachedObserved);
    CHECK(!result.reached);
    CHECK(rig.api.readOperation(started.operationId).state == DeviceOperationState::Reached);

    DirectPositionRequest direct;
    direct.id = 1;
    direct.angleTenths = 100;
    direct.speedTenths = 300;
    setMillis(130);
    const DeviceReceipt directMove = rig.api.requestDirectPosition(direct);
    CHECK(directMove.accepted());
    CHECK(directMove.operationId > homing.operationId);
    CHECK(rig.api.readOperation(directMove.operationId).kind ==
          DeviceOperationKind::DirectPosition);
    const DeviceReceipt stopped = rig.api.requestStop(1);
    CHECK(stopped.operationId == 0);
    CHECK(rig.api.readOperation(directMove.operationId).state ==
          DeviceOperationState::Cancelled);
    CHECK(!rig.api.readOperation(directMove.operationId).driverReachedObserved);
    CHECK(rig.state(1, 130).motor.stopPending); // software cancellation is not stop proof
    injectRx(makeAck(1, 0xFB, 0x9F)); // late answer cannot revive the handle
    rig.poll(140);
    CHECK(rig.api.readOperation(directMove.operationId).state ==
          DeviceOperationState::Cancelled);
    CHECK(!rig.api.readOperation(directMove.operationId).driverReachedObserved);
    injectRx(makePosition(1, 100));
    injectRx(makeVelocity(1, 0));
    rig.poll(150);
    CHECK(!rig.state(1, 150).motor.stopPending);
    CHECK(rig.api.readOperation(directMove.operationId).state ==
          DeviceOperationState::Cancelled);
}

static void test_program_and_demo_receipts_do_not_claim_manual_operations() {
    Rig rig;
    rig.begin();
    enableAndFeed(rig, 1);
    setMillis(40);
    const DeviceReceipt manual = rig.api.requestHome(1, 0);
    CHECK(manual.operationId != 0);

    const char waiting[] = "wait 1000\n";
    setMillis(50);
    const DeviceReceipt program = rig.api.startProgram(waiting, sizeof(waiting) - 1,
                                                       1, rig.rotation, 50);
    CHECK(program.accepted());
    CHECK(program.runId != 0);
    CHECK(program.operationId == 0);
    CHECK(rig.api.readOperation(manual.operationId).state ==
          DeviceOperationState::Superseded);
    CHECK(rig.state(1, 50).manualHome == DeviceHomeStage::None);
    injectRx(makeAck(1, 0x9A, 0x9F));
    rig.poll(60);
    CHECK(rig.api.readOperation(manual.operationId).state ==
          DeviceOperationState::Superseded);
    CHECK(rig.api.cancelProgram("test").operationId == 0);

    Rig demoRig;
    demoRig.begin();
    QueueProgram demo;
    demo.count = 1;
    demo.steps[0].action = QueueAction::Wait;
    demo.steps[0].waitMs = 1000;
    const DeviceReceipt demoRun = demoRig.api.startDemo(demo, 0);
    CHECK(demoRun.accepted());
    CHECK(demoRun.runId != 0);
    CHECK(demoRun.operationId == 0);
    CHECK(demoRig.api.requestStopAll().operationId == 0);
}

static void test_raw_invalidation_and_session_expiry() {
    Rig rig;
    rig.begin();
    enableAndFeed(rig, 1);
    setMillis(40);
    const DeviceReceipt manual = rig.api.requestMove(move(1));
    CHECK(manual.operationId != 0);
    rig.motor.noteRawTransmission(1); // raw CAN bypass invalidates software evidence
    CHECK(rig.api.readOperation(manual.operationId).state ==
          DeviceOperationState::Invalidated);
    CHECK(rig.state(1, 40).manualMove == DeviceMoveStage::None);

    // Reinitializing the same instance starts a fresh session. The counter is
    // monotonic in this process, so an old handle never aliases a new request.
    CHECK(rig.api.begin(4, 5, 500000));
    CHECK(rig.api.readOperation(manual.operationId).state ==
          DeviceOperationState::Expired);
    CHECK(rig.api.readOperation(0).state == DeviceOperationState::Unknown);
}

static void test_raw_manual_commands_share_the_operation_contract() {
    {
        Rig rig;
        rig.begin();
        enableAndFeed(rig, 1);
        const uint8_t rawMove[] =
            {1, 0xCD, 0, 0, 60, 0, 60, 0, 100, 0, 0, 0, 100, 2, 0, 3, 32, 0x6B};
        setMillis(40);
        const DeviceReceipt sent = rig.api.requestRawCommand(rawMove, sizeof(rawMove));
        CHECK(sent.accepted());
        CHECK(sent.operationId != 0);
        CHECK(sent.runId == 0);
        CHECK(rig.api.readOperation(sent.operationId).kind == DeviceOperationKind::Move);
        CHECK(rig.api.readOperation(sent.operationId).state == DeviceOperationState::Running);
        const uint8_t read[] = {1, 0x36, 0x6B};
        CHECK(rig.api.requestRawCommand(read, sizeof(read)).operationId == 0);
        const uint8_t stop[] = {1, 0xFE, 0x98, 0, 0x6B};
        CHECK(rig.api.requestRawCommand(stop, sizeof(stop)).operationId == 0);
        CHECK(rig.api.readOperation(sent.operationId).state ==
              DeviceOperationState::Cancelled);
    }
    {
        Rig rig;
        rig.begin();
        enableAndFeed(rig, 1);
        const uint8_t rawDirect[] =
            {1, 0xFB, 0, 0x01, 0x2C, 0, 0, 0, 100, 1, 0, 0x6B};
        setMillis(40);
        const DeviceReceipt sent = rig.api.requestRawCommand(rawDirect, sizeof(rawDirect));
        CHECK(sent.accepted());
        CHECK(sent.operationId != 0);
        CHECK(rig.api.readOperation(sent.operationId).kind ==
              DeviceOperationKind::DirectPosition);
        CHECK(rig.api.readOperation(sent.operationId).state == DeviceOperationState::Running);
    }
    {
        Rig rig;
        rig.begin();
        enableAndFeed(rig, 1);
        const uint8_t rawHome[] = {1, 0x9A, 0, 0, 0x6B};
        setMillis(40);
        const DeviceReceipt sent = rig.api.requestRawCommand(rawHome, sizeof(rawHome));
        CHECK(sent.accepted());
        CHECK(sent.operationId != 0);
        CHECK(rig.api.readOperation(sent.operationId).kind == DeviceOperationKind::Home);
        CHECK(rig.api.readOperation(sent.operationId).state == DeviceOperationState::Running);
    }
    {
        Rig rig;
        rig.begin();
        rig.motor.watch(1);
        injectRx(flags(1, false));
        injectRx(makePosition(1, 0));
        injectRx(makeVelocity(1, 0));
        rig.poll(20);
        const uint8_t configure[] = {1, 0x46, 0x69, 0, 1, 0x6B};
        const DeviceReceipt sent = rig.api.requestRawCommand(configure, sizeof(configure));
        CHECK(sent.accepted());
        CHECK(sent.operationId == 0);
    }
}

static void test_rejected_ack_and_bus_off_have_operation_faults() {
    {
        Rig rig;
        rig.begin();
        enableAndFeed(rig, 1);
        setMillis(40);
        const DeviceReceipt moveRun = rig.api.requestMove(move(1));
        CHECK(moveRun.operationId != 0);
        injectRx(makeAck(1, kFrameMove, 0xEE));
        rig.poll(60);
        const DeviceOperationResult failed = rig.api.readOperation(moveRun.operationId);
        CHECK(failed.state == DeviceOperationState::Failed);
        CHECK(failed.fault == DeviceOperationFault::AckRejected);
        CHECK(!failed.protocolAck);
        CHECK(!failed.reached);
    }
    {
        Rig rig;
        rig.begin();
        enableAndFeed(rig, 1);
        setMillis(40);
        const DeviceReceipt homeRun = rig.api.requestHome(1, 0);
        CHECK(homeRun.operationId != 0);
        injectRx(makeAck(1, 0x9A, 0xEE));
        rig.poll(60);
        const DeviceOperationResult failed = rig.api.readOperation(homeRun.operationId);
        CHECK(failed.state == DeviceOperationState::Failed);
        CHECK(failed.fault == DeviceOperationFault::AckRejected);
        CHECK(!failed.protocolAck);
        CHECK(!failed.reached);
    }
    {
        Rig rig;
        rig.begin();
        enableAndFeed(rig, 1);
        setMillis(40);
        const DeviceReceipt moveRun = rig.api.requestMove(move(1));
        CHECK(moveRun.operationId != 0);
        busState = CanControllerState::BusOff;
        rig.poll(60);
        const DeviceOperationResult failed = rig.api.readOperation(moveRun.operationId);
        CHECK(failed.state == DeviceOperationState::Failed);
        CHECK(failed.fault == DeviceOperationFault::BusOff);
        CHECK(!failed.reached);
    }
    {
        Rig rig;
        rig.begin();
        enableAndFeed(rig, 1);
        setMillis(40);
        const DeviceReceipt homeRun = rig.api.requestHome(1, 0);
        CHECK(homeRun.operationId != 0);
        rig.motor.watch(2); // UI selection changes, but Home still owns axis 1.
        busState = CanControllerState::BusOff;
        rig.poll(60);
        const DeviceOperationResult failed = rig.api.readOperation(homeRun.operationId);
        CHECK(failed.state == DeviceOperationState::Failed);
        CHECK(failed.fault == DeviceOperationFault::BusOff);
        CHECK(rig.state(1, 60).motor.stopPending);
    }
}

static void test_clear_control_state_keeps_fault_and_stop_evidence() {
    Rig rig;
    rig.begin();
    enableAndFeed(rig, 1);
    setMillis(40);
    const DeviceReceipt started = rig.api.requestMove(move(1));
    CHECK(started.accepted());
    injectRx(makeAck(1, kFrameMove, 0xEE));
    rig.poll(60);
    CHECK(rig.api.readOperation(started.operationId).fault ==
          DeviceOperationFault::AckRejected);
    CHECK(rig.state(1, 60).motor.stopPending);

    // The public reset cancels software ownership and requests a stop, but it
    // cannot erase the fault or turn that stop request into physical evidence.
    setMillis(70);
    const DeviceReceipt cleared = rig.api.clearControlState();
    CHECK(cleared.accepted());
    CHECK(cleared.operationId == 0);
    const DeviceSnapshot afterClear = rig.state(1, 70);
    CHECK(afterClear.fault);
    CHECK(afterClear.motor.fault);
    CHECK(std::strcmp(afterClear.faultTag, "ack_rejected") == 0);
    CHECK(afterClear.motor.stopPending);
    const std::string status = rig.motor.statusJson(1).str();
    CHECK(status.find("\"fault\":\"ack_rejected\"") != std::string::npos);

    injectRx(makeAck(1, kFrameStop, 0x02));
    rig.poll(80);
    CHECK(rig.state(1, 80).motor.stopPending); // FE receipt is not stop proof.
    injectRx(makePosition(1, 0));
    rig.poll(90);
    CHECK(rig.state(1, 90).motor.stopPending); // Both measurements are needed.
    injectRx(makeVelocity(1, 0));
    rig.poll(100);
    const DeviceSnapshot stopped = rig.state(1, 100);
    CHECK(!stopped.motor.stopPending);
    CHECK(stopped.fault); // Stopping does not silently recover the failed move.
    CHECK(std::strcmp(stopped.faultTag, "ack_rejected") == 0);
}

static void test_broadcast_disable_records_cancellation() {
    Rig rig;
    rig.begin();
    enableAndFeed(rig, 1);
    setMillis(40);
    const DeviceReceipt moveRun = rig.api.requestMove(move(1));
    CHECK(moveRun.operationId != 0);
    const DeviceReceipt disabled = rig.api.requestBroadcastEnable(false);
    CHECK(disabled.operationId == 0);
    CHECK(rig.api.readOperation(moveRun.operationId).state ==
          DeviceOperationState::Cancelled);
    CHECK(rig.state(1, 40).manualMove == DeviceMoveStage::None);
}

static void test_unverified_api_reports_submission_without_completion() {
    Rig rig;
    rig.begin();
    CHECK(rig.api.requestMove(move(1)).admission == DeviceAdmission::Busy);
    rig.api.setUnverifiedMode(true);
    CHECK(rig.api.unverifiedMode());
    DeviceSnapshot state = rig.state(1, 0);
    CHECK(state.unverifiedMode);
    CHECK(!state.unverifiedMotionOutstanding);

    const DeviceReceipt sent = rig.api.requestMove(move(1));
    CHECK(sent.admission == DeviceAdmission::Accepted);
    CHECK(sent.code == 202);
    CHECK(sent.operationId == 0); // No supervised result or reached claim.
    CHECK(rig.api.readSnapshot(1).manualMove == DeviceMoveStage::None);
    CHECK(rig.api.unverifiedMotionOutstanding());

    const uint8_t unknown[] = {1, 0x7E, 0x01, 0x6B};
    const size_t beforeRaw = capturedTX.size();
    CHECK(rig.api.requestRawCommand(unknown, sizeof(unknown)).accepted());
    CHECK(capturedTX.size() == beforeRaw + 1);
    CHECK(capturedTX.back().data[0] == 0x7E);
    const uint8_t bad[] = {1, 0x7E, 0x01, 0};
    CHECK(rig.api.requestRawCommand(bad, sizeof(bad)).admission == DeviceAdmission::Invalid);

    const size_t beforeStop = capturedTX.size();
    CHECK(rig.api.requestStopAll().accepted());
    CHECK(capturedTX.size() == beforeStop + 1);
    CHECK(capturedTX.back().data[0] == kFrameStop);
    CHECK(rig.api.unverifiedMotionOutstanding()); // FE does not prove stillness.
    rig.api.setUnverifiedMode(false);
    CHECK(!rig.api.unverifiedMode());
    CHECK(rig.api.requestMove(move(1)).admission == DeviceAdmission::Busy);
}

static void test_unverified_stop_disable_and_cancel_are_single_wire_actions() {
    const char waiting[] = "wait 1000\nenable 1\n";
    for (int path = 0; path < 8; ++path) {
        Rig rig;
        rig.begin();
        rig.api.setUnverifiedMode(true);
        CHECK(rig.api.startProgram(waiting, sizeof(waiting) - 1,
                                   1, rig.rotation, 0).accepted());
        rig.poll(10);
        CHECK(rig.state(1, 10).program == DeviceProgramStage::Running);
        const uint8_t malformed[] = {1, 0xFE, 0x98, 0, 0};
        CHECK(rig.api.requestRawCommand(malformed, sizeof(malformed)).code == 400);
        CHECK(rig.state(1, 10).program == DeviceProgramStage::Running);
        const size_t before = capturedTX.size();
        DeviceReceipt result = {DeviceAdmission::Failed, 0, "", 0, 0};
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
            case 7: result = rig.api.cancelProgram("operator_cancel"); break;
        }
        CHECK(result.accepted());
        CHECK(capturedTX.size() == before + 1);
        CHECK(rig.state(1, 10).program == DeviceProgramStage::Cancelled);
        for (uint32_t t = 20; t <= 1200; t += 20) rig.poll(t);
        CHECK(countWireEnableOn(1) == 0); // No later queue step restarts motion.
        CHECK(capturedTX.size() == before + 1); // No hidden abort/query/stop.
    }
}

static void test_switching_mode_mid_program_retains_uncertainty_for_ota() {
    Rig rig;
    rig.begin();
    const char program[] = "move 1 10 deg\nwait 1000\n";
    // The regular queue is a direct sender; no CAN reply is needed to start.
    CHECK(rig.api.startProgram(program, sizeof(program) - 1,
                               1, rig.rotation, 0).accepted());
    rig.poll(10);
    bool sawMove = false;
    for (const CanRawFrame& frame : capturedTX) {
        if (x42sCanAddress(frame.identifier) == 1 && frame.length &&
            frame.data[0] == kFrameMove) sawMove = true;
    }
    CHECK(sawMove);
    const size_t beforeToggle = capturedTX.size();
    rig.api.setUnverifiedMode(true);
    CHECK(capturedTX.size() == beforeToggle);
    CHECK(rig.api.unverifiedMotionOutstanding());
    rig.api.setUnverifiedMode(false);
    CHECK(capturedTX.size() == beforeToggle);
    CHECK(rig.api.unverifiedMotionOutstanding());
}

static void test_unverified_sync_batch_blocks_cross_entry_frames_until_ff_or_stop() {
    const char syncProgram[] =
        "sync begin\nmove 1 90 deg 100 200 200 800\n"
        "move 2 90 deg 100 200 200 800\nsync end\n";
    for (int stopPath = 0; stopPath < 6; ++stopPath) {
        Rig rig;
        rig.begin();
        rig.api.setUnverifiedMode(true);
        CHECK(rig.api.startProgram(syncProgram, sizeof(syncProgram) - 1,
                                   1, rig.rotation, 0).accepted());
        rig.poll(10); // Begin the batch.
        rig.poll(12); // First cached CD; FF must still be held.
        CHECK(rig.queue.unverifiedSyncInFlight());
        bool sawCache = false, sawTrigger = false;
        for (const CanRawFrame& frame : capturedTX) {
            if (frame.length && frame.data[0] == kFrameMove) sawCache = true;
            if (frame.length && frame.data[0] == 0xFF) sawTrigger = true;
        }
        CHECK(sawCache && !sawTrigger);

        const size_t beforeBlocked = capturedTX.size();
        const DeviceReceipt moveBlocked = rig.api.requestMove(move(3));
        CHECK(moveBlocked.code == 409);
        CHECK(std::strcmp(moveBlocked.message, "sync_batch_active") == 0);
        CHECK(rig.api.requestEnable(3, true).code == 409);
        CHECK(rig.api.requestBroadcastEnable(true).code == 409);
        CHECK(rig.api.requestHome(3, 0).code == 409);
        DirectPositionRequest direct;
        direct.id = 3;
        CHECK(rig.api.requestDirectPosition(direct).code == 409);
        const uint8_t raw[] = {3, 0x7E, 0x55, 0x6B};
        CHECK(rig.api.requestRawCommand(raw, sizeof(raw)).code == 409);
        CHECK(rig.api.startProgram("wait 1\n", 7, 1, rig.rotation, 12).code == 409);
        CHECK(rig.api.startDemo(QueueProgram{}, 12).code == 409);
        CHECK(!rig.api.requestDemoMarker(3));
        CHECK(capturedTX.size() == beforeBlocked);

        if (stopPath >= 4) rig.api.setUnverifiedMode(false);
        const size_t beforeStop = capturedTX.size();
        DeviceReceipt stopped = {DeviceAdmission::Failed, 0, "", 0, 0};
        if (stopPath == 0 || stopPath == 4) stopped = rig.api.requestStopAll();
        else if (stopPath == 1 || stopPath == 5) {
            const uint8_t fe[] = {0, 0xFE, 0x98, 0, 0x6B};
            stopped = rig.api.requestRawCommand(fe, sizeof(fe));
        } else if (stopPath == 2) {
            const uint8_t interrupt[] = {0, 0x9C, 0x48, 0x6B};
            stopped = rig.api.requestRawCommand(interrupt, sizeof(interrupt));
        } else stopped = rig.api.requestEnable(3, false);
        CHECK(stopped.accepted());
        CHECK(capturedTX.size() == beforeStop + 1);
        CHECK(!rig.queue.unverifiedSyncInFlight());
        for (uint32_t t = 14; t <= 100; t += 2) rig.poll(t);
        CHECK(rig.state(1, 100).program == DeviceProgramStage::Cancelled);
        bool lateTrigger = false;
        for (const CanRawFrame& frame : capturedTX)
            if (frame.length && frame.data[0] == 0xFF) lateTrigger = true;
        CHECK(!lateTrigger);
    }

    Rig completed;
    completed.begin();
    completed.api.setUnverifiedMode(true);
    CHECK(completed.api.startProgram(syncProgram, sizeof(syncProgram) - 1,
                                     1, completed.rotation, 0).accepted());
    for (uint32_t t = 10; t <= 30; t += 2) completed.poll(t);
    CHECK(!completed.queue.unverifiedSyncInFlight());
    CHECK(completed.api.requestHome(3, 0).accepted()); // Gate ends after FF.
}

int main() {
    test_receipt_ack_and_observation_are_distinct();
    test_manual_move_reached_needs_new_evidence();
    test_manual_failure_json_keeps_diagnostic_snapshot();
    test_home_response_modes_need_fresh_evidence();
    test_home_no_motion_is_distinct_from_reached();
    test_manual_move_and_home_share_one_slot();
    test_stop_requires_both_post_request_stationary_samples();
    test_queue_takeover_ignores_late_manual_home_reply();
    test_program_done_is_not_motor_reached();
    test_bad_program_is_atomic_and_cancel_prevents_later_step();
    test_stop_is_a_request_until_feedback_confirms();
    test_all_public_stop_and_disable_paths_preempt_program();
    test_invalid_target_never_preempts_program();
    test_snapshot_reads_create_no_delayed_query_demand();
    test_manual_operation_ids_and_results();
    test_program_and_demo_receipts_do_not_claim_manual_operations();
    test_raw_invalidation_and_session_expiry();
    test_raw_manual_commands_share_the_operation_contract();
    test_rejected_ack_and_bus_off_have_operation_faults();
    test_clear_control_state_keeps_fault_and_stop_evidence();
    test_broadcast_disable_records_cancellation();
    test_unverified_api_reports_submission_without_completion();
    test_unverified_stop_disable_and_cancel_are_single_wire_actions();
    test_switching_mode_mid_program_retains_uncertainty_for_ota();
    test_unverified_sync_batch_blocks_cross_entry_frames_until_ff_or_stop();
    if (failures) std::printf("device-api: %d/%d checks failed\n", failures, checks);
    else std::printf("device-api: %d checks passed\n", checks);
    return failures ? 1 : 0;
}
