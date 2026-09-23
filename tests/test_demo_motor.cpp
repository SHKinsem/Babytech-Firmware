#include "DemoMotorExecutor.h"
#include "fake_x42s.h"
#include <cassert>
#include <iostream>
using namespace motion;
using namespace fakecan;
struct Rotation : QueueRotationSource {
    bool rotationMm(uint8_t, double&) const override { return false; }
};
void feedback(MotorControl& motor, uint32_t now, int32_t position) {
    setMillis(now); injectRx(makePosition(1, position)); injectRx(makeVelocity(1, 0)); motor.poll();
    const uint8_t flags[] = {0x3A, 0x83, 0x6B}; injectRx(makeFrame(1,flags,3)); motor.poll();
}
int main() {
    fakeReset(); MotorControl motor; CommandQueue queue(motor); Rotation rotation;
    assert(motor.begin(4,5,500000));
    DemoConfig c; c.axes.push_back({1,10,0,true});
    DemoMotorExecutor executor(motor,queue,rotation); executor.configure(c);
    c.axes[0].rotationMm = 10;
    assert(!executor.configurationValid() && executor.available());
    DemoFlowController flow(executor);
    assert(flow.apply(c)); // stale rotation must not prevent replacing the configuration
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
    c.axes.push_back({3,10,0,false}); executor.configure(c);
    setMillis(1610); injectRx(makePosition(3,700)); injectRx(makeVelocity(3,0));
    const uint8_t flags3[]={0x3A,0x83,0x6B}; injectRx(makeFrame(3,flags3,3)); motor.poll();
    assert(executor.evidence(3).fresh && executor.evidence(3).position==700);
    std::cout << "PASS real demo queue: absolute zero, preserved feedback, post-stop proof, strict home, rejected enable\n";
}
