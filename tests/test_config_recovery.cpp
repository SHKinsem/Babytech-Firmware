#include "CommandQueue.h"
#include "fake_x42s.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

using namespace motion;
using namespace fakecan;

struct Rotation : QueueRotationSource {
    bool rotationMm(uint8_t id,double& value) const override {value=2;return id==1;}
};
struct Rig {
    MotorControl motor; CommandQueue queue; Rotation rotation;
    Rig():queue(motor) {fakeReset();assert(motor.begin(4,5,500000));}
    void tick(uint32_t t) {setMillis(t);motor.poll();queue.poll(t);}
    void feed(uint32_t t,bool enabled,int pos=0) {
        const uint8_t flags[]={0x3A,uint8_t(enabled),0x6B};
        injectRx(makeFrame(1,flags,sizeof(flags)));
        injectRx(makePosition(1,pos));injectRx(makeVelocity(1,0));tick(t);
    }
    void start(const char* text,uint32_t t=10) {
        setMillis(t);assert(queue.start(text,strlen(text),1,rotation,t).code==202);
    }
};
static const uint8_t config[]={1,0x4C,0xAE,1,2,0,0,100,0,0,0x27,0x10,0,5,0,120,0,60,0,0x6B};
static CanRawFrame part(uint8_t packet,bool mismatch=false) {
    uint8_t data[8]={0x22};
    const uint8_t count=packet<2?7:2;
    for(uint8_t i=0;i<count;++i) {
        const uint8_t offset=packet*7+i;
        data[i+1]=offset==15?0x6B:config[4+offset];
    }
    if(mismatch && packet==0)data[1]=3;
    auto frame=makeFrame(1,data,count+1);frame.identifier|=packet;return frame;
}
static unsigned wireCount(uint8_t opcode) {
    unsigned count=0;
    for(const auto& f:capturedTX)
        if((f.identifier>>8)==1 && f.length && f.data[0]==opcode)++count;
    return count;
}
static void expectConfig(const MotorControl& motor,const char* state,bool pending) {
    assert(std::strcmp(motor.configMessage(),state)==0);
    const std::string json=motor.configJson().str();
    assert(json.find(std::string("\"state\":\"")+state+"\"")!=std::string::npos);
    assert(json.find(pending?"\"pending\":true":"\"pending\":false")!=std::string::npos);
}
static void sendConfig(Rig& rig,uint32_t now=10) {
    setMillis(now);assert(rig.motor.rawLogical(config,sizeof(config)));
    expectConfig(rig.motor,"config_wait_ack",true);
}
static void acceptAndQuery(Rig& rig) {
    injectRx(makeAck(1,0x4C,2));rig.tick(40);
    expectConfig(rig.motor,"config_wait_readback",true);
    rig.tick(120);assert(wireCount(0x22)==1);
}
int main() {
    // The read uses the shared query budget once, and successful evidence sticks.
    {
        Rig r;r.motor.setAutoQueriesEnabled(false);sendConfig(r);
        injectRx(makeAck(1,0x4C,2));r.tick(40);
        expectConfig(r.motor,"config_wait_readback",true);
        r.tick(99);assert(wireCount(0x22)==0);
        r.tick(120);assert(wireCount(0x22)==1);
        assert(r.motor.queryStatusJson().str().find("\"queries\":1")!=std::string::npos);
        r.tick(140);assert(wireCount(0x22)==1);
        injectRx(part(0));injectRx(part(1));injectRx(part(2));r.tick(160);
        expectConfig(r.motor,"config_verified",false);
        assert(r.motor.queryStatusJson().str().find("\"responses\":1")!=std::string::npos);
        const std::string evidence=r.motor.configJson().str();
        r.feed(180,false);assert(r.motor.configJson().str()==evidence);
        sendConfig(r,200);
        assert(r.motor.configJson().str().find("\"sequence\":2")!=std::string::npos);
    }
    // Complete stale response buffered with the ACK cannot satisfy a fresh read.
    {
        Rig r;r.motor.setAutoQueriesEnabled(false);sendConfig(r);
        injectRx(makeAck(1,0x4C,2));
        injectRx(part(0));injectRx(part(1));injectRx(part(2));r.tick(40);
        expectConfig(r.motor,"config_wait_readback",true);
        r.tick(120);assert(wireCount(0x22)==1);
        expectConfig(r.motor,"config_wait_readback",true);
        injectRx(part(0));injectRx(part(1));injectRx(part(2));r.tick(140);
        expectConfig(r.motor,"config_verified",false);
    }
    // The RX drain is bounded to 16 frames per poll. A stale readback left in
    // its queue must be discarded before the new 0x22 query can be sent.
    {
        Rig r;r.motor.setAutoQueriesEnabled(false);sendConfig(r);
        injectRx(makeAck(1,0x4C,2));
        for(int i=0;i<16;++i)injectRx(makeAck(2,0x4C,2));
        injectRx(part(0));injectRx(part(1));injectRx(part(2));
        r.tick(120);
        assert(!rxQueue.empty());
        assert(wireCount(0x22)==0);
        expectConfig(r.motor,"config_wait_readback",true);
        r.tick(140);
        assert(rxQueue.empty());
        assert(wireCount(0x22)==1);
        expectConfig(r.motor,"config_wait_readback",true);
        injectRx(part(0));injectRx(part(1));injectRx(part(2));r.tick(160);
        expectConfig(r.motor,"config_verified",false);
    }
    // Holding the pending config read does not stall another selected node's
    // ordinary page query. Both still share the same scheduler.
    {
        Rig r;r.motor.watch(2);sendConfig(r);
        injectRx(makeAck(1,0x4C,2));
        for(int i=0;i<16;++i)injectRx(makeAck(2,0x4C,2));
        injectRx(part(0));injectRx(part(1));injectRx(part(2));
        r.tick(120);
        assert(!rxQueue.empty());
        assert(wireCount(0x22)==0);
        assert(countTxTo(2,TxKind::ReadSysParam)==1);
        expectConfig(r.motor,"config_wait_readback",true);
    }
    // Wrong node, out of order, duplicate and truncated packets cannot verify.
    {
        Rig r;r.motor.setAutoQueriesEnabled(false);sendConfig(r);acceptAndQuery(r);
        auto wrongNode=part(0);wrongNode.identifier=(uint32_t(2)<<8);
        auto truncated=part(1);truncated.length=7;
        injectRx(part(1));injectRx(wrongNode);injectRx(part(0));
        injectRx(part(0));injectRx(truncated);injectRx(part(2));r.tick(140);
        expectConfig(r.motor,"config_wait_readback",true);
        injectRx(part(1));injectRx(part(2));r.tick(160);
        expectConfig(r.motor,"config_verified",false);
    }
    // A fresh response can be drained again within the query's millisecond.
    {
        Rig r;sendConfig(r);acceptAndQuery(r);
        injectRx(part(0));injectRx(part(1));injectRx(part(2));r.tick(120);
        expectConfig(r.motor,"config_verified",false);
    }
    for(int failure=0;failure<4;++failure) {
        Rig r;sendConfig(r);r.tick(20);
        if(failure==0) {
            injectRx(makeAck(1,0x4C,0xE2));r.tick(40);
            expectConfig(r.motor,"config_rejected",false);
        }
        if(failure==1) {
            r.tick(3021);expectConfig(r.motor,"config_ack_timeout",false);
            injectRx(makeAck(1,0x4C,2));r.tick(3040);
            expectConfig(r.motor,"config_ack_timeout",false);
        }
        if(failure>=2) {
            injectRx(makeAck(1,0x4C,2));r.tick(40);r.tick(120);
            if(failure==2) {
                r.tick(3121);expectConfig(r.motor,"config_readback_timeout",false);
                injectRx(part(0));injectRx(part(1));injectRx(part(2));r.tick(3140);
                expectConfig(r.motor,"config_readback_timeout",false);
            } else {
                injectRx(part(0,true));injectRx(part(1));injectRx(part(2));r.tick(140);
                expectConfig(r.motor,"config_mismatch",false);
            }
        }
        assert(countTx(TxKind::Enable)==0);assert(r.motor.configFailed());
    }
    // Bus-off prevents RX and TX, but must not freeze configuration evidence
    // forever. Both the ACK and readback phases retain their original timeout.
    {
        Rig r;r.motor.setAutoQueriesEnabled(false);sendConfig(r);
        busState=CanControllerState::BusOff;
        r.tick(20);expectConfig(r.motor,"config_wait_ack",true);
        r.tick(3011);expectConfig(r.motor,"config_ack_timeout",false);
        assert(wireCount(0x22)==0);
    }
    {
        Rig r;r.motor.setAutoQueriesEnabled(false);sendConfig(r);
        injectRx(makeAck(1,0x4C,2));r.tick(40);
        expectConfig(r.motor,"config_wait_readback",true);
        busState=CanControllerState::BusOff;
        r.tick(60);expectConfig(r.motor,"config_wait_readback",true);
        r.tick(3041);expectConfig(r.motor,"config_readback_timeout",false);
        assert(wireCount(0x22)==0);
    }
    // A failed 4C write cannot leave a pending transaction.
    {
        Rig r;setMillis(10);failNextMoveTx=true;
        assert(!r.motor.rawLogical(config,sizeof(config)));
        expectConfig(r.motor,"none",false);assert(wireCount(0x4C)==0);
    }
    // A failed budgeted 0x22 transmission is visible and terminal. It neither
    // retries behind the operator's back nor holds the operation gate.
    {
        Rig r;r.motor.setAutoQueriesEnabled(false);sendConfig(r);
        injectRx(makeAck(1,0x4C,2));r.tick(40);
        failNextMoveTx=true;r.tick(120);
        expectConfig(r.motor,"config_read_tx_failed",false);
        assert(r.motor.configFailed() && !r.motor.operationBusy());
        assert(wireCount(0x22)==0);
        assert(r.motor.queryStatusJson().str().find("\"sendErrors\":1")!=std::string::npos);
        r.tick(240);r.tick(420);
        assert(wireCount(0x22)==0);
        assert(r.motor.queryStatistics().sendErrors==1);
    }
    // A stop for another node leaves the transaction, matching/broadcast stop cancel.
    {
        Rig r;sendConfig(r);
        assert(r.motor.stop(2).code==202);
        expectConfig(r.motor,"config_wait_ack",true);
        assert(r.motor.stop(1).code==202);
        expectConfig(r.motor,"config_cancelled",false);
        injectRx(makeAck(1,0x4C,2));
        injectRx(part(0));injectRx(part(1));injectRx(part(2));r.tick(120);
        expectConfig(r.motor,"config_cancelled",false);
    }
    {
        Rig r;sendConfig(r);assert(r.motor.stopAll().code==202);
        expectConfig(r.motor,"config_cancelled",false);
        injectRx(makeAck(1,0x4C,2));r.tick(120);
        expectConfig(r.motor,"config_cancelled",false);
    }
    // Fault recovery through a queue enable stays possible.
    {
        Rig r;r.motor.watch(1);r.feed(10,true);
        assert(r.motor.enable(1,true).code==202);
        injectRx(makeAck(1,0xF3,2));r.feed(30,true);
        MoveRequest move{1,10,30,60,60,200};
        DebugLimits limits;limits.maxMoveDurationMs=1000;assert(r.motor.setDebugLimits(limits));
        assert(r.motor.move(move).code==202);injectRx(makeAck(1,0xCD,2));
        for(uint32_t t=50;t<=1250;t+=100)r.feed(t,true);
        assert(r.motor.hasFault());r.feed(1300,true);
        r.start("enable 1\nwait 0",1320);r.tick(1340);r.feed(1360,true);
        assert(!r.motor.hasFault());
        injectRx(makeAck(1,0xF3,2));r.feed(1380,true);
        for(uint32_t t=1400;t<=1500;t+=20)r.tick(t);
        assert(r.queue.state()==QueueState::Done);
    }
    puts("PASS 4C wire ordering, budget, cancellation, failures and fault recovery");
}
