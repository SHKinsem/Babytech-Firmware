#include "DemoMotorExecutor.h"
#include "fake_x42s.h"
#include <cassert>
#include <iostream>
#include <cstring>
#include <vector>
using namespace motion;
using namespace fakecan;
struct Rotation : QueueRotationSource {
    bool rotationMm(uint8_t, double&) const override { return false; }
};
void feedback(MotorControl& motor, uint32_t now, int32_t position) {
    setMillis(now); injectRx(makePosition(1, position)); injectRx(makeVelocity(1, 0)); motor.poll();
    const uint8_t flags[] = {0x3A, 0x83, 0x6B}; injectRx(makeFrame(1,flags,3)); motor.poll();
}
// Legacy integration fixture: protects the current wire-level behavior during
// a no-behavior-change migration. It does not define the future DeviceAPI shape.
static void test_demo_polling_does_not_starve_queue_await() {
    fakeReset(); MotorControl motor; CommandQueue queue(motor);
    struct Millimetres : QueueRotationSource {
        bool rotationMm(uint8_t, double& mm) const override { mm=10; return true; }
    } rotation;
    assert(motor.begin(4,5,500000));
    CanQueryScheduler::Config budget; budget.queriesPerSecond=30; budget.gapMs=33;
    assert(motor.queries().configure(budget));
    DemoConfig config;
    for (uint8_t id=1;id<=5;++id) config.axes.push_back({id,10,10,false});
    DemoMotorExecutor executor(motor,queue,rotation); executor.configure(config);
    motor.watch(6); // An open Page monitors another node while all five Demo axes poll.
    const char* program="enable 1\nmove 1 -5 mm 300 300 300 200 await\nhome 1 2\n";
    assert(queue.start(program,std::strlen(program),1,rotation,0).code==202);
    bool moved=false, targetRead=false, homeSent=false, pageRead=false;
    std::vector<uint32_t> queryTimes;
    size_t seen=0;
    for (uint32_t now=1;now<5000;++now) {
        setMillis(now);
        if (now%500==0) motor.watch(6); // Renew the Page lease as a live page does.
        motor.poll(false); executor.poll(now); queue.poll(now);
        motor.dispatchQueries();
        while (seen<capturedTX.size()) {
            const auto frame=capturedTX[seen];
            const uint8_t id=uint8_t(frame.identifier>>8), op=frame.data[0];
            if (frame.length==2 && frame.data[1]==0x6B && CanQueryScheduler::supported(op)) {
                if (!queryTimes.empty()) assert(now-queryTimes.back()>=34); // ceil(1000/30)
                queryTimes.push_back(now);
                if (id==6) pageRead=true;
            }
            ++seen;
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
    if (!pageRead || !targetRead || !homeSent || queue.state()!=QueueState::Done)
        std::cerr << "demo/await: pageRead=" << pageRead << " targetRead=" << targetRead << " homeSent=" << homeSent
                  << " " << queue.statusJson().str() << '\n';
    assert(pageRead && targetRead && homeSent && queue.state()==QueueState::Done);
    assert(motor.queries().statistics().queries==queryTimes.size());
    // Page and Demo demands stay live, but only Sync queries may use its window.
    motor.queries().exclusiveSync(true);
    assert(motor.queries().demand(7,0x36,CanQueryScheduler::Sync,100,600,2,5000));
    const size_t before=capturedTX.size();
    for (uint32_t now=5000;now<5500;++now) {
        setMillis(now);
        if (now%100==0) motor.watch(6);
        motor.poll(false); executor.poll(now); motor.dispatchQueries();
        while (seen<capturedTX.size()) {
            const auto frame=capturedTX[seen++];
            assert(uint8_t(frame.identifier>>8)==7 && frame.data[0]==0x36);
            assert(now-queryTimes.back()>=34);
            queryTimes.push_back(now);
            injectRx(makePosition(7,0));
        }
    }
    assert(capturedTX.size()>before);
    assert(motor.queries().statistics().queries==queryTimes.size());
    std::cout << "PASS demo and Page polling share budget: move await reaches home, Sync excludes other queries\n";
}

int main() {
    test_demo_polling_does_not_starve_queue_await();
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
