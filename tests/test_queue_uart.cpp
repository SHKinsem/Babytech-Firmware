#include "QueueBoardMotion.h"
#include "fake_x42s.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace babytech::v2;
struct Rotation : motion::QueueRotationSource {
    bool rotationMm(uint8_t,double&) const override {return false;}
};
static Frame exec(uint64_t boot, uint32_t seq, uint8_t cls, uint16_t id, uint16_t op) {
    Frame q; q.cmd=Cmd::Exec; q.session=9; q.sequence=seq;
    Writer w(q.payload,sizeof(q.payload)); w.put(boot,8); w.put(cls,1);
    w.put(id,2); w.put(op,2); w.put(1,4); q.length=w.size(); return q;
}
static void test_disable_preempts_queue_and_preserves_validation() {
    fakecan::fakeReset(); motion::MotorControl motor; assert(motor.begin(4,5,500000));
    motion::CommandQueue queue(motor); motion::DeviceAPI api(motor,queue); QueueBoardMotion backend(motor,queue,api);
    Endpoint endpoint(backend); endpoint.begin(9); Rotation rotation;
    const char* program="wait 1000\nenable 1";
    assert(queue.start(program,strlen(program),1,rotation,0).code==202);
    Frame response; Outcome outcome; Reason reason;
    auto q=exec(9,1,kMotor,1,kEnable); endpoint.handle(q,0,response);
    result(response,outcome,reason); assert(outcome==Outcome::Rejected && reason==Reason::Busy);
    q=exec(9,2,kMotor,0,kDisable); endpoint.handle(q,0,response);
    result(response,outcome,reason); assert(outcome==Outcome::Rejected && queue.active());
    q=exec(9,3,kMotor,1,kDisable); q.length++; endpoint.handle(q,0,response);
    result(response,outcome,reason); assert(outcome==Outcome::Rejected && queue.active());
    q=exec(8,4,kMotor,1,kDisable); endpoint.handle(q,0,response);
    result(response,outcome,reason); assert(outcome==Outcome::Rejected && queue.active());
    q=exec(9,5,kMotor,1,kDisable); q.payload[q.length-4]=2; endpoint.handle(q,0,response);
    result(response,outcome,reason); assert(outcome==Outcome::Rejected && reason==Reason::ConfigMismatch && queue.active());
    assert(fakecan::capturedTX.empty());
    q=exec(9,6,kMotor,1,kDisable); endpoint.handle(q,10,response);
    result(response,outcome,reason); assert(outcome==Outcome::Accepted);
    assert(queue.state()==motion::QueueState::Cancelled);
    const size_t count=fakecan::capturedTX.size();
    endpoint.handle(q,11,response); assert(fakecan::capturedTX.size()==count);
    queue.poll(2000); assert(fakecan::capturedTX.size()==count);
}
static void test_disable_other_node_preserves_cancelled_owner() {
    fakecan::fakeReset(); motion::MotorControl motor; assert(motor.begin(4,5,500000));
    motion::CommandQueue queue(motor); motion::DeviceAPI api(motor,queue); QueueBoardMotion backend(motor,queue,api);
    Endpoint endpoint(backend); endpoint.begin(9);
    fakecan::setMillis(10); assert(motor.enable(1,true).code==202);
    fakecan::injectRx(fakecan::makeAck(1,0xF3,2));
    const uint8_t flags[]={0x3A,1,0x6B}; fakecan::injectRx(fakecan::makeFrame(1,flags,sizeof(flags)));
    fakecan::setMillis(20); fakecan::injectRx(fakecan::makePosition(1,0));
    fakecan::injectRx(fakecan::makeVelocity(1,0)); motor.poll();
    Frame response; Outcome outcome; Reason reason;
    const auto run=exec(9,1,kStage,kMoveStage,kRun); endpoint.handle(run,30,response);
    result(response,outcome,reason); assert(outcome==Outcome::Accepted);
    assert(endpoint.status(30).state==State::Running);
    const auto disable=exec(9,2,kMotor,2,kDisable); endpoint.handle(disable,40,response);
    result(response,outcome,reason); assert(outcome==Outcome::Accepted);
    assert(motor.moveOutcome()==motion::MotorControl::MoveOutcome::Cancelled);
    assert(fakecan::countTxTo(0,fakecan::TxKind::Stop)>0);
    assert(motor.snapshot(1).stopPending && motor.snapshot(2).enablePending);
    assert(endpoint.status(40).state==State::Stopping);
    const size_t count=fakecan::capturedTX.size();
    endpoint.handle(run,41,response); result(response,outcome,reason);
    assert(outcome==Outcome::Cancelled && fakecan::capturedTX.size()==count);
    endpoint.handle(disable,42,response); assert(fakecan::capturedTX.size()==count);
}
int main() {
    test_disable_preempts_queue_and_preserves_validation();
    test_disable_other_node_preserves_cancelled_owner();
    fakecan::fakeReset();motion::MotorControl motor;assert(motor.begin(4,5,500000));
    motion::CommandQueue queue(motor);motion::DeviceAPI api(motor,queue);QueueBoardMotion backend(motor,queue,api);
    Endpoint endpoint(backend);const uint64_t boot=0x0102030405060708ULL;endpoint.begin(boot);
    Rotation rotation;const char* program="wait 1000";
    assert(queue.start(program,strlen(program),1,rotation,0).code==202);
    Frame stop,response;stop.kind=Kind::Request;stop.cmd=Cmd::Stop;stop.session=9;stop.sequence=1;
    Writer w(stop.payload,sizeof(stop.payload));w.put(boot,8);stop.length=w.size();
    Frame wrong=stop;wrong.length=9;
    endpoint.handle(wrong,0,response);assert(queue.active());assert(fakecan::capturedTX.empty());
    wrong=stop;wrong.sequence=2;wrong.payload[0]^=1;
    endpoint.handle(wrong,0,response);assert(queue.active());assert(fakecan::capturedTX.empty());
    stop.sequence=3;
    assert(endpoint.handle(stop,0,response));
    Outcome outcome;Reason reason;assert(result(response,outcome,reason));
    assert(outcome==Outcome::Accepted);assert(queue.state()==motion::QueueState::Cancelled);
    const size_t transmissions=fakecan::capturedTX.size();assert(transmissions>0);
    endpoint.handle(stop,0,response);assert(fakecan::capturedTX.size()==transmissions);
    queue.poll(2000);assert(queue.state()==motion::QueueState::Cancelled);
    // Explicit reset releases both HTTP queue and UART transaction ownership,
    // keeps the original request's terminal cancellation cached, and never
    // reports a physically confirmed stop merely because memory was cleared.
    Frame event;
    assert(endpoint.busy());
    assert(endpoint.cancelPending(event));
    assert(result(event,outcome,reason));
    assert(outcome==Outcome::Cancelled && reason==Reason::StopUnconfirmed);
    assert(!endpoint.cancelPending(event));
    const auto reset=queue.clearControlState();assert(reset.code<300);
    assert(queue.state()==motion::QueueState::Idle && !motor.operationBusy());
    assert(!endpoint.busy());
    const size_t afterClear=fakecan::capturedTX.size();
    endpoint.handle(stop,2200,response);
    assert(result(response,outcome,reason) && outcome==Outcome::Cancelled);
    assert(fakecan::capturedTX.size()==afterClear);
    queue.poll(5000);assert(!queue.active());
    assert(fakecan::capturedTX.size()==afterClear);
    assert(queue.start(program,strlen(program),1,rotation,5010).code==202);
    queue.clearControlState();queue.poll(7000);
    assert(queue.state()==motion::QueueState::Idle);
    puts("PASS queue + actual UART endpoint: malformed/wrong-boot STOP has no effects, accepted STOP cancels, duplicate STOP does not retransmit");
}
