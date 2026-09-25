#include "BoardMotion.h"
#include "fake_x42s.h"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace babytech::v2;
using namespace fakecan;
struct Rig {
    motion::MotorControl motor; motion::CommandQueue queue; motion::DeviceAPI api; BoardMotion bridge; Endpoint endpoint;
    Rig():queue(motor),api(motor,queue),bridge(motor,api),endpoint(bridge) {
        fakeReset();motor.begin(4,5,500000);endpoint.begin(9);
    }
    void feedback(uint32_t now,int32_t pos,int32_t speed=0) {
        setMillis(now);injectRx(makePosition(1,pos));injectRx(makeVelocity(1,speed));motor.poll();
    }
    void enable() {
        setMillis(10);assert(bridge.enable(1,true)==Reason::None);
        Reason reason;assert(bridge.operation(reason)==Outcome::Accepted);
        injectRx(makeAck(1,motion::kFrameEnable,0x02));
        const uint8_t flags[]={0x3A,1,0x6B};injectRx(makeFrame(1,flags,sizeof(flags)));feedback(20,0);
        assert(bridge.operation(reason)==Outcome::Done);
    }
    Frame run(uint32_t seq=1) {
        Frame q;q.cmd=Cmd::Exec;q.session=7;q.sequence=seq;
        Writer w(q.payload,kMaxPayload);w.put(9,8);w.put(kStage,1);w.put(1,2);w.put(1,2);w.put(1,4);q.length=w.size();return q;
    }
};
static Frame unverifiedWrite(uint32_t seq,uint32_t revision,uint16_t field,uint32_t value) {
    Frame q;q.cmd=Cmd::Write;q.session=7;q.sequence=seq;
    Writer w(q.payload,kMaxPayload);w.put(9,8);w.put(revision,4);w.put(1,1);
    w.put(kStage,1);w.put(kMoveStage,2);w.put(field,2);w.put(4,2);w.put(value,4);
    q.length=w.size();return q;
}
static Frame unverifiedEnable(uint32_t seq,uint16_t id) {
    Frame q;q.cmd=Cmd::Exec;q.session=7;q.sequence=seq;
    Writer w(q.payload,kMaxPayload);w.put(9,8);w.put(kMotor,1);
    w.put(id,2);w.put(kEnable,2);w.put(0,4);q.length=w.size();return q;
}
static std::vector<uint8_t> logicalFromRawPackets(uint8_t id) {
    std::vector<uint8_t> logical{id};
    for (const auto& packet : capturedTX) {
        if (uint8_t(packet.identifier>>8)!=id) continue;
        if (logical.size()==1) logical.push_back(packet.data[0]);
        for (uint8_t i=1;i<packet.length;++i) logical.push_back(packet.data[i]);
    }
    return logical;
}
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
    {
        Rig r;Frame response,event;Outcome outcome;Reason reason;
        // Historical fault and absent ACK/position/velocity no longer gate the
        // UART command once the persistent board mode has been enabled.
        busState=CanControllerState::BusOff;setMillis(1);r.motor.poll();
        assert(r.motor.hasFault() && capturedTX.empty());
        busState=CanControllerState::Running;r.motor.setUnverifiedMode(true);
        auto q=unverifiedWrite(1,999,2,uint32_t(-1234567890));
        r.endpoint.handle(q,2,response);result(response,outcome,reason);assert(outcome==Outcome::Ok);
        q=unverifiedWrite(2,0,3,1234);
        r.endpoint.handle(q,3,response);result(response,outcome,reason);assert(outcome==Outcome::Ok);
        const auto move=r.run(3);setMillis(4);r.endpoint.handle(move,4,response);
        assert(result(response,outcome,reason) && outcome==Outcome::Done && reason==Reason::None);
        const std::vector<uint8_t> expected={
            1,0xCD,1,0,10,0,10,0x04,0xD2,0x49,0x96,0x02,0xD2,2,0,0x03,0x20,0x6B};
        assert(logicalFromRawPackets(1)==expected);
        const size_t sent=capturedTX.size();
        r.endpoint.handle(move,5,response);assert(capturedTX.size()==sent);
        setMillis(5000);assert(!r.endpoint.tick(5000,event));assert(capturedTX.size()==sent);
        const auto enable=unverifiedEnable(4,1);
        r.endpoint.handle(enable,5001,response);
        assert(result(response,outcome,reason) && outcome==Outcome::Done && reason==Reason::None);
        assert(capturedTX.size()==sent+1 && capturedTX.back().data[0]==0xF3);
        assert(capturedTX.back().data[1]==0xAB && capturedTX.back().data[2]==1);
        assert(capturedTX.back().data[3]==0 && capturedTX.back().data[4]==0x6B);
        const size_t beforeRead=capturedTX.size();
        const auto motorRead=readField(7,5,9,kMotor,1,1);
        r.endpoint.handle(motorRead,5002,response);
        assert(result(response,outcome,reason) && outcome==Outcome::Ok);
        r.motor.poll();
        assert(capturedTX.size()==beforeRead); // Brain READ observes cache only
    }
    std::puts("PASS: UART endpoint + actual motor controller: ACK, completion, external stop, link loss, feedback and faults");
}
