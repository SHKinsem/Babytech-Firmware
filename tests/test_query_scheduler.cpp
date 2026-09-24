#include "CanQueryScheduler.h"
#include <cassert>
#include <cstdio>
#include <vector>
using S=motion::CanQueryScheduler;
struct Tx {uint8_t id,field;};
static std::vector<Tx> tx;
static bool fail=false;
static bool send(void*,uint8_t id,uint8_t field) {tx.push_back({id,field});return !fail;}
static void request(S& s,uint8_t id,S::Owner owner,uint32_t now=0,uint8_t priority=1) {
    assert(s.demand(id,0x36,owner,100,60000,priority,now));
}
int main() {
    S s;
    request(s,1,S::Controller);request(s,1,S::Await);request(s,1,S::Page);
    assert(s.dispatch(0,send,nullptr));
    assert(!s.dispatch(100,send,nullptr) && tx.size()==1 && s.inflight()==1);
    s.receive(1,0x35,101);assert(s.inflight()==1); // unrelated response
    s.receive(1,0x36,120);assert(s.inflight()==0 && s.evidence(1,0x36).received);
    assert(s.statistics().latencyMaxMs==120);
    assert(s.dispatch(120,send,nullptr));
    request(s,2,S::Page);request(s,3,S::Controller);
    assert(s.dispatch(220,send,nullptr));
    assert(!s.dispatch(320,send,nullptr) && s.inflight()==2);
    // Offline slot times out; it does not block other motors or immediately retry.
    assert(s.dispatch(620,send,nullptr));assert(tx.back().id==3);
    assert(s.statistics().unanswered==1);
    assert(!s.dispatch(621,send,nullptr));
    assert(!s.dispatch(622,send,nullptr)); // never catch up overdue credits
    s.receive(3,0x36,650);
    s.noteTraffic(720);assert(!s.dispatch(720,send,nullptr));
    assert(!s.dispatch(819,send,nullptr));
    s.release(S::Page);s.release(S::Controller);s.release(S::Await);
    assert(!s.dispatch(820,send,nullptr));
    // Same global budget while an exclusive sync phase is active.
    request(s,4,S::Page,900);request(s,5,S::Sync,900);
    s.exclusiveSync(true);assert(s.dispatch(900,send,nullptr));assert(tx.back().id==5);
    s.receive(5,0x36,950);s.release(S::Sync);
    assert(!s.dispatch(1000,send,nullptr));
    s.exclusiveSync(false);assert(s.dispatch(1100,send,nullptr));assert(tx.back().id==4);
    // A hundred browser refreshes renew one lease, never add a budget or slot.
    S browsers;size_t before=tx.size();
    for(int i=0;i<100;++i) request(browsers,1,S::Page);
    assert(browsers.dispatch(0,send,nullptr));
    for(unsigned t=1;t<500;++t) assert(!browsers.dispatch(t,send,nullptr));
    assert(tx.size()==before+1);
    assert(!browsers.dispatch(500,send,nullptr));
    assert(!browsers.dispatch(999,send,nullptr));
    assert(browsers.dispatch(1000,send,nullptr));
    // Send failure does not occupy a slot, retry immediately or starve peers.
    S errors;request(errors,1,S::Page);request(errors,2,S::Page);
    fail=true;assert(!errors.dispatch(0,send,nullptr));fail=false;
    assert(errors.inflight()==0 && errors.statistics().sendErrors==1);
    assert(errors.dispatch(100,send,nullptr) && tx.back().id==2);
    // Millisecond wrap and demand expiry.
    S wrap;request(wrap,1,S::Await,UINT32_MAX-50);
    assert(wrap.dispatch(UINT32_MAX-50,send,nullptr));
    wrap.receive(1,0x36,20);assert(wrap.statistics().latencyMaxMs==71);
    assert(!wrap.dispatch(30,send,nullptr));assert(wrap.dispatch(50,send,nullptr));
    S expired;assert(expired.demand(1,0x36,S::Page,100,200,1,0));
    assert(!expired.dispatch(200,send,nullptr));
    puts("PASS query scheduler: dedup, global budget, in-flight, timeout/cooldown, fairness, raw traffic, exclusivity, browsers, wrap");
}
