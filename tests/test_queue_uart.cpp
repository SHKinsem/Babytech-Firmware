#include "QueueBoardMotion.h"
#include "fake_x42s.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace babytech::v2;
struct Rotation : motion::QueueRotationSource {
    bool rotationMm(uint8_t,double&) const override {return false;}
};
int main() {
    fakecan::fakeReset();motion::MotorControl motor;assert(motor.begin(4,5,500000));
    motion::CommandQueue queue(motor);QueueBoardMotion backend(motor,queue);
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
