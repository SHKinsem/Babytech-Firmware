#include "CommandQueue.h"
#include "fake_x42s.h"
#include <cassert>
#include <cstring>
#include <cstdio>
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
    auto f=makeFrame(1,data,count+1);f.identifier|=packet;return f;
}
int main() {
    {
        Rig r;
        assert(r.motor.rawLogical(config,sizeof(config)));
        assert(r.motor.configPending());
        injectRx(makeAck(1,0x4C,2));r.tick(40);
        assert(std::strcmp(r.motor.configMessage(),"config_wait_readback")==0);
        injectRx(part(0));injectRx(part(1));injectRx(part(2));r.tick(60);
        assert(std::strcmp(r.motor.configMessage(),"config_verified")==0);
        r.feed(80,false);
        assert(std::strcmp(r.motor.configMessage(),"config_verified")==0);
    }
    for(int failure=0;failure<4;++failure) {
        Rig r;setMillis(10);assert(r.motor.rawLogical(config,sizeof(config)));r.tick(20);
        if(failure==0) {injectRx(makeAck(1,0x4C,0xE2));r.tick(40);}
        if(failure==1)r.tick(3021);
        if(failure>=2) {
            injectRx(makeAck(1,0x4C,2));r.tick(40);
            if(failure==2)r.tick(3041);
            else {injectRx(part(0,true));injectRx(part(1));injectRx(part(2));r.tick(60);}
        }
        assert(countTx(TxKind::Enable)==0);
        assert(r.motor.configFailed());
    }
    {
        Rig r;r.motor.watch(1);r.feed(10,true);
        assert(r.motor.enable(1,true).code==202);
        injectRx(makeAck(1,0xF3,2));r.feed(30,true);
        MoveRequest move{1,10,30,60,60,200};
        DebugLimits limits;limits.maxMoveDurationMs=1000;assert(r.motor.setDebugLimits(limits));
        assert(r.motor.move(move).code==202);injectRx(makeAck(1,0xCD,2));
        for(uint32_t t=50;t<=1250;t+=100)r.feed(t,true);
        assert(r.motor.hasFault());r.feed(1300,true);
        // The fault is recoverable from a queue enable without a lab request.
        r.start("enable 1\nwait 0",1320);r.tick(1340);r.feed(1360,true);
        assert(!r.motor.hasFault());
        injectRx(makeAck(1,0xF3,2));r.feed(1380,true);
        for(uint32_t t=1400;t<=1500;t+=20)r.tick(t);
        assert(r.queue.state()==QueueState::Done);
    }
    puts("PASS manual 4C readback, sticky evidence, rejection/timeouts, queue clears historical fault");
}
