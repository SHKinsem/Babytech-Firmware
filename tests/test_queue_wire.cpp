// Only the hardware boundary is fake. Parser, queue, controller and production
// X42sProtocol all run together, including real packet splitting and delays.
#include "CommandQueue.h"
#include <cassert>
#include <deque>
#include <vector>
#include <cstdio>
#include <cstring>

FakeSerial Serial;
static uint32_t now=0;
unsigned long millis() { return now; }
void delay(unsigned long ms) { now+=ms; }
static std::vector<twai_message_t> tx;
static std::deque<twai_message_t> rx;
static size_t failPacket=0;
esp_err_t twai_transmit(const twai_message_t* frame,uint32_t timeout) {
    assert(timeout<=50); tx.push_back(*frame);
    return failPacket==tx.size() ? -1 : ESP_OK;
}
esp_err_t twai_receive(twai_message_t* frame,uint32_t) {
    if (rx.empty()) return -1;
    *frame=rx.front(); rx.pop_front(); return ESP_OK;
}
struct Rotation : motion::QueueRotationSource {
    bool rotationMm(uint8_t id,double& out) const override {out=id==1?8:40;return true;}
};
static void reply(uint8_t id,uint8_t code) {
    twai_message_t frame;
    frame.identifier=uint32_t(id)<<8; frame.extd=1; frame.data_length_code=3;
    frame.data[0]=0xCD; frame.data[1]=code; frame.data[2]=0x6B; rx.push_back(frame);
}
static bool contains(const String& s,const char* text) {return strstr(s.c_str(),text)!=nullptr;}
static void feedback(uint8_t id,uint8_t function,int32_t value) {
    twai_message_t frame={};frame.identifier=uint32_t(id)<<8;frame.extd=1;
    frame.data[0]=function;
    if(function==0x3A || function==0x3B) {frame.data[1]=uint8_t(value);frame.data[2]=0x6B;frame.data_length_code=3;}
    else {
        const unsigned bytes=function==0x35?2:4;
        frame.data[1]=value<0?1:0;const uint32_t magnitude=value<0?uint32_t(-int64_t(value)):uint32_t(value);
        for(unsigned i=0;i<bytes;++i) frame.data[2+i]=uint8_t(magnitude>>((bytes-i-1)*8));
        frame.data[2+bytes]=0x6B;frame.data_length_code=bytes+3;
    }
    rx.push_back(frame);
}

static void syncWire(unsigned productGroup = 0) {
    tx.clear();rx.clear();failPacket=0;now=100;
    Rotation rotation;motion::MotorControl motor;motion::CommandQueue queue(motor);
    assert(motor.begin(4,5,500000));
    motion::SyncSettings settings;settings.tolerance.progress=.2;settings.tolerance.timeMs=50;
    settings.feedbackTimeoutMs=5000;settings.prepareTimeoutMs=10000;settings.stopTimeoutMs=2000;
    settings.responseBudgetMs=20;settings.completionTenths=2;
    if (!productGroup) assert(queue.setSyncSettings(settings));
    const char* program = productGroup == 1 ?
        "sync begin trigger\nmove 6 -216 deg 100 300 300 200\nmove 7 1000 deg 400 1500 1500 500\nsync end\nwait 0" :
        productGroup == 2 ?
        "sync begin trigger\nmove 6 -360 deg 100 300 300 200\nmove 7 -2400 deg 500 1500 1500 500\nsync end\nwait 0" :
        "sync begin\nmove 6 360 deg 1 60 60 800\nmove 7 -180 deg 1 60 60 800\nsync end\nwait 0";
    motion::QueueProgram parsed; motion::QueueError error;
    assert(motion::parseQueueProgram(program,strlen(program),rotation,parsed,error));
    if (productGroup) assert(queue.startDemo(parsed,now).code==202);
    else assert(queue.start(program,strlen(program),1,rotation,now).code==202);
    size_t handled=0;unsigned triggers=0,movePackets=0;bool cached[256]={};uint32_t triggered=0;
    motion::SyncPlan plan;
    assert(!motion::planSync(parsed.steps+1,2,productGroup ? motion::productSyncDefaults().tolerance : settings.tolerance,plan));
    const auto curve=plan.common;
    for(unsigned loops=0;queue.active() && loops<8000;++loops) {
        now+=10;motor.poll(false);queue.poll(now);motor.dispatchQueries();
        while(handled<tx.size()) {
            const auto frame=tx[handled++];const uint8_t id=uint8_t(frame.identifier>>8),field=frame.data[0];
            if(field==0xCD) {
                ++movePackets;
                if((frame.identifier&0xFF)==1) assert(frame.data[6]==1); // sync bit preserved by real packet split
                if((frame.identifier&0xFF)==2) {cached[id]=true;reply(id,2);reply(id,0x9F);}
            } else if(field==0xFF) {
                assert(cached[6] && cached[7]);assert(frame.identifier==0 && frame.data_length_code==3);
                assert(frame.data[1]==0x66 && frame.data[2]==0x6B);++triggers;triggered=now;
            } else if(motion::CanQueryScheduler::supported(field)) {
                assert(id==6 || id==7); // no selected/default motor queries in exclusive group
                const int32_t target=parsed.steps[id==6?1:2].distanceTenths;
                const auto sample=curve.at(triggered?(now-triggered)/1000.0:0);
                const int32_t value=field==0x33?(cached[id]?target:0):
                    field==0x36?int32_t(round(target*sample.position)):
                    field==0x35?int32_t(round(target*sample.velocity/6)):field==0x3A?1:0;
                feedback(id,field,value);
            } else {fprintf(stderr,"unexpected frame %02X: %s\n",field,queue.statusJson().c_str());assert(false);} // no hidden enable, stop, retry, or streamed motion
        }
    }
    assert(queue.state()==motion::QueueState::Done && triggers==1 && movePackets==6);
    assert(contains(queue.statusJson(),"\"done\":true"));
    assert(contains(queue.statusJson(),"\"motionComplete\":true"));
    assert(motor.queries().statistics().queries<=now/100+1);
    if (productGroup) assert(!queue.syncSettings().valid());
    puts("PASS real sync wire: budgeted fresh reads, target change corroboration, both cache ACKs, batched 02/9F, 6 CD packets, single FF, independent completion");
}
static void referenceClassification() {
    tx.clear(); rx.clear(); failPacket = 0;
    Rotation rotation; motion::MotorControl motor; motor.begin(2, 3, 500000); motion::CommandQueue queue(motor);
    struct Case { const char* program; bool changes; };
    const Case cases[] = {
        {"wait 20", false}, {"enable 1", false}, {"hex 01 36 6B", false},
        {"can ext 100 36 6B", false}, {"hex 01 45 66 00 03 20 6B", false},
        {"hex 01 22 6B", false}, {"can ext 100 22 6B", false},
        {"hex 01 22 00 6B", true}, {"can ext 100 22 00", true},
        {"hex 01 11 18 36 00 64 6B", false},
        {"hex 01 4C AE 00 02 00 00 1E 00 00 27 10 01 2C 03 E8 00 3C 00 6B", false},
        {"hex 01 0A 6D 6B", true}, {"can ext 100 93 88 00 6B", true},
        {"hex 01 46 69 00 01 6B", true}, {"can std 123 DE AD", true},
        {"hex 01 FF 66 6B", true}, {"move 1 10 deg", true}, {"home 1 2", true},
        {"torque 1 200", true}, {"velocity 1 10", true},
    };
    for (const auto& c : cases) {
        const auto before = tx.size();
        assert(queue.start(c.program, strlen(c.program), 1, rotation, now).code == 202);
        assert(queue.changesReference() == c.changes && tx.size() == before);
        queue.cancel("fixture_cleanup");
    }
    puts("PASS accepted-plan reference classification: read/enable/parameters isolated, motion/origin/mode/unknown raw retained");
}
static void allStopDuringSync() {
    tx.clear();rx.clear();failPacket=0;now=100;
    Rotation rotation;motion::MotorControl motor;motion::CommandQueue queue(motor);
    assert(motor.begin(4,5,500000));
    motion::SyncSettings settings;settings.tolerance.progress=.2;settings.tolerance.timeMs=50;
    settings.feedbackTimeoutMs=5000;settings.prepareTimeoutMs=10000;settings.stopTimeoutMs=2000;
    settings.responseBudgetMs=20;settings.completionTenths=2;
    assert(queue.setSyncSettings(settings));
    const char* program="sync begin\nmove 6 360 deg 1 60 60 800\nmove 7 -180 deg 1 60 60 800\nsync end";
    assert(queue.start(program,strlen(program),1,rotation,now).code==202);
    queue.poll(now);
    assert(contains(queue.statusJson(),"\"phase\":\"checking\""));
    assert(queue.cancel("stopped").code==202);
    bool broadcastStop=false,broadcastHomeAbort=false;
    for(const auto& frame:tx) {
        if(frame.identifier==0 && frame.data[0]==0xFE) broadcastStop=true;
        if(frame.identifier==0 && frame.data[0]==0x9C) broadcastHomeAbort=true;
    }
    assert(broadcastStop && broadcastHomeAbort);
    puts("PASS all-stop during sync also reaches nonmember motors");
}
static void parserChecks() {
    Rotation rotation;motion::QueueProgram parsed;motion::QueueError error;
    const auto parse=[&](const char* text){return motion::parseQueueProgram(text,strlen(text),rotation,parsed,error);};
    assert(parse("sync begin\nmove 1 360\nmove 2 -180\nsync end"));
    assert(parsed.count==4 && parsed.steps[0].groupSize==2 && parsed.steps[2].line==3);
    for(const char* bad:{"sync end","sync begin\nmove 1 90","sync begin\nmove 1 90\nsync end",
        "sync begin\nmove 1 90\nmove 1 -90\nsync end","sync begin\nmove 0 90\nmove 2 90\nsync end",
        "sync begin\nsync begin\nmove 1 90\nmove 2 90\nsync end\nsync end",
        "sync begin\nmove 1 90 await\nmove 2 90\nsync end","sync begin\nhex 01 CD 6B\nmove 2 90\nsync end"}) {
        assert(!parse(bad));assert(parsed.count==0);
    }
    assert(parse("helix 1 2 3 2 1 1 -1 1 60 60 800 0.1"));
    assert(parsed.count==4 && parsed.steps[1].distanceTenths==10800 && parsed.steps[2].distanceTenths==-540);
    assert(parsed.steps[0].helixTravelMm==6);
    assert(!parse("helix 1 2 3 2 1 1 0 1 60 60 800 0.1"));
    puts("PASS board sync/helix parser: atomic grammar, duplicate/broadcast/raw/await rejection, board geometry expansion");
}
int main() {
    parserChecks();
    Rotation rotation;
    motion::MotorControl motor;
    motion::CommandQueue queue(motor);
    assert(motor.begin(4,5,500000)); motor.setAutoQueriesEnabled(false);
    const char* program="move 1 20 mm\nmove 2 -20 mm";
    assert(queue.start(program,strlen(program),1,rotation,now).code==202);
    queue.poll(now); queue.poll(now); queue.poll(now);
    assert(queue.state()==motion::QueueState::Done && tx.size()==6);
    const uint8_t expected[][8]={
        {0xCD,0,0,60,0,60,1,44}, {0xCD,0,0,0x23,0x28,2,0,3}, {0xCD,0x20,0x6B},
        {0xCD,1,0,60,0,60,1,44}, {0xCD,0,0,7,8,2,0,3}, {0xCD,0x20,0x6B}
    };
    for (size_t i=0;i<6;++i) {
        assert(tx[i].identifier==((i<3?0x100u:0x200u)+i%3));
        assert(tx[i].extd && tx[i].ss && !tx[i].rtr);
        assert(tx[i].data_length_code==(i%3==2?3:8));
        assert(!memcmp(tx[i].data,expected[i],tx[i].data_length_code));
    }
    // Neither node is selected. ACKs after queue completion must still appear.
    reply(1,2); reply(2,0xE2); now+=10; motor.poll();
    auto status=queue.statusJson();
    assert(contains(status,"driver_accepted") && contains(status,"driver_rejected"));
    assert(queue.state()==motion::QueueState::Done && tx.size()==6);
    reply(2,2); motor.poll(); assert(contains(queue.statusJson(),"driver_rejected"));
    // New run, same opcode: no invented attribution of a late earlier response.
    assert(queue.start(program,strlen(program),2,rotation,now).code==202);
    for(int i=0;i<6;++i) queue.poll(now);
    reply(1,2); motor.poll(); assert(contains(queue.statusJson(),"association_uncertain"));
    // Unanswered motion is not replayed or stopped implicitly. Independent
    // stationary evidence may issue budgeted position/velocity reads.
    const char* silent="move 3 90";
    assert(queue.start(silent,strlen(silent),1,rotation,now).code==202);
    queue.poll(now); queue.poll(now); const size_t sent=tx.size();
    now+=2001; motor.poll();
    assert(contains(queue.statusJson(),"receive_unconfirmed"));
    assert(tx.size()>sent);
    for (size_t i=sent;i<tx.size();++i) {
        const auto& frame=tx[i];
        const uint8_t id=uint8_t(frame.identifier>>8);
        assert(id>=1 && id<=3 && frame.identifier==(uint32_t(id)<<8));
        assert(frame.extd && frame.ss && !frame.rtr && frame.data_length_code==2);
        assert((frame.data[0]==0x36 || frame.data[0]==0x35) && frame.data[1]==0x6B);
    }
    // Middle packet failure terminates this logical send; motor 5 is not sent.
    const char* failure="move 4 90\nmove 5 90";
    assert(queue.start(failure,strlen(failure),1,rotation,now).code==202);
    failPacket=tx.size()+2; queue.poll(now); queue.poll(now);
    assert(queue.state()==motion::QueueState::Failed && tx.size()==failPacket);
    assert(contains(queue.statusJson(),"send_failed"));
    puts("PASS real queue wire: two IDs, mm conversion, direction, all CD packets, passive ACK/reject/late/missing, partial TX");
    syncWire();
    syncWire(1); syncWire(2);
    referenceClassification();
    allStopDuringSync();
}
