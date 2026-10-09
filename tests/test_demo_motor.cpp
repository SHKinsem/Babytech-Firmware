#include "DemoMotorExecutor.h"
#include "fake_x42s.h"
#include <cassert>
#include <iostream>
#include <cstring>
#include <limits>
using namespace motion;
using namespace fakecan;
struct Rotation : QueueRotationSource {
    bool rotationMm(uint8_t, double&) const override { return false; }
};
void feedback(MotorControl& motor, uint32_t now, int32_t position) {
    setMillis(now); injectRx(makePosition(1, position)); injectRx(makeVelocity(1, 0)); motor.poll();
    const uint8_t flags[] = {0x3A, 0x83, 0x6B}; injectRx(makeFrame(1,flags,3)); motor.poll();
}
static void test_demo_polling_does_not_starve_queue_await() {
    fakeReset(); MotorControl motor; CommandQueue queue(motor);
    struct Millimetres : QueueRotationSource {
        bool rotationMm(uint8_t, double& mm) const override { mm=10; return true; }
    } rotation;
    assert(motor.begin(4,5,500000)); motor.setAutoQueriesEnabled(false);
    CanQueryScheduler::Config budget; budget.queriesPerSecond=30; budget.gapMs=33;
    assert(motor.queries().configure(budget));
    DemoConfig config;
    for (uint8_t id=1;id<=5;++id) config.axes.push_back({id,10,10,false});
    DemoMotorExecutor executor(motor,queue); executor.configure(config);
    const char* program="enable 1\nmove 1 -5 mm 300 300 300 200 await\nhome 1 2\n";
    assert(queue.start(program,std::strlen(program),1,rotation,0).code==202);
    bool moved=false, targetRead=false, homeSent=false;
    size_t seen=0;
    for (uint32_t now=1;now<5000;++now) {
        setMillis(now); motor.poll(false); executor.poll(now); queue.poll(now);
        motor.dispatchQueries();
        while (seen<capturedTX.size()) {
            const auto frame=capturedTX[seen++];
            const uint8_t id=uint8_t(frame.identifier>>8), op=frame.data[0];
            if (op==0xCD && (frame.identifier&0xFF)==0) {
                moved=true; injectRx(makeAck(id,op,2));
            } else if (op==0x9A) {
                assert(frame.length==4 && frame.data[1]==2 && frame.data[2]==0);
                homeSent=true;
            } else if (op==0xF3) injectRx(makeAck(id,op,2));
            else if (op==0x36) injectRx(makePosition(id,id==1 && moved ? -1800 : 0));
            else if (op==0x35) injectRx(makeVelocity(id,0));
            else if (op==0x33) { targetRead=true; injectRx(makeTarget(id,-1800)); }
            else if (op==0x3A || op==0x3B) {
                const uint8_t reply[]={op,3,0x6B}; injectRx(makeFrame(id,reply,3));
            }
        }
    }
    if (!targetRead || !homeSent || queue.state()!=QueueState::Done)
        std::cerr << "demo/await: targetRead=" << targetRead << " homeSent=" << homeSent
                  << " " << queue.statusJson().str() << '\n';
    assert(targetRead && homeSent && queue.state()==QueueState::Done);
    // Demo polling must also respect the sync owner's exclusive window.
    motor.queries().exclusiveSync(true);
    const size_t before=capturedTX.size();
    for (uint32_t now=5000;now<5500;++now) {
        setMillis(now); motor.poll(false); executor.poll(now); motor.dispatchQueries();
    }
    assert(capturedTX.size()==before);
    std::cout << "PASS demo polling shares budget: move await reaches home, sync excludes demo queries\n";
}

static void test_stop_proof_is_not_product_readiness() {
    fakeReset(); MotorControl motor; CommandQueue queue(motor);
    assert(motor.begin(4, 5, 500000));
    auto budget = motor.queries().config();
    budget.queriesPerSecond = 100; budget.gapMs = 2; budget.timeoutMs = 20;
    assert(motor.queries().configure(budget)); // Single-axis 600 ms lower bound.
    DemoConfig config; config.axes.push_back({1, 10, 0, true});
    DemoMotorExecutor executor(motor, queue); executor.configure(config);
    assert(!executor.stopConfirmed());
    setMillis(1); assert(motor.enable(1, true).code == 202);
    injectRx(makeAck(1, 0xF3, 2)); motor.poll();
    feedback(motor, 10, 0);
    assert(motor.hasActiveMotion()); // Includes enabled, stationary holding.
    setMillis(20); assert(executor.stop());
    assert(!executor.stopConfirmed()); // Pre-stop samples cannot confirm Stop.
    feedback(motor, 21, 0);
    assert(motor.hasActiveMotion() && !motor.operationBusy());
    assert(executor.stopConfirmed());
    config.axes[0].rotationMm = 10;
    assert(executor.configurationValid() && executor.stopConfirmed());
    // Keep the driver fault visible, but it does not invalidate fresh low-speed
    // measurements or require a blanket debug lock once the axis stopped.
    injectRx(makeAck(1, 0xF3, 0xE2)); setMillis(22); motor.poll();
    feedback(motor, 23, 0);
    assert(!executor.healthy() && executor.stopConfirmed());
    setMillis(24); injectRx(makeVelocity(1, 6)); motor.poll();
    assert(!executor.stopConfirmed());
    feedback(motor, 25, 0); assert(executor.stopConfirmed());
    setMillis(2025); assert(!executor.stopConfirmed());
    setMillis(2030); assert(executor.stop());
    feedback(motor, 2030, 0);
    setMillis(2031); setMillisReadStep(1);
    assert(!executor.evidence(1).fresh); // Clock ticks cannot renew a Stop-time sample.
    setMillisReadStep(0);
    feedback(motor, 2032, 0);
    setMillis(2040); setMillisReadStep(1);
    assert(executor.evidence(1).fresh);
    setMillisReadStep(0);
    // Every configured axis needs evidence; a valid axis is not whole-machine proof.
    config.axes.push_back({2, 10, 0, false});
    executor.configure(config); setMillis(2030); assert(executor.stop());
    feedback(motor, 2031, 0); assert(!executor.stopConfirmed());
    injectRx(makePosition(2, 100)); injectRx(makeVelocity(2, 0));
    const uint8_t flags[] = {0x3A, 0x83, 0x6B}; injectRx(makeFrame(2, flags, 3)); motor.poll();
    assert(executor.stopConfirmed());
    config.axes.clear(); executor.configure(config); assert(!executor.stopConfirmed());
    std::cout << "PASS stop proof: post-stop/all-axis/fresh/velocity, enabled hold and non-Ready faults\n";
}

static void test_boot_stop_inventory_without_executable_config() {
    fakeReset(); MotorControl motor; CommandQueue queue(motor);
    assert(motor.begin(4, 5, 500000)); motor.setAutoQueriesEnabled(false);
    DemoMotorExecutor executor(motor, queue);
    {
        DemoConfig rejected; rejected.axes.push_back({1, 10, 10, true});
        executor.configureStopAxes(rejected); // Axis inventory alone is not an accepted script.
    } // No pointer to the rejected temporary may remain.
    setMillis(20); assert(executor.stop());
    setMillis(120); executor.poll(120); motor.dispatchQueries();
    assert(!capturedTX.empty() && capturedTX.back().data[0] == 0x36);
    assert(!executor.stopConfirmed());
    feedback(motor, 121, 0);
    assert(executor.stopConfirmed());
    DemoScript script; std::array<int32_t, 256> zeros{};
    assert(!executor.start(script, true, zeros, 122)); // Monitoring does not authorize motion.
    std::cout << "PASS boot stop inventory survives unapplied configuration without enabling scripts\n";
}

static void test_default_five_axis_flow_stop() {
    fakeReset(); MotorControl motor; CommandQueue queue(motor);
    assert(motor.begin(4, 5, 500000)); motor.setAutoQueriesEnabled(false);
    DemoMotorExecutor executor(motor, queue); DemoFlowController flow(executor);
    DemoConfig config;
    for (uint8_t id = 1; id <= 5; ++id) config.axes.push_back({id, 10, 0, false});
    assert(flow.apply(config)); executor.configure(config);
    setMillis(20); flow.stop(20);
    for (uint8_t id = 1; id <= 5; ++id) {
        setMillis(20 + 300 * id);
        injectRx(makePosition(id, 0)); injectRx(makeVelocity(id, 0));
        const uint8_t flags[] = {0x3A, 1, 0x6B}; injectRx(makeFrame(id, flags, 3)); motor.poll(false);
        flow.tick(millis());
        if (id < 5) assert(flow.busy());
    }
    assert(!flow.busy() && std::strcmp(flow.reason(), "stopped") == 0);
    assert(executor.stopConfirmed() && !motor.snapshot(1).positionValid);
    assert(executor.evidence(1).fresh && executor.stopEvidence(1).fresh);
    assert(flow.stationary()); // Product readiness shares the frozen post-Stop budget.
    assert(motor.queries().config().queriesPerSecond == 10);
    setMillis(6000); assert(!executor.stopConfirmed());
    std::cout << "PASS actual five-axis Flow Stop and readiness use bounded proof, manual feedback stays 600 ms\n";
}

static void test_default_five_axis_readiness() {
    fakeReset(); MotorControl motor; CommandQueue queue(motor);
    assert(motor.begin(4, 5, 500000)); motor.setAutoQueriesEnabled(false);
    DemoMotorExecutor executor(motor, queue); DemoFlowController flow(executor);
    DemoConfig config; config.configured = true;
    config.initialization.commands = {"wait 1"};
    for (uint8_t id = 1; id <= 5; ++id) config.axes.push_back({id, 10, 0, false});
    assert(flow.apply(config)); executor.configure(config);
    assert(flow.initialize(0));
    size_t seen = 0;
    unsigned missingId = 0, movingId = 0;
    const auto runUntil = [&](uint32_t end) {
        while (millis() < end) {
            const auto now = millis() + 1;
            setMillis(now); motor.poll(false); executor.poll(now); flow.tick(now);
            queue.poll(now); motor.dispatchQueries();
            while (seen < capturedTX.size()) {
                const auto frame = capturedTX[seen++];
                const auto id = uint8_t(frame.identifier >> 8);
                if (id == missingId) continue;
                const auto op = frame.data[0];
                if (op == 0x36) injectRx(makePosition(id, 0));
                else if (op == 0x35) injectRx(makeVelocity(id, id == movingId ? 30 : 0));
                else if (op == 0x3A || op == 0x3B) {
                    const uint8_t bytes[] = {op, 1, 0x6B}; injectRx(makeFrame(id, bytes, 3));
                }
            }
        }
    };
    runUntil(4000);
    assert(flow.referenceValid() && flow.startEnabled() && flow.stationary());
    bool ordinaryExpired = false;
    for (uint8_t id = 1; id <= 5; ++id) {
        assert(executor.evidence(id).fresh);
        ordinaryExpired = ordinaryExpired || !motor.snapshot(id).positionValid;
    }
    assert(ordinaryExpired && motor.queries().config().queriesPerSecond == 10);
    for (const auto& frame : capturedTX) assert(frame.length == 2); // No fabricated movement.
    missingId = 5; runUntil(11000); assert(!flow.stationary());
    missingId = 0; runUntil(15000); assert(flow.stationary());
    movingId = 3; runUntil(19000); assert(!flow.stationary());
    movingId = 0; runUntil(23000); assert(flow.stationary());
    const uint8_t fault[] = {0x3A, 9, 0x6B};
    injectRx(makeFrame(4, fault, 3)); motor.poll(false); assert(!flow.stationary());
    runUntil(24000);
    flow.stop(millis()); assert(flow.busy() && !executor.stopConfirmed());
    runUntil(26900);
    assert(!flow.busy() && std::strcmp(flow.reason(), "stopped") == 0 && executor.stopConfirmed());
    setMillis(32000); assert(!executor.evidence(1).fresh);
    std::cout << "PASS five-axis readiness at default query budget: missing, moving, faulty and expired feedback refused\n";
}

static void test_readiness_budget_does_not_revive_samples() {
    fakeReset(); MotorControl motor; CommandQueue queue(motor);
    assert(motor.begin(4, 5, 500000)); motor.setAutoQueriesEnabled(false);
    auto budget = motor.queries().config();
    budget.queriesPerSecond = 100; budget.gapMs = 2; budget.timeoutMs = 20;
    assert(motor.queries().configure(budget));
    DemoConfig config; config.axes.push_back({1, 10, 0, false});
    DemoMotorExecutor executor(motor, queue); executor.configure(config);
    assert(motor.demoEvidenceWindow() == 600);
    feedback(motor, 10, 0); assert(executor.evidence(1).fresh);
    setMillis(611); assert(!executor.evidence(1).fresh);
    budget.queriesPerSecond = 1; budget.gapMs = 1000; budget.cooldownMs = 1000;
    assert(motor.queries().configure(budget));
    assert(motor.demoEvidenceWindow() == 5000 && !executor.evidence(1).fresh);
    feedback(motor, 612, 0); assert(executor.evidence(1).fresh);
    setMillis(1220); assert(!motor.snapshot(1).positionValid && executor.evidence(1).fresh);
    setMillis(6000); assert(!executor.evidence(1).fresh);
    std::cout << "PASS readiness budget changes require new samples; manual snapshot remains 600 ms\n";
}

static void test_product_await_keeps_completion_proof() {
    fakeReset(); MotorControl motor; CommandQueue queue(motor);
    assert(motor.begin(4, 5, 500000)); motor.setAutoQueriesEnabled(false);
    DemoConfig config;
    for (uint8_t id = 1; id <= 5; ++id) config.axes.push_back({id, 10, 0, false});
    DemoMotorExecutor executor(motor, queue); executor.configure(config);
    const char* text = "move 1 10 deg 10 20 20 100 await";
    Rotation rotation; QueueProgram program; QueueError error;
    assert(parseQueueProgram(text, std::strlen(text), rotation, program, error));
    feedback(motor, 10, 0);
    assert(queue.startDemo(program, 20).code == 202);
    setMillis(20); queue.poll(20);
    setMillis(30); injectRx(makeAck(1, 0xCD, 2)); motor.poll(false); queue.poll(30);
    assert(queue.active()); // Pre-acceptance samples cannot complete the move.
    setMillis(40); injectRx(makeTarget(1, 999)); motor.poll(false);
    feedback(motor, 40, 999); queue.poll(40); assert(queue.active());
    feedback(motor, 50, 999); queue.poll(50); assert(queue.active()); // Wrong driver target.
    setMillis(60); injectRx(makeTarget(1, 100)); injectRx(makePosition(1, 100));
    injectRx(makeVelocity(1, 30)); motor.poll(false); queue.poll(60); assert(queue.active());
    feedback(motor, 70, 100); queue.poll(70);
    for (uint32_t now : {71u, 1200u, 1700u}) {
        setMillis(now); queue.poll(now); assert(queue.active()); // One pair is not two.
    }
    feedback(motor, 1800, 100); queue.poll(1800); queue.poll(1810);
    assert(queue.state() == QueueState::Done);

    // A later manual run must not inherit the product's larger validity window.
    assert(queue.start(text, std::strlen(text), 1, rotation, 1820).code == 202);
    setMillis(1820); queue.poll(1820);
    setMillis(1830); injectRx(makeAck(1, 0xCD, 2)); motor.poll(false); queue.poll(1830);
    setMillis(1840); injectRx(makeTarget(1, 200)); motor.poll(false);
    feedback(motor, 1850, 200); queue.poll(1850);
    setMillis(2851); queue.poll(2851); assert(queue.active()); // Manual 1 s expiry resets count.
    feedback(motor, 3550, 200); queue.poll(3550); assert(queue.active());
    feedback(motor, 3560, 200); queue.poll(3560); queue.poll(3570);
    assert(queue.state() == QueueState::Done);

    // Product feedback expiry still resets the consecutive stationary proof.
    feedback(motor, 4000, 200);
    assert(queue.startDemo(program, 4010).code == 202);
    setMillis(4010); queue.poll(4010);
    setMillis(4020); injectRx(makeAck(1, 0xCD, 2)); motor.poll(false); queue.poll(4020);
    setMillis(4030); injectRx(makeTarget(1, 300)); motor.poll(false);
    feedback(motor, 4040, 300); queue.poll(4040);
    setMillis(9041); queue.poll(9041); assert(queue.active());
    feedback(motor, 9050, 300); queue.poll(9050); assert(queue.active());
    feedback(motor, 9060, 300); queue.poll(9060); queue.poll(9070);
    assert(queue.state() == QueueState::Done);
    std::cout << "PASS product await preserves target, post-proof, speed, two new pairs and expiry; manual stays 1 s\n";
}

static void test_product_mm_uses_script_configuration() {
    fakeReset(); MotorControl motor; CommandQueue queue(motor);
    assert(motor.begin(4, 5, 500000)); motor.setAutoQueriesEnabled(false);
    DemoConfig config; config.axes.push_back({1, 10, 2, true});
    DemoMotorExecutor executor(motor, queue); executor.configure(config);
    const char* command = "move 1 10 mm 10 20 20 100 await";
    Rotation missing;
    QueueProgram manual; QueueError error;
    assert(!parseQueueProgram(command, std::strlen(command), missing, manual, error));
    assert(std::strcmp(error.message, "rotation_distance_missing") == 0);
    struct DifferentRotation : QueueRotationSource {
        bool rotationMm(uint8_t, double& value) const override { value = 8; return true; }
    } different;
    assert(parseQueueProgram(command, std::strlen(command), different, manual, error));
    assert(manual.steps[0].distanceTenths == 4500); // Manual table: 10 mm / 8 mm per turn.
    DemoScript script; script.commands = {command};
    std::array<int32_t, 256> zeros{};
    feedback(motor, 10, 0);
    assert(executor.start(script, false, zeros, 10));
    setMillis(20); queue.poll(20);
    std::vector<uint8_t> logical{0xCD};
    for (const auto& frame : capturedTX) if (frame.data[0] == 0xCD)
        for (uint8_t i = 1; i < frame.length; ++i) logical.push_back(frame.data[i]);
    assert(logical.size() == 17 && logical[1] == 0 && logical[12] == 2);
    const uint32_t magnitude = (uint32_t(logical[8]) << 24) | (uint32_t(logical[9]) << 16) |
                              (uint32_t(logical[10]) << 8) | logical[11];
    assert(magnitude == 18000); // Product JSON: 10 mm / 2 mm per turn = 1800 degrees.
    assert(executor.stop());
    for (double invalid : {0.0, -2.0, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
        config.axes[0].rotationMm = invalid;
        const auto before = capturedTX.size();
        assert(!executor.start(script, false, zeros, 30));
        assert(capturedTX.size() == before && !queue.active());
    }
    std::cout << "PASS product mm conversion uses JSON, manual table remains independent, invalid mm emits no motion\n";
}

int main() {
    test_default_five_axis_readiness();
    test_readiness_budget_does_not_revive_samples();
    test_product_await_keeps_completion_proof();
    test_product_mm_uses_script_configuration();
    test_demo_polling_does_not_starve_queue_await();
    test_stop_proof_is_not_product_readiness();
    test_default_five_axis_flow_stop();
    test_boot_stop_inventory_without_executable_config();
    fakeReset(); MotorControl motor; CommandQueue queue(motor);
    assert(motor.begin(4,5,500000));
    QueueProgram unconfiguredSync;
    unconfiguredSync.count = 4;
    unconfiguredSync.steps[0].action = QueueAction::SyncBegin;
    unconfiguredSync.steps[0].groupSize = 2;
    unconfiguredSync.steps[1].action = QueueAction::Move;
    unconfiguredSync.steps[1].id = 1;
    unconfiguredSync.steps[2].action = QueueAction::Move;
    unconfiguredSync.steps[2].id = 2;
    unconfiguredSync.steps[3].action = QueueAction::SyncEnd;
    const auto sentBeforeSync = capturedTX.size();
    const auto syncResult = queue.startDemo(unconfiguredSync, 0);
    assert(syncResult.code == 400 && !queue.active() && capturedTX.size() == sentBeforeSync);
    DemoConfig c; c.axes.push_back({1,10,0,true});
    DemoMotorExecutor executor(motor,queue); executor.configure(c);
    c.axes[0].rotationMm = 10;
    assert(executor.configurationValid() && executor.available());
    DemoFlowController flow(executor);
    assert(flow.apply(c)); // Product configuration does not depend on the manual rotation table.
    c.axes[0].rotationMm = 0;
    assert(executor.configurationValid());
    std::array<int32_t,256> zeros{}; zeros[1]=-123;
    feedback(motor,10,500); assert(executor.evidence(1).fresh);
    DemoScript script; script.commands={"zero 1 10 20 20 100"};
    assert(executor.start(script,false,zeros,10));
    assert(executor.evidence(1).fresh); // demo start must not clear received position
    setMillis(20); queue.poll(20);
    std::vector<uint8_t> logical{0xCD};
    for (const auto& frame : capturedTX) if (frame.data[0] == 0xCD)
        for (uint8_t i=1;i<frame.length;++i) logical.push_back(frame.data[i]);
    assert(logical.size() == 17 && logical[12] == 1);
    assert(logical[1] == 1 && logical[11] == 123); // negative recorded zero
    assert(executor.stop()); assert(!executor.evidence(1).fresh);
    feedback(motor,21,500); assert(executor.evidence(1).fresh);
    c.axes.push_back({3,10,0,false}); executor.configure(c);
    script.commands={"home 1 2 await"};
    assert(executor.start(script,true,zeros,30)); setMillis(31); queue.poll(31);
    injectRx(makeAck(1,0x9A,2)); setMillis(40); motor.poll(); queue.poll(40);
    for (uint32_t now=100;now<1400;now+=100) {
        const uint8_t status[]={0x3B,3,0x6B}; injectRx(makeFrame(1,status,3));
        feedback(motor,now,0); queue.poll(now);
    }
    assert(queue.active()); // ACK + idle status never establishes a demo home
    injectRx(makeAck(1,0x9A,0x9F)); setMillis(1400); motor.poll(); queue.poll(1400);
    feedback(motor,1410,0); queue.poll(1410);
    feedback(motor,1420,0); queue.poll(1420); queue.poll(1430);
    assert(queue.state()==QueueState::Done);
    setMillis(1440); executor.poll(1440);
    assert(executor.execution()==DemoExecution::Running); // marker needs fresh readback
    feedback(motor,1450,0); setMillis(1460); executor.poll(1460);
    setMillis(1480); executor.poll(1480);
    assert(executor.execution()==DemoExecution::Done);
    for (const auto& frame : capturedTX) if (frame.data[0] == 0x50)
        assert(uint8_t(frame.identifier >> 8) == 1); // non-zero axes do not gate initialization
    const uint8_t rebootFlags[]={0x3A,3,0x6B};
    setMillis(1490); injectRx(makeFrame(1,rebootFlags,3)); motor.poll();
    assert(!executor.healthy()); // actual driver power-cycle marker disappeared
    assert(executor.reset()); feedback(motor,1495,0);
    assert(executor.start(script,true,zeros,1500)); setMillis(1501); queue.poll(1501);
    injectRx(makeAck(1,0x9A,0x12)); setMillis(1510); motor.poll(); queue.poll(1510);
    assert(queue.state()==QueueState::Failed);
    assert(executor.failureError()==DisplayError::Unknown);
    assert(executor.reset()); feedback(motor,1600,0);
    injectRx(makeAck(1,0xF3,0xE2)); setMillis(1601); motor.poll();
    assert(!executor.healthy());
    executor.configure(c);
    setMillis(1610); injectRx(makePosition(3,700)); injectRx(makeVelocity(3,0));
    const uint8_t flags3[]={0x3A,0x83,0x6B}; injectRx(makeFrame(3,flags3,3)); motor.poll();
    assert(executor.evidence(3).fresh && executor.evidence(3).position==700);
    std::cout << "PASS real demo queue: sync preflight, absolute zero, preserved feedback, post-stop proof, strict home, rejected enable\n";
}
