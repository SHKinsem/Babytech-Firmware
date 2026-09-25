#include "BoardEndpoint.h"
#include "BoardClient.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace babytech::v2;
struct Fake : Backend {
    bool busyValue=false,faultValue=false,stoppedValue=false,stopSentValue=false;
    bool stopTransmit=true,unverifiedValue=false;
    int moves=0,stops=0,enables=0; Outcome op=Outcome::Accepted;
    bool unverifiedMode()const override{return unverifiedValue;}
    bool busy()const override{return busyValue;}
    bool fault()const override{return faultValue;}
    bool motorsAvailable()const override{return true;}
    Reason startMove(const Parameters&)override{++moves;op=Outcome::Accepted;return Reason::None;}
    Reason enable(uint8_t,bool)override{++enables;op=Outcome::Accepted;return Reason::None;}
    Outcome operation(Reason& r)const override{r=Reason::None;return op;}
    void stop()override{++stops;op=Outcome::Cancelled;stopSentValue=stopTransmit;}
    bool stopped()const override{return unverifiedValue ? stopSentValue : stoppedValue;}
    void watch(uint8_t)override{}
    size_t motorFeedback(uint8_t,uint8_t* p,size_t)const override{std::memset(p,0,23);return 23;}
};
Frame run(uint32_t seq=1,uint64_t session=7) {
    Frame f;f.cmd=Cmd::Exec;f.session=session;f.sequence=seq;
    Writer w(f.payload,kMaxPayload);w.put(9,8);w.put(kStage,1);w.put(1,2);w.put(kRun,2);w.put(1,4);f.length=w.size();return f;
}
Frame stop(uint32_t seq=2,uint64_t session=7) {
    Frame f;f.cmd=Cmd::Stop;f.session=session;f.sequence=seq;
    Writer w(f.payload,kMaxPayload);w.put(0,8);f.length=w.size();return f;
}
Frame write(uint32_t seq,uint32_t revision,uint16_t field,uint32_t value) {
    Frame f;f.cmd=Cmd::Write;f.session=7;f.sequence=seq;
    Writer w(f.payload,kMaxPayload);w.put(9,8);w.put(revision,4);w.put(1,1);
    w.put(kStage,1);w.put(1,2);w.put(field,2);w.put(4,2);w.put(value,4);f.length=w.size();return f;
}
void expect(const Frame& f,Outcome o,Reason reason=Reason::None) {
    Outcome actual;Reason r;assert(result(f,actual,r));assert(actual==o);assert(r==reason);
}
void framing() {
    const Frame f=run();uint8_t bytes[kMaxFrameSize];size_t n=encode(f,bytes,sizeof(bytes));
    assert(n==38);assert(bytes[0]==0x42 && bytes[2]==2 && bytes[3]==3 && bytes[5]==17);
    for (size_t split=0;split<=n;++split) {
        Parser p;Frame got;int count=0;
        for(size_t i=0;i<split;++i)count+=p.push(bytes[i],got);
        for(size_t i=split;i<n;++i)count+=p.push(bytes[i],got);
        assert(count==1 && sameRequest(got,f));
    }
    Parser p;Frame got;int count=0;
    for(int j=0;j<2;++j)for(size_t i=0;i<n;++i)count+=p.push(bytes[i],got);
    assert(count==2);
    for(size_t bad=0;bad<n;++bad) {
        Parser parser;uint8_t corrupt[kMaxFrameSize];std::memcpy(corrupt,bytes,n);corrupt[bad]^=0x80;
        bool delivered=false;
        for(size_t i=0;i<n;++i)delivered|=parser.push(corrupt[i],got);
        assert(!delivered);
        // Explicit byte timeout permits recovery even from a corrupted length.
        parser.reset();int valid=0;for(size_t i=0;i<n;++i)valid+=parser.push(bytes[i],got);
        assert(valid==1);
    }
    Frame full=f;full.length=kMaxPayload;assert(encode(full,bytes,sizeof(bytes))==kMaxFrameSize);
    full.length=kMaxPayload+1;assert(encode(full,bytes,sizeof(bytes))==0);
}
void endpointTests() {
    Fake b;Endpoint e(b);e.begin(9);Frame out,event;
    auto q=write(1,1,3,100);assert(e.handle(q,0,out));expect(out,Outcome::Ok);assert(e.parameters().speed==100);
    assert(e.handle(q,1,out));expect(out,Outcome::Ok);assert(e.status(1).revision==2);
    auto conflict=q;conflict.payload[23]^=1;e.handle(conflict,2,out);expect(out,Outcome::Rejected,Reason::RequestConflict);
    q=write(2,1,3,80);e.handle(q,3,out);expect(out,Outcome::Rejected,Reason::ConfigMismatch);
    q=write(3,2,1,256);e.handle(q,4,out);expect(out,Outcome::Rejected,Reason::InvalidParam);assert(e.parameters().motor==1);
    // Two-field invalid batch must not partially change a valid first field.
    q=write(4,2,3,150);q.payload[12]=2;Writer tail(q.payload+q.length,kMaxPayload-q.length);
    tail.put(kStage,1);tail.put(1,2);tail.put(1,2);tail.put(4,2);tail.put(0,4);q.length+=tail.size();
    e.handle(q,5,out);expect(out,Outcome::Rejected,Reason::InvalidParam);assert(e.parameters().speed==100);
    // Duplicate field and trailing bytes both rejected.
    q=write(5,2,3,150);q.payload[q.length++]=0;e.handle(q,5,out);expect(out,Outcome::Rejected,Reason::InvalidParam);
    q=readField(7,6,9,kSystem,0,kStatus);e.handle(q,6,out);expect(out,Outcome::Ok);
    Reader r(out.payload+11,out.length-11);Status state;assert(readStatus(r,state)&&state.revision==2);
    q=write(1,1,3,100);e.handle(q,7,out); // saved outcome or expired, never another write
    assert(e.status(7).revision==2);
    q=run(7);q.payload[13]=2;e.handle(q,8,out);expect(out,Outcome::Accepted);assert(b.moves==1);
    e.handle(q,9,out);expect(out,Outcome::Accepted);assert(b.moves==1);
    auto newer=run(8);newer.payload[13]=2;e.handle(newer,10,out);expect(out,Outcome::Rejected,Reason::Busy);
    assert(e.handle(stop(9),11,out));expect(out,Outcome::Accepted);assert(b.stops==1);
    assert(e.tick(12,event));expect(event,Outcome::Cancelled);assert(event.sequence==7);
    assert(!e.tick(3010,event));assert(e.tick(3011,event));expect(event,Outcome::Failed,Reason::StopUnconfirmed);
    e.handle(stop(9),4000,out);expect(out,Outcome::Failed,Reason::StopUnconfirmed);assert(b.stops==1);
    q=readField(7,10,0,kSystem,0,kStatus);e.handle(q,4001,out);expect(out,Outcome::Rejected,Reason::BootMismatch);
    q=readField(7,11,9,kSystem,0,kInfo);q.payload[8]=3;
    for(int i=0;i<2;++i){std::memcpy(q.payload+q.length,q.payload+9,5);q.length+=5;}
    // 3 INFO values fit; 3 STATUS values do not and cannot be truncated.
    for(int i=0;i<3;++i)q.payload[12+i*5]=kStatus;
    e.handle(q,4002,out);expect(out,Outcome::Rejected,Reason::InvalidParam);
    q=readField(7,12,9,kSystem,0,kInfo);q.cmd=Cmd(99);e.handle(q,4003,out);expect(out,Outcome::Rejected,Reason::Unsupported);
    q.kind=Kind::Response;assert(!e.handle(q,4004,out));
}
void lifecycle() {
    Fake b;Endpoint e(b);e.begin(9);Frame out,event;
    const auto q=run();e.handle(q,0,out);expect(out,Outcome::Accepted);
    assert(!e.tick(1499,event));assert(e.tick(1500,event));expect(event,Outcome::Failed,Reason::Timeout);assert(b.stops==1);
    e.handle(q,1501,out);expect(out,Outcome::Failed,Reason::Timeout);assert(b.moves==1);
    Fake b2;Endpoint e2(b2);e2.begin(9);e2.handle(q,0,out);
    auto heartbeat=readField(7,2,9,kSystem,0,kStatus);e2.handle(heartbeat,1000,out);
    assert(!e2.tick(2000,event));b2.op=Outcome::Done;assert(e2.tick(2001,event));expect(event,Outcome::Done);
    e2.handle(q,2002,out);expect(out,Outcome::Done);assert(b2.moves==1);
    // Other sessions cannot keep the owning session's action alive.
    Fake b3;Endpoint e3(b3);e3.begin(9);e3.handle(q,0,out);
    e3.handle(readField(8,1,9,kSystem,0,kStatus),1000,out);
    assert(e3.tick(1500,event));expect(event,Outcome::Failed,Reason::Timeout);
    // Fill sessions; STOP remains available. Reboot rejects old target boot.
    e3.handle(readField(9,1,9,kSystem,0,kInfo),1501,out);
    e3.handle(readField(10,1,9,kSystem,0,kInfo),1502,out);
    e3.handle(readField(11,1,9,kSystem,0,kInfo),1503,out);expect(out,Outcome::Rejected,Reason::Busy);
    e3.handle(stop(1,11),1504,out);expect(out,Outcome::Accepted);b3.stoppedValue=true;
    assert(e3.tick(1505,event));expect(event,Outcome::Done);
    Endpoint reboot(b3);reboot.begin(10);reboot.handle(q,0,out);expect(out,Outcome::Rejected,Reason::BootMismatch);
    // Cache eviction cannot resurrect a motion request.
    for(uint32_t seq=3;seq<15;++seq)e2.handle(write(seq,1,3,0),3000+seq,out);
    e2.handle(q,4000,out);expect(out,Outcome::Rejected,Reason::ResultExpired);assert(b2.moves==1);
}
void unverifiedEndpoint() {
    Fake b;b.unverifiedValue=true;b.busyValue=true;b.faultValue=true;
    Endpoint e(b);e.begin(9);Frame out,event;
    // Revision, fault, busy and strict travel/speed policy do not block writes.
    auto q=write(1,999,3,65535);
    e.handle(q,0,out);expect(out,Outcome::Ok);
    assert(e.parameters().speed==65535 && e.status(0).revision==2);
    q=write(2,0,5,0);e.handle(q,1,out);expect(out,Outcome::Ok);
    q=write(3,0,1,255);e.handle(q,2,out);expect(out,Outcome::Ok);
    q=write(4,0,2,0x80000000u);e.handle(q,3,out);expect(out,Outcome::Ok);
    assert(e.parameters().angle==INT32_MIN);
    // An integer that cannot fit a CAN field is still invalid; no narrowing.
    q=write(5,0,6,65536);e.handle(q,4,out);
    expect(out,Outcome::Rejected,Reason::InvalidParam);
    q=write(6,0,1,256);e.handle(q,5,out);
    expect(out,Outcome::Rejected,Reason::InvalidParam);
    const auto move=run(7);e.handle(move,6,out);expect(out,Outcome::Done);
    assert(b.moves==1 && !e.busy());
    assert(!e.tick(kLinkTimeoutMs+7,event) && b.stops==0);
    e.handle(move,kLinkTimeoutMs+8,out);expect(out,Outcome::Done);assert(b.moves==1);
    auto another=run(8);another.payload[13]=0; // stale revision is accepted
    e.handle(another,kLinkTimeoutMs+9,out);expect(out,Outcome::Done);assert(b.moves==2);
    auto conflict=move;conflict.payload[13]^=1;
    e.handle(conflict,kLinkTimeoutMs+10,out);
    expect(out,Outcome::Rejected,Reason::RequestConflict);assert(b.moves==2);
    auto wrongBoot=run(9);wrongBoot.payload[0]^=1;
    e.handle(wrongBoot,kLinkTimeoutMs+10,out);
    expect(out,Outcome::Rejected,Reason::BootMismatch);assert(b.moves==2);
    auto malformed=run(10);malformed.length++;
    e.handle(malformed,kLinkTimeoutMs+10,out);
    expect(out,Outcome::Rejected,Reason::InvalidParam);assert(b.moves==2);
    const auto stopRequest=stop(11);
    e.handle(stopRequest,kLinkTimeoutMs+11,out);expect(out,Outcome::Done);
    assert(b.stops==1);
    e.handle(stopRequest,kLinkTimeoutMs+12,out);expect(out,Outcome::Done);assert(b.stops==1);
    // A verified request already on the wire is settled on mode change, not
    // timed out or stopped for lack of feedback/heartbeats.
    Fake switching;Endpoint switched(switching);switched.begin(9);
    switched.handle(run(),0,out);expect(out,Outcome::Accepted);
    switching.unverifiedValue=true;
    switched.handle(run(),1,out);expect(out,Outcome::Done);assert(switching.moves==1);
    switched.handle(run(2),2,out);expect(out,Outcome::Done);assert(switching.moves==2);
    assert(switched.tick(kLinkTimeoutMs+1,event));expect(event,Outcome::Done);
    assert(switching.stops==0 && !switched.busy());
    switched.handle(run(),kLinkTimeoutMs+2,out);expect(out,Outcome::Done);
    assert(switching.moves==2);

    // A STOP already Accepted before the switch keeps its own terminal event.
    Fake stopSwitch;Endpoint stops(stopSwitch);stops.begin(9);
    stops.handle(stop(1),0,out);expect(out,Outcome::Accepted);
    stopSwitch.unverifiedValue=true;
    stops.handle(stop(1),1,out);expect(out,Outcome::Done);
    stops.handle(stop(2),2,out);expect(out,Outcome::Done);
    assert(stopSwitch.stops==2); // old pending STOP did not block a new one
    assert(stops.tick(3,event));expect(event,Outcome::Done);
    assert(event.sequence==1 && !stops.tick(4,event));
    stops.handle(stop(1),5,out);expect(out,Outcome::Done);
    assert(stopSwitch.stops==2);

    Fake mixedStops;mixedStops.stopTransmit=false;
    Endpoint independent(mixedStops);independent.begin(9);
    independent.handle(stop(1),0,out);expect(out,Outcome::Accepted);
    mixedStops.unverifiedValue=true;mixedStops.stopTransmit=true;
    independent.handle(stop(2),1,out);expect(out,Outcome::Done);
    independent.handle(stop(1),2,out);expect(out,Outcome::Failed,Reason::NotReady);
    assert(independent.tick(3,event));expect(event,Outcome::Failed,Reason::NotReady);
    assert(event.sequence==1 && !independent.tick(4,event));
    independent.handle(stop(1),5,out);expect(out,Outcome::Failed,Reason::NotReady);
    assert(mixedStops.stops==2);

    // An already accepted STOP cancels the old supervised motion owner. Mode
    // switching must emit both terminal events rather than lose either one.
    Fake both;Endpoint combined(both);combined.begin(9);
    combined.handle(run(),0,out);expect(out,Outcome::Accepted);
    combined.handle(stop(2),1,out);expect(out,Outcome::Accepted);
    both.unverifiedValue=true;
    combined.handle(run(),2,out);expect(out,Outcome::Cancelled);
    assert(combined.tick(3,event));expect(event,Outcome::Cancelled);
    assert(event.sequence==1);
    assert(combined.tick(4,event));expect(event,Outcome::Done);
    assert(event.sequence==2 && !combined.tick(5,event));

    Fake lateStop;Endpoint stoppedMove(lateStop);stoppedMove.begin(9);
    stoppedMove.handle(run(),0,out);expect(out,Outcome::Accepted);
    lateStop.unverifiedValue=true;
    stoppedMove.handle(stop(2),1,out);expect(out,Outcome::Done);
    stoppedMove.handle(run(),2,out);expect(out,Outcome::Cancelled);
    assert(stoppedMove.tick(3,event));expect(event,Outcome::Cancelled);
    assert(event.sequence==1 && !stoppedMove.tick(4,event));
}
void clientTests() {
    {
        Client rejected; rejected.begin(99); Frame query, next;
        assert(rejected.query(0,query));
        rejected.receive(reply(query,Outcome::Rejected,Reason::Busy),0);
        assert(!rejected.query(1,next)); // rejected discovery cannot flood UART
        assert(rejected.query(500,next));
    }
    Fake b;Endpoint e(b);e.begin(9);Client c;c.begin(7);Frame q,out,event;
    auto exchange=[&](uint32_t now){assert(c.query(now,q,true));assert(e.handle(q,now,out));c.receive(out,now);};
    exchange(0);assert(c.boot()==9 && !c.connected(0));exchange(500);assert(c.connected(500));
    exchange(1000);assert(c.parameterRevision()==1);exchange(1500);
    q=run();assert(c.submit(q,1600));e.handle(q,1600,out); // deliberately lose ACCEPTED
    b.op=Outcome::Done;assert(e.tick(1700,event)); // deliberately lose EVENT
    exchange(2000);exchange(2500);assert(!c.controlBusy());assert(c.controlOutcome()==Outcome::Done);
    c.receive(reply(q,Outcome::Accepted),2600);assert(c.controlOutcome()==Outcome::Done);
    // Recovery of a WRITE reply comes from its exact key, not inferred revision.
    q=write(1,1,3,90);assert(c.submit(q,2700));e.handle(q,2700,out);
    for(uint32_t now=3000;now<=4500 && c.controlBusy();now+=500)exchange(now);
    assert(!c.controlBusy() && c.controlOutcome()==Outcome::Ok);
    // Discovery after a long outage of the same board resumes status polling.
    exchange(9000);exchange(9500);assert(c.connected(9500));
    q=run();q.payload[13]=2;assert(c.submit(q,9600));e.handle(q,9600,out);c.receive(out,9600);
    Endpoint reboot(b);reboot.begin(10);
    for(uint32_t now=10000;now<=12500;now+=500){if(c.query(now,q,true)&&reboot.handle(q,now,out))c.receive(out,now);}
    assert(c.boot()==10 && c.connected(12500));assert(!c.controlReplied() && c.controlReason()==Reason::BootMismatch);
    // STOP is available while disconnected, repeated submits preserve its key.
    q=stop();assert(c.submit(q,20000));Frame again=stop();assert(c.submit(again,20001));assert(sameRequest(q,again));
}
int main(){framing();endpointTests();lifecycle();unverifiedEndpoint();clientTests();std::puts("PASS: v2 framing, atomic writes, lifecycle, unverified send outcome, dedup, stop, reconnect and client recovery");}
