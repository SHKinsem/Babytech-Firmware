#include "MotorControl.h"
#include "CommandQueue.h"
#include <deque>
#include <vector>
#include <stdexcept>
#include <iostream>
#include <climits>

using namespace motion;
MotorTestSerial Serial;
namespace hw {
uint32_t now = 1000;
int state = TWAI_STATE_RUNNING, rejectOpcode = -1, rejectPacket = -1;
bool automatic = false;
std::deque<twai_message_t> rx;
std::vector<twai_message_t> tx;
std::vector<uint32_t> txAt;
struct Delayed { twai_message_t frame; uint32_t at; };
std::deque<Delayed> delayed;
uint32_t responseDelay=0;
uint8_t missingId=0, missingField=0, movingId=0;
void reset() {
    now=1000; state=TWAI_STATE_RUNNING; rejectOpcode=rejectPacket=-1; automatic=false;
    responseDelay=0; missingId=missingField=movingId=0;
    rx.clear();tx.clear();txAt.clear();delayed.clear();
}
void frame(uint8_t id, std::initializer_list<uint8_t> bytes) {
    twai_message_t f; f.extd=true; f.identifier=uint32_t(id)<<8;
    for (auto b:bytes) f.data[f.data_length_code++]=b;
    rx.push_back(f);
}
void position(uint8_t id) { frame(id,{0x36,0,0,0,0,0,0x6B}); }
void velocity(uint8_t id, uint16_t speed=0) { frame(id,{0x35,0,uint8_t(speed>>8),uint8_t(speed),0x6B}); }
}
unsigned long millis() { return hw::now; }
void delay(unsigned long ms) { hw::now += uint32_t(ms); }
esp_err_t twai_stop() { return ESP_OK; }
esp_err_t twai_start() { return ESP_OK; }
esp_err_t twai_driver_uninstall() { return ESP_OK; }
esp_err_t twai_driver_install(const twai_general_config_t*,const twai_timing_config_t*,const twai_filter_config_t*) { return ESP_OK; }
esp_err_t twai_get_status_info(twai_status_info_t* out) { *out=twai_status_info_t{}; out->state=hw::state; return ESP_OK; }
esp_err_t twai_transmit(const twai_message_t* f,unsigned long) {
    if (f->data_length_code && f->data[0]==hw::rejectOpcode &&
        (hw::rejectPacket<0 || int(f->identifier&255)==hw::rejectPacket)) return ESP_FAIL;
    hw::tx.push_back(*f); hw::txAt.push_back(hw::now);
    const uint8_t id=uint8_t(f->identifier>>8);
    if (hw::automatic && id && !(f->identifier&255) && f->data_length_code==2) {
        const uint8_t op=f->data[0];
        if(id!=hw::missingId || op!=hw::missingField) {
            const auto size=hw::rx.size();
            if(op==0x36) hw::position(id);
            if(op==0x35) hw::velocity(id,id==hw::movingId ? 30 : 0);
            if(op==0x3A) hw::frame(id,{0x3A,1,0x6B});
            if(op==0x3B) hw::frame(id,{0x3B,0,0x6B});
            if(hw::responseDelay && hw::rx.size()>size) {
                hw::delayed.push_back({hw::rx.back(),hw::now+hw::responseDelay});hw::rx.pop_back();
            }
        }
    }
    return ESP_OK;
}
esp_err_t twai_receive(twai_message_t* out,unsigned long) {
    while(!hw::delayed.empty() && int32_t(hw::now-hw::delayed.front().at)>=0) {
        hw::rx.push_back(hw::delayed.front().frame);hw::delayed.pop_front();
    }
    if(hw::rx.empty()) return ESP_FAIL;
    *out=hw::rx.front(); hw::rx.pop_front(); return ESP_OK;
}
unsigned checks=0;
#define CHECK(expr) do { ++checks; if(!(expr)) throw std::runtime_error(std::string(#expr)+":"+std::to_string(__LINE__)); } while(false)
struct Rig {
    MotorControl motor;
    Rig() { hw::reset(); CHECK(motor.begin(4,5,500000)); motor.setAutoQueriesEnabled(false); }
    void feed(uint8_t id, uint16_t speed=0, bool advance=true) {
        if(advance) ++hw::now;
        hw::position(id); hw::velocity(id,speed); motor.poll(false);
    }
    void enabled(uint8_t id=1) {
        CHECK(motor.enable(id,true).code==202); ++hw::now;
        hw::frame(id,{0xF3,2,0x6B}); hw::frame(id,{0x3A,1,0x6B}); feed(id);
        CHECK(motor.snapshot(id).enabled);
    }
    void fastQueries() {
        auto config=motor.queries().config(); config.queriesPerSecond=100; config.gapMs=2;
        CHECK(motor.queries().configure(config));
    }
    void prove(unsigned duration=1200) {
        hw::automatic=true;
        for(unsigned i=0;i<duration/10;++i) { hw::now+=10; motor.poll(); }
    }
    void move(uint8_t id=1) {
        const uint8_t command[]={id,0xCD,0,0,60,0,60,1,44,0,0,0,100,2,0,3,32,0x6B};
        CHECK(motor.queueSendLogical(command,sizeof(command)));
    }
};

void harmless() {
    Rig r; CHECK(r.motor.otaMotionSafe()); CHECK(!r.motor.hasUnsettledMotionEvidence());
    r.enabled(); CHECK(r.motor.hasActiveMotion()); CHECK(r.motor.otaMotionSafe());
    const uint8_t read[]={0,0x36,0x6B}, stop[]={0,0xFE,0x98,0,0x6B}, abort[]={0,0x9C,0x48,0x6B};
    CHECK(r.motor.rawLogical(read,sizeof(read))); CHECK(r.motor.queueSendLogical(stop,sizeof(stop)));
    CHECK(r.motor.queueSendLogical(abort,sizeof(abort))); CHECK(r.motor.broadcastEnable(true).code==202);
    CHECK(r.motor.movementGeneration()==0); CHECK(r.motor.otaMotionSafe());
    CHECK(r.motor.queueSendFrame(0,true,read+1,2)); CHECK(r.motor.movementGeneration()==0);
}
void inheritance() {
    Rig r; r.move(7); CHECK(r.motor.movementGeneration()==1); CHECK(r.motor.hasUnsettledMotionEvidence());
    r.motor.clearControlState(); r.motor.takeQueueControl(); r.motor.noteRawTransmission(0); r.motor.watch(99);
    CHECK(!r.motor.operationBusy()); CHECK(r.motor.hasUnsettledMotionEvidence());
    r.move(8); CHECK(r.motor.movementGeneration()==2); r.feed(8); CHECK(!r.motor.affectedAxesStationary());
    r.feed(7); CHECK(r.motor.affectedAxesStationary()); CHECK(!r.motor.otaMotionSafe());
    r.motor.clearControlState(); CHECK(r.motor.affectedAxesStationary());
    CHECK(r.motor.stopAll().code==202); CHECK(r.motor.snapshot(7).stopPending); CHECK(r.motor.snapshot(8).stopPending);
    CHECK(!r.motor.snapshot(99).stopPending); r.motor.clearControlState(); r.fastQueries(); r.prove();
    CHECK(r.motor.otaMotionSafe()); r.motor.noteRawTransmission(0); r.motor.takeQueueControl();
    CHECK(r.motor.otaMotionSafe()); r.move(8); CHECK(!r.motor.otaMotionSafe()); CHECK(r.motor.hasUnsettledMotionEvidence());
}
void freshness() {
    Rig r; r.move(); r.feed(1); CHECK(r.motor.affectedAxesStationary()); CHECK(!r.motor.otaMotionSafe());
    CHECK(r.motor.stopAll().code==202); r.motor.clearControlState(); r.feed(1,0,false);
    CHECK(!r.motor.affectedAxesStationary()); r.feed(1);
    CHECK(r.motor.affectedAxesStationary()); CHECK(!r.motor.otaMotionSafe()); // No post-stop query yet.
    r.prove(); CHECK(r.motor.otaMotionSafe()); r.feed(1,6); CHECK(!r.motor.otaMotionSafe());
    r.prove(); CHECK(r.motor.otaMotionSafe()); hw::now+=601;
    CHECK(!r.motor.snapshot(1).positionValid); // Ordinary supervision stays 600 ms.
    CHECK(r.motor.affectedAxesStationary()); CHECK(r.motor.otaMotionSafe()); hw::now+=900;
    CHECK(!r.motor.affectedAxesStationary()); CHECK(!r.motor.otaMotionSafe()); r.prove(); CHECK(r.motor.otaMotionSafe());
    hw::state=TWAI_STATE_BUS_OFF; r.motor.poll(false); CHECK(!r.motor.otaMotionSafe());
}
void old_query() {
    Rig r; r.move(); hw::now+=100; r.motor.poll(); // A query predating Stop is in flight.
    CHECK(r.motor.queries().inflight()>0); CHECK(r.motor.stopAll().code==202); r.motor.clearControlState();
    r.feed(1); CHECK(!r.motor.otaMotionSafe());
    r.prove(); CHECK(r.motor.otaMotionSafe());
}
void duplicate_response() {
    Rig r; r.move(); CHECK(r.motor.stopAll().code==202); r.motor.clearControlState();
    r.prove(); CHECK(r.motor.otaMotionSafe());
    hw::automatic=false; r.motor.poll(); hw::now+=110; r.motor.poll();
    CHECK(r.motor.queries().inflight()>0); const auto query=hw::tx.back();
    CHECK(query.data_length_code==2); CHECK(query.data[0]==0x36 || query.data[0]==0x35);
    ++hw::now;
    if(query.data[0]==0x36) {hw::position(1);hw::position(1);}
    else {hw::velocity(1);hw::velocity(1);}
    r.motor.poll(false);
    CHECK(r.motor.affectedAxesStationary()); CHECK(!r.motor.otaMotionSafe());
    r.prove(); CHECK(r.motor.otaMotionSafe());
}
void stop_failure() {
    Rig r; r.move(6); r.feed(6); hw::rejectOpcode=0xFE;
    CHECK(r.motor.stopAll().code==503); r.motor.clearControlState(); r.prove();
    CHECK(r.motor.affectedAxesStationary()); CHECK(!r.motor.otaMotionSafe());
    hw::rejectOpcode=-1; CHECK(r.motor.broadcastAbortAll()); r.motor.clearControlState(); r.prove(); CHECK(r.motor.otaMotionSafe());
}
void cached_stop() {
    for(unsigned mode=0;mode<4;++mode) {
        Rig r; const uint8_t id=(mode&1)?0:3;
        const uint8_t stop[]={id,0xFE,0x98,1,0x6B};
        if(mode<2) CHECK(r.motor.rawLogical(stop,sizeof(stop)));
        else CHECK(r.motor.rawCanFrame(uint32_t(id)<<8,true,stop+1,sizeof(stop)-1));
        CHECK(r.motor.movementGeneration()==0); CHECK(!r.motor.hasUnsettledMotionEvidence());
        CHECK(r.motor.otaMotionSafe());
        r.move(3);
        if(mode<2) CHECK(r.motor.queueSendLogical(stop,sizeof(stop)));
        else CHECK(r.motor.queueSendFrame(uint32_t(id)<<8,true,stop+1,sizeof(stop)-1));
        r.prove(); CHECK(r.motor.affectedAxesStationary()); CHECK(!r.motor.otaMotionSafe());
        CHECK(r.motor.movementGeneration()==1);
        CHECK(r.motor.stopAll().code==202); r.motor.clearControlState(); r.prove();
        CHECK(r.motor.otaMotionSafe()); // No unknown latch from either cached Stop.
    }
    Rig r; const uint8_t manual[]={3,0xFE,0x98,1,0x6B};
    CHECK(r.motor.command(manual,sizeof(manual)).code==400); CHECK(hw::tx.empty());
    CHECK(r.motor.movementGeneration()==0); CHECK(r.motor.otaMotionSafe());
    const uint8_t malformed[]={3,0xFE,0x98,2,0x6B};
    CHECK(r.motor.rawLogical(malformed,sizeof(malformed))); CHECK(!r.motor.otaMotionSafe());
}
void known_transport() {
    for (uint8_t mode=0; mode<=2; ++mode) {
        Rig r;
        const uint8_t fd[]={3,0xFD,0,0,60,0,60,1,44,0,0,0,100,mode,0,0x6B};
        CHECK(r.motor.rawLogical(fd,sizeof(fd))); CHECK(r.motor.movementGeneration()==1);
        CHECK(r.motor.hasUnsettledMotionEvidence());
        CHECK(r.motor.stopAll().code==202); r.motor.clearControlState(); r.prove(); CHECK(r.motor.otaMotionSafe());
    }
    for(uint8_t op:{uint8_t(0xF5),uint8_t(0xF6),uint8_t(0xC5),uint8_t(0xC6),uint8_t(0x9A)}) {
        Rig r; uint8_t b[]={3,op,0,0,60,0,100,0,1,44,0x6B};
        uint8_t n=(op==0xF5 || op==0xF6)?9:11;
        if(n==9) b[8]=0x6B;
        if(op==0x9A) {b[2]=2;b[3]=0;b[4]=0x6B;n=5;}
        CHECK(r.motor.rawLogical(b,n)); CHECK(r.motor.movementGeneration()==1);
        CHECK(r.motor.stopAll().code==202); r.motor.clearControlState(); r.prove(); CHECK(r.motor.otaMotionSafe());
    }
    Rig r; const uint8_t home[]={0x9A,2,0,0x6B};
    CHECK(r.motor.rawCanFrame(0x900,true,home,sizeof(home))); CHECK(r.motor.movementGeneration()==1);
    CHECK(r.motor.stopAll().code==202); r.motor.clearControlState(); r.prove(); CHECK(r.motor.otaMotionSafe());
}
void structured() {
    for(unsigned mode=0;mode<7;++mode) {
        Rig r; r.enabled(); const auto before=r.motor.movementGeneration();
        if(mode==0) { MoveRequest move{1,10,30,60,60,800}; CHECK(r.motor.move(move).code==202); }
        else if(mode<=2) {
            DirectPositionRequest d; d.id=1; d.speedTenths=300; d.angleTenths=100; d.motionMode=2;
            d.withCurrentLimit=mode==2; d.currentMa=800; CHECK(r.motor.directPosition(d).code==202);
        } else if(mode==3) CHECK(r.motor.home(1,2).code==202);
        else {
            const uint8_t op=mode==4?0xF5:mode==5?0xF6:0xC6;
            uint8_t b[]={1,op,0,0,60,0,100,0,3,32,0x6B};
            const uint8_t n=op==0xC6?11:9; if(n==9) b[8]=0x6B;
            CHECK(r.motor.command(b,n).code==202);
        }
        CHECK(r.motor.movementGeneration()==before+1); r.motor.clearControlState();
        CHECK(!r.motor.affectedAxesStationary()); r.feed(1); CHECK(r.motor.affectedAxesStationary());
        CHECK(!r.motor.otaMotionSafe()); CHECK(r.motor.broadcastAbortAll()); r.motor.clearControlState(); r.prove(); CHECK(r.motor.otaMotionSafe());
    }
}
void partial() {
    {
        Rig r; hw::rejectOpcode=0xFD; hw::rejectPacket=1;
        const uint8_t fd[]={4,0xFD,0,0,60,0,60,1,44,0,0,0,100,2,1,0x6B};
        CHECK(!r.motor.queueSendLogical(fd,sizeof(fd))); CHECK(r.motor.movementGeneration()==1);
        CHECK(r.motor.hasUnsettledMotionEvidence()); hw::rejectOpcode=-1;
        CHECK(r.motor.stopAll().code==202); r.motor.clearControlState(); r.prove(); CHECK(r.motor.otaMotionSafe());
    }
    for(unsigned mode=0;mode<3;++mode) {
        Rig r; r.enabled(); hw::rejectOpcode=mode==2?0xFB:0xCD; hw::rejectPacket=1;
        if(mode==0) {
            const uint8_t b[]={1,0xCD,0,0,60,0,60,1,44,0,0,0,100,2,0,3,32,0x6B};
            CHECK(!r.motor.queueSendLogical(b,sizeof(b)));
        } else if(mode==1) { MoveRequest m{1,10,30,60,60,800}; CHECK(r.motor.move(m).code==503); }
        else { DirectPositionRequest d; d.id=1; d.speedTenths=300; d.angleTenths=100; d.motionMode=2; CHECK(r.motor.directPosition(d).code==503); }
        CHECK(r.motor.movementGeneration()==1); CHECK(r.motor.hasUnsettledMotionEvidence());
        hw::rejectOpcode=-1; r.motor.clearControlState(); CHECK(r.motor.stopAll().code==202);
        r.motor.clearControlState(); r.prove(); CHECK(r.motor.otaMotionSafe());
    }
    Rig r; uint8_t unknown[18]={4,0xE0}; unknown[17]=0x6B; hw::rejectOpcode=0xE0; hw::rejectPacket=1;
    CHECK(!r.motor.queueSendLogical(unknown,sizeof(unknown))); CHECK(r.motor.movementGeneration()==1);
    hw::rejectOpcode=-1; CHECK(r.motor.broadcastAbortAll()); r.motor.clearControlState(); r.prove(); CHECK(!r.motor.otaMotionSafe());
}
void unknown() {
    for(unsigned mode=0;mode<7;++mode) {
        Rig r; const uint8_t home[]={0x9A,2,0,0x6B};
        if(mode==0) CHECK(r.motor.queueSendFrame(0x100,false,home,sizeof(home)));
        if(mode==1) CHECK(r.motor.queueSendFrame(0x10000,true,home,sizeof(home)));
        if(mode==2) CHECK(r.motor.queueSendFrame(0x101,true,home,sizeof(home)));
        if(mode==3) {const uint8_t b[]={1,0xAE,0x4B,0,2,0x6B};CHECK(r.motor.rawLogical(b,sizeof(b)));}
        if(mode==4) {const uint8_t b[]={0,0x9A,2,0,0x6B};CHECK(r.motor.queueSendLogical(b,sizeof(b)));}
        if(mode==5) {const uint8_t b[]={0,0xFF,0x66,0x6B};CHECK(r.motor.queueSendLogical(b,sizeof(b)));}
        if(mode==6) CHECK(r.motor.rawCanFrame(0x100,true,nullptr,0));
        CHECK(r.motor.movementGeneration()==1); CHECK(!r.motor.hasUnsettledMotionEvidence());
        CHECK(r.motor.broadcastAbortAll()); r.motor.clearControlState(); r.motor.takeQueueControl(); r.motor.noteRawTransmission(0);
        r.prove(); CHECK(!r.motor.otaMotionSafe()); CHECK(r.motor.affectedAxesStationary());
        // Unknown risk is evidence, not a new debug transport gate.
        const uint8_t read[]={1,0x36,0x6B}; CHECK(r.motor.queueSendLogical(read,sizeof(read)));
        r.move(2); CHECK(r.motor.movementGeneration()==2);
    }
}
void no_tx() {
    Rig r; hw::rejectOpcode=0xCD;
    const uint8_t b[]={1,0xCD,0,0,60,0,60,1,44,0,0,0,100,2,0,3,32,0x6B};
    CHECK(!r.motor.queueSendLogical(b,sizeof(b))); CHECK(r.motor.movementGeneration()==0);
    const uint8_t unknown[]={0xE0,0x6B}; hw::rejectOpcode=0xE0;
    CHECK(!r.motor.rawCanFrame(0x100,false,unknown,sizeof(unknown)));
    CHECK(!r.motor.queueSendFrame(0x20000000,true,unknown,sizeof(unknown)));
    CHECK(!r.motor.rawLogical(nullptr,0)); CHECK(r.motor.movementGeneration()==0); CHECK(r.motor.otaMotionSafe());
    MoveRequest invalid{1,0,30,60,60,800}; CHECK(r.motor.move(invalid).code>=300); CHECK(r.motor.otaMotionSafe());
}
void multi_budget() {
    Rig r; r.fastQueries();
    for(uint8_t id=1;id<=5;++id) r.move(id);
    CHECK(r.motor.stopAll().code==202); r.motor.clearControlState(); r.prove(); CHECK(r.motor.otaMotionSafe());
    bool ids[256]={};
    for(const auto& f:hw::tx) if(f.data_length_code==2 && (f.data[0]==0x36 || f.data[0]==0x35)) ids[uint8_t(f.identifier>>8)]=true;
    for(unsigned id=1;id<256;++id) CHECK(ids[id]==(id<=5));
}
void release_control() {
    Rig r; r.move(7); CHECK(!r.motor.releaseSettledMotion()); r.feed(7);
    CHECK(r.motor.affectedAxesStationary()); CHECK(r.motor.hasUnsettledMotionEvidence());
    CHECK(r.motor.releaseSettledMotion()); CHECK(!r.motor.hasUnsettledMotionEvidence());
    hw::now+=601; CHECK(r.motor.affectedAxesStationary()); CHECK(!r.motor.otaMotionSafe());
    r.feed(7,30); CHECK(r.motor.affectedAxesStationary()); CHECK(!r.motor.hasUnsettledMotionEvidence());
    CHECK(!r.motor.otaMotionSafe());
    r.move(8); r.feed(8); CHECK(r.motor.affectedAxesStationary()); CHECK(!r.motor.otaMotionSafe());
    CHECK(r.motor.releaseSettledMotion()); r.motor.clearControlState(); CHECK(r.motor.stopAll().code==202);
    CHECK(!r.motor.snapshot(7).stopPending); CHECK(!r.motor.snapshot(8).stopPending);
    r.motor.clearControlState(); r.fastQueries(); r.prove(); CHECK(r.motor.otaMotionSafe());
    CHECK(r.motor.affectedAxesStationary());
    const uint8_t unknown[]={0,0xFF,0x66,0x6B}; CHECK(r.motor.queueSendLogical(unknown,sizeof(unknown)));
    CHECK(r.motor.affectedAxesStationary()); CHECK(r.motor.releaseSettledMotion()); CHECK(!r.motor.otaMotionSafe());
}
void current_query_budget() {
    Rig r;
    for(uint8_t id=1;id<=8;++id) {r.move(id);r.feed(id);CHECK(r.motor.releaseSettledMotion());}
    r.motor.clearControlState(); hw::tx.clear(); hw::now+=601; r.move(9); r.prove(500);
    CHECK(r.motor.affectedAxesStationary()); CHECK(r.motor.hasUnsettledMotionEvidence());
    for(const auto& f:hw::tx)
        if(f.data_length_code==2 && (f.data[0]==0x36 || f.data[0]==0x35)) CHECK(uint8_t(f.identifier>>8)==9);
    CHECK(r.motor.releaseSettledMotion()); CHECK(!r.motor.otaMotionSafe());
}
void wrap() {
    Rig r; hw::now=UINT32_MAX-20; r.move(); hw::now=UINT32_MAX-1;
    CHECK(r.motor.stopAll().code==202); CHECK(hw::now==0);
    r.motor.clearControlState(); r.feed(1,0,false); CHECK(!r.motor.otaMotionSafe());
    r.prove(); CHECK(r.motor.otaMotionSafe());
}
struct Rotation : QueueRotationSource { bool rotationMm(uint8_t,double&) const override {return false;} };
void default_budget(Rig& r) {
    const auto& c=r.motor.queries().config();
    CHECK(c.queriesPerSecond==10);CHECK(c.gapMs==100);CHECK(c.maxInflight==2);CHECK(c.timeoutMs==500);
}
bool converge(Rig& r,bool ota,bool competing=false,unsigned limit=4900) {
    hw::automatic=true; const auto start=hw::now;
    while(uint32_t(hw::now-start)<limit) {
        hw::now+=10;
        if(competing) for(uint8_t id=1;id<=5;++id)
            for(uint8_t field=0;field<4;++field) r.motor.demoProbe(id,field);
        r.motor.poll();
        if((ota ? r.motor.otaMotionSafe() : r.motor.affectedAxesStationary()) &&
            !r.motor.operationBusy()) return true;
    }
    if(!hw::missingId && !hw::movingId && hw::responseDelay<=450) {
        std::cout<<"Nonconvergence after "<<uint32_t(hw::now-start)<<"ms:";
        for(uint8_t id=1;id<=5;++id) {
            unsigned position=0,velocity=0;
            for(const auto& f:hw::tx) if(uint8_t(f.identifier>>8)==id && f.data_length_code==2) {
                if(f.data[0]==0x36) ++position;
                if(f.data[0]==0x35) ++velocity;
            }
            std::cout<<" axis"<<unsigned(id)<<"="<<position<<"/"<<velocity;
        }
        std::cout<<'\n';
    }
    return false;
}
void unchanged_rate() {
    bool seen=false; uint32_t last=0; unsigned queries=0;
    for(size_t i=0;i<hw::tx.size();++i) {
        const auto& f=hw::tx[i];
        if(f.data_length_code!=2) continue;
        if(seen) CHECK(uint32_t(hw::txAt[i]-last)>=100);
        last=hw::txAt[i];seen=true;++queries;
    }
    CHECK(queries>=10);
}
void default_five_natural() {
    Rig r; default_budget(r); CommandQueue queue(r.motor); Rotation rotation;
    const char* text="move 1 10 deg\nmove 2 10 deg\nmove 3 10 deg\nmove 4 10 deg\nmove 5 10 deg";
    CHECK(queue.start(text,std::strlen(text),1,rotation,hw::now).code==202);
    for(unsigned i=0;i<20 && queue.active();++i) {hw::now+=20;queue.poll(hw::now);}
    CHECK(queue.state()==QueueState::Done); CHECK(r.motor.movementGeneration()==5);
    CHECK(!r.motor.releaseSettledMotion());CHECK(converge(r,false));
    CHECK(!r.motor.operationBusy());CHECK(!r.motor.otaMotionSafe());
    CHECK(r.motor.releaseSettledMotion());CHECK(!r.motor.hasUnsettledMotionEvidence());
    unchanged_rate();default_budget(r);
    for(const auto& f:hw::tx) CHECK(!f.data_length_code || f.data[0]!=0xFE);
}
void default_five_stop() {
    for(unsigned reset=0;reset<2;++reset) {
        Rig r; default_budget(r); for(uint8_t id=1;id<=5;++id) r.move(id);
        CHECK(r.motor.stopAll().code==202);
        if(reset) r.motor.clearControlState();
        else CHECK(r.motor.operationBusy());
        CHECK(converge(r,true));CHECK(!r.motor.operationBusy());
        CHECK(r.motor.affectedAxesStationary());CHECK(r.motor.releaseSettledMotion());
        CHECK(r.motor.otaMotionSafe());unchanged_rate();default_budget(r);
    }
}
void competing_cap() {
    Rig r;default_budget(r);r.motor.setAutoQueriesEnabled(true);r.motor.watch(9);
    for(uint8_t id=1;id<=5;++id) {r.motor.demoWatch(id,true);r.move(id);}
    CHECK(r.motor.stopAll().code==202);CHECK(r.motor.operationBusy());
    CHECK(converge(r,true,true));CHECK(r.motor.affectedAxesStationary());
    CHECK(!r.motor.operationBusy());
    uint32_t oldest=hw::now;
    for(uint8_t id=1;id<=5;++id) {
        const auto s=r.motor.snapshot(id);
        if(hw::now-s.positionAge<oldest) oldest=hw::now-s.positionAge;
        if(hw::now-s.velocityAge<oldest) oldest=hw::now-s.velocityAge;
    }
    unchanged_rate();default_budget(r);hw::now=oldest+5000;
    CHECK(r.motor.affectedAxesStationary());CHECK(r.motor.otaMotionSafe());
    ++hw::now;CHECK(!r.motor.affectedAxesStationary());CHECK(!r.motor.otaMotionSafe());
    CHECK(converge(r,true,true));CHECK(r.motor.releaseSettledMotion());CHECK(r.motor.otaMotionSafe());
}
void latency_missing_moving() {
    for(unsigned mode=0;mode<4;++mode) {
        Rig r;hw::responseDelay=450;
        for(uint8_t id=1;id<=5;++id) r.move(id);
        CHECK(r.motor.stopAll().code==202);CHECK(r.motor.operationBusy());
        if(mode==1) {hw::missingId=3;hw::missingField=0x35;}
        if(mode==2) hw::movingId=3;
        if(mode==3) {hw::missingId=3;hw::missingField=0x36;}
        const bool safe=converge(r,true);
        CHECK(safe==(mode==0));
        if(mode) {
            CHECK(!r.motor.affectedAxesStationary());CHECK(!r.motor.releaseSettledMotion());
            hw::missingId=hw::missingField=hw::movingId=0;CHECK(converge(r,true));
        }
        CHECK(!r.motor.operationBusy());CHECK(r.motor.affectedAxesStationary());
        CHECK(r.motor.releaseSettledMotion());CHECK(r.motor.otaMotionSafe());
        unchanged_rate();default_budget(r);
    }
    Rig r;hw::responseDelay=550;r.move();CHECK(r.motor.stopAll().code==202);r.motor.clearControlState();
    CHECK(!converge(r,true)); // Replies outside the actual query timeout are not Stop proof.
}
void expansion_and_minimum() {
    Rig r;r.move(1);r.feed(1);hw::now+=1501;
    CHECK(!r.motor.affectedAxesStationary());
    r.move(2);r.feed(2);CHECK(!r.motor.affectedAxesStationary());
    hw::position(1);++hw::now;r.motor.poll(false);CHECK(!r.motor.affectedAxesStationary());
    r.feed(1);CHECK(r.motor.affectedAxesStationary());
    hw::now+=2200;CHECK(!r.motor.affectedAxesStationary());
    auto c=r.motor.queries().config();c.queriesPerSecond=1;CHECK(r.motor.queries().configure(c));
    CHECK(!r.motor.affectedAxesStationary());r.feed(1);CHECK(!r.motor.affectedAxesStationary());
    r.feed(2);CHECK(r.motor.affectedAxesStationary());
    Rig small; c=small.motor.queries().config();c.queriesPerSecond=100;c.gapMs=2;c.timeoutMs=20;
    CHECK(small.motor.queries().configure(c));small.move();small.feed(1);hw::now+=600;
    CHECK(small.motor.affectedAxesStationary());++hw::now;CHECK(!small.motor.affectedAxesStationary());
}
void shrink_expand_receipt() {
    Rig r;
    for(uint8_t id=1;id<=5;++id) {r.motor.demoWatch(id,true);r.move(id);}
    CHECK(r.motor.stopAll().code==202);r.motor.clearControlState();CHECK(converge(r,true,true));
    uint32_t oldest=hw::now;
    for(uint8_t id=1;id<=5;++id) {
        const auto s=r.motor.snapshot(id);
        if(hw::now-s.positionAge<oldest) oldest=hw::now-s.positionAge;
        if(hw::now-s.velocityAge<oldest) oldest=hw::now-s.velocityAge;
        r.motor.demoWatch(id,false);
    }
    hw::now=oldest+3600;
    CHECK(r.motor.affectedAxesStationary());CHECK(r.motor.otaMotionSafe());
    for(uint8_t id=1;id<=5;++id) r.motor.demoWatch(id,true);
    CHECK(r.motor.affectedAxesStationary());CHECK(r.motor.otaMotionSafe());
    hw::now=oldest+5001;CHECK(!r.motor.affectedAxesStationary());CHECK(!r.motor.otaMotionSafe());
    for(uint8_t id=1;id<=5;++id) r.motor.demoWatch(id,false);
    CHECK(!r.motor.affectedAxesStationary());CHECK(!r.motor.otaMotionSafe());
    for(uint8_t id=1;id<=5;++id) r.motor.demoWatch(id,true);
    CHECK(!r.motor.affectedAxesStationary());CHECK(!r.motor.otaMotionSafe());
    CHECK(converge(r,true,true));
}
void queue_lifecycle() {
    Rig r; CommandQueue queue(r.motor); Rotation rotation;
    const char* text="move 7 10 deg\nhome 8 2\nwait 10";
    CHECK(queue.start(text,std::strlen(text),1,rotation,hw::now).code==202);
    for(unsigned i=0;i<10;++i) {hw::now+=20;queue.poll(hw::now);}
    CHECK(queue.state()==QueueState::Done); CHECK(r.motor.movementGeneration()==2);
    CHECK(!r.motor.affectedAxesStationary()); CHECK(queue.clearControlState().code<300);
    CHECK(!r.motor.operationBusy()); CHECK(r.motor.hasUnsettledMotionEvidence());
    r.fastQueries(); r.prove(); CHECK(r.motor.otaMotionSafe());
}
void demo_stop_window() {
    Rig r;
    for(uint8_t id=1;id<=5;++id) r.motor.demoWatch(id,true);
    const auto window=r.motor.stopEvidenceWindow();
    CHECK(window>=600 && window<=5000);
    r.feed(1); hw::frame(1,{0x3A,1,0x6B}); r.motor.poll(false);
    const uint32_t stopAt=hw::now;
    CHECK(!r.motor.stopEvidence(1,stopAt,window));
    r.feed(1);
    CHECK(!r.motor.stopEvidence(1,stopAt,window)); // Pre-Stop flags are not proof.
    hw::frame(1,{0x3A,1,0x6B}); r.motor.poll(false);
    CHECK(r.motor.stopEvidence(1,stopAt,window));
    CHECK(!r.motor.stopEvidence(2,stopAt,window));
    const auto stamp=hw::now;
    hw::now=stamp+601;
    CHECK(!r.motor.snapshot(1).positionValid && !r.motor.snapshot(1).velocityValid);
    uint8_t flags; uint32_t age;
    CHECK(!r.motor.demoFlags(1,flags,age));
    CHECK(r.motor.stopEvidence(1,stopAt,window));
    auto c=r.motor.queries().config(); c.queriesPerSecond=100;c.gapMs=2;c.timeoutMs=20;
    CHECK(r.motor.queries().configure(c));
    CHECK(r.motor.stopEvidenceWindow()<window);
    CHECK(r.motor.stopEvidence(1,stopAt,window)); // Captured budget does not shrink retroactively.
    hw::now=stamp+window; CHECK(r.motor.stopEvidence(1,stopAt,window));
    ++hw::now; CHECK(!r.motor.stopEvidence(1,stopAt,window));
    c.queriesPerSecond=1;c.gapMs=1000;c.cooldownMs=1000;CHECK(r.motor.queries().configure(c));
    CHECK(r.motor.stopEvidenceWindow()==5000);
    CHECK(!r.motor.stopEvidence(1,stopAt,window)); // New budget cannot revive old proof.
    CHECK(!r.motor.stopEvidence(1,stopAt,5001));
    CHECK(!r.motor.stopEvidence(1,stopAt,599));
    r.feed(1,30);hw::frame(1,{0x3A,1,0x6B});r.motor.poll(false);
    CHECK(!r.motor.stopEvidence(1,stopAt,window));
    r.feed(1);CHECK(r.motor.stopEvidence(1,stopAt,window));
    hw::now=UINT32_MAX-2;
    const auto wrappedStop=hw::now;
    hw::now=2;r.feed(1,0,false);hw::frame(1,{0x3A,1,0x6B});r.motor.poll(false);
    CHECK(r.motor.stopEvidence(1,wrappedStop,window));
}
int main() {
    struct Case {const char* name;void(*run)();};
    const Case cases[]={{"harmless/held-enable",harmless},{"inherit/reset/raw/owner",inheritance},
        {"fresh/moving/stale/bus-off",freshness},{"pre-Stop query",old_query},
        {"same-poll duplicate is not query proof",duplicate_response},{"failed Stop",stop_failure},
        {"cached Stop is neither motion nor Stop proof",cached_stop},
        {"raw known transports",known_transport},{"structured paths",structured},{"partial submission",partial},
        {"sticky unknown/debug available",unknown},{"no TX/no risk",no_tx},{"shared multi-axis budget",multi_budget},
        {"control retirement separate from OTA",release_control},
        {"released history does not consume current proof budget",current_query_budget},
        {"default five-axis queue natural retirement",default_five_natural},
        {"default five-axis Stop OTA",default_five_stop},
        {"Demo/Page competition and 5000ms cap",competing_cap},
        {"latency missing moving timeout",latency_missing_moving},
        {"window expansion cannot revive and 600ms minimum",expansion_and_minimum},
        {"receipt TTL survives shrink but expiry survives expansion",shrink_expand_receipt},
        {"millis wrap/zero Stop",wrap},{"actual queue lifecycle",queue_lifecycle},
        {"Demo Stop budget frozen/bounded/post-Stop only",demo_stop_window}};
    unsigned failed=0;
    for(const auto& test:cases) {
        try {test.run();std::cout<<"PASS "<<test.name<<'\n';}
        catch(const std::exception& e) {++failed;std::cout<<"FAIL "<<test.name<<": "<<e.what()<<'\n';}
    }
    std::cout<<sizeof(cases)/sizeof(cases[0])<<" groups, "<<checks<<" checks, "<<failed<<" failures\n";
    std::cout<<"Production MotorControl/CommandQueue/X42sProtocol; TWAI/clock only are substitutes. No main, physical feedback or OTA upload.\n";
    return failed?1:0;
}
