#include "BoardMotion.h"
#include "fake_x42s.h"
#include <cassert>
#include <cstdio>
using namespace babytech::v2;
using namespace fakecan;
struct Rig {
    motion::MotorControl motor; BoardMotion bridge; Endpoint endpoint;
    Rig():bridge(motor),endpoint(bridge) { fakeReset();motor.begin(4,5,500000);endpoint.begin(9); }
    void feedback(uint32_t now,int32_t pos,int32_t speed=0) {
        setMillis(now);injectRx(makePosition(1,pos));injectRx(makeVelocity(1,speed));motor.poll();
    }
    void enable() {
        setMillis(10);assert(bridge.enable(1,true)==Reason::None);
        Reason reason;assert(bridge.operation(reason)==Outcome::Accepted);
        injectRx(makeAck(1,motion::kFrameEnable,0x02));feedback(20,0);
        assert(bridge.operation(reason)==Outcome::Done);
    }
    Frame run(uint32_t seq=1) {
        Frame q;q.cmd=Cmd::Exec;q.session=7;q.sequence=seq;
        Writer w(q.payload,kMaxPayload);w.put(9,8);w.put(kStage,1);w.put(1,2);w.put(1,2);w.put(1,4);q.length=w.size();return q;
    }
};
int main() {
    {
        Rig r;Frame response,event;assert(capturedTX.empty());
        auto q=r.run();r.endpoint.handle(q,0,response);Outcome o;Reason reason;
        result(response,o,reason);assert(o==Outcome::Rejected);assert(countTx(TxKind::Move)==0);
        r.enable();setMillis(30);q=r.run(2);r.endpoint.handle(q,30,response);
        result(response,o,reason);assert(o==Outcome::Accepted);assert(countTx(TxKind::Move)==1);
        r.endpoint.handle(q,31,response);assert(countTx(TxKind::Move)==1);
        injectRx(makeAck(1,motion::kFrameMove,0x02));r.feedback(100,100);
        assert(!r.endpoint.tick(100,event));r.feedback(200,100);
        assert(r.endpoint.tick(200,event));result(event,o,reason);assert(o==Outcome::Done);
        assert(r.motor.moveOutcome()==motion::MotorControl::MoveOutcome::Done);
    }
    {
        Rig r;r.enable();Frame response,event;setMillis(30);r.endpoint.handle(r.run(),30,response);
        // HTTP stop can cancel before UART tick; explicit outcome prevents false DONE.
        setMillis(40);r.motor.stop(1);r.feedback(50,0);
        assert(r.endpoint.tick(50,event));Outcome o;Reason reason;result(event,o,reason);assert(o==Outcome::Cancelled);
        r.bridge.stop();assert(!r.bridge.stopped());r.feedback(60,0);assert(r.bridge.stopped());
    }
    {
        Rig r;r.enable();Frame response,event;setMillis(30);r.endpoint.handle(r.run(),30,response);
        injectRx(makeAck(1,motion::kFrameMove,0x02));r.feedback(100,20);
        // Maintain CAN feedback while withholding owner UART heartbeat.
        r.feedback(1530,20,10);assert(r.endpoint.tick(1530,event));
        Outcome o;Reason reason;result(event,o,reason);assert(o==Outcome::Failed && reason==Reason::Timeout);
        assert(countTxTo(0,TxKind::Stop)==1);assert(!r.bridge.stopped());
        assert(r.endpoint.status(1530).state==State::Stopping);
        r.feedback(1540,20);assert(r.bridge.stopped());
    }
    {
        Rig r;r.bridge.stop();assert(!r.bridge.stopped()); // broadcast without targets is unconfirmed
        r.motor.watch(1);r.feedback(10,0);r.bridge.stop();assert(!r.bridge.stopped());
        r.feedback(20,0);assert(r.bridge.stopped());
        busState=CanControllerState::BusOff;r.motor.poll();assert(!r.bridge.stopped());
    }
    {
        Rig r;r.enable();Frame response,event;setMillis(30);r.endpoint.handle(r.run(),30,response);
        injectRx(makeAck(1,motion::kFrameMove,0xE2));r.motor.poll();
        assert(r.endpoint.tick(30,event));Outcome o;Reason reason;result(event,o,reason);assert(o==Outcome::Failed);
        assert(r.motor.moveOutcome()==motion::MotorControl::MoveOutcome::Failed);
    }
    {
        Rig r;r.enable();setMillis(30);assert(r.bridge.enable(1,false)==Reason::None);
        r.feedback(40,0); // stationary feedback alone cannot confirm disable
        Reason reason;assert(r.bridge.operation(reason)==Outcome::Accepted);
        r.feedback(1600,0);assert(r.bridge.operation(reason)==Outcome::Failed && reason==Reason::Timeout);
    }
    {
        Rig r;setMillis(10);assert(r.bridge.enable(1,true)==Reason::None);
        setMillis(1600);r.motor.poll();Reason reason;
        assert(r.bridge.operation(reason)==Outcome::Failed && reason==Reason::Timeout);
    }
    std::puts("PASS: UART endpoint + actual motor controller: ACK, completion, external stop, link loss, feedback and faults");
}
