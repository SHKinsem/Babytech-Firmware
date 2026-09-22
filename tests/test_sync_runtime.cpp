#include "SyncRuntime.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace motion;
struct Port : SyncPort {
    SyncFeedback feedback[256];std::vector<uint8_t> cached,stopped;
    bool isolation=true,failTrigger=false;uint8_t failCache=0;unsigned triggers=0;uint32_t now=0;
    SyncFeedback syncFeedback(uint8_t id) const override {return feedback[id];}
    bool syncSendMove(const QueueStep& step) override {
        cached.push_back(step.id);auto& f=feedback[step.id];f.target=f.position+step.distanceTenths;
        ++f.ackSequence;f.ackAt=now;f.ack=2;return step.id!=failCache;
    }
    bool syncTrigger() override {++triggers;return !failTrigger;}
    bool syncStop(uint8_t id) override {stopped.push_back(id);return true;}
    void syncObserve(uint8_t,bool) override {}
    bool syncIsolationReady() const override {return isolation;}
    void syncInvalidateIsolation() override {isolation=false;}
    void syncCompletedIsolation() override {isolation=true;}
    void fresh(uint32_t time,bool atTarget=false) {
        now=time;
        for(uint8_t id:{1,2}) {
            auto& f=feedback[id];if(atTarget)f.position=f.target;
            f.positionValid=f.velocityValid=f.flagsValid=f.homeValid=f.targetValid=true;
            f.positionAt=f.velocityAt=f.flagsAt=f.homeAt=f.targetAt=time;
            f.positionRequestedAt=f.velocityRequestedAt=f.flagsRequestedAt=f.homeRequestedAt=f.targetRequestedAt=time;
            f.flags=1;f.homeFlags=0;f.velocity=0;
        }
    }
};
static QueueStep axis(uint8_t id,int32_t distance) {
    QueueStep s;s.action=QueueAction::Move;s.id=id;s.distanceTenths=distance;
    s.speedTenths=10;s.accelRpmS=s.decelRpmS=60;s.currentMa=800;return s;
}
struct Rig {
    Port port;CanQueryScheduler scheduler;SyncRuntime runtime;
    QueueStep axes[2];SyncSettings settings;
    Rig():runtime(port,scheduler) {
        axes[0]=axis(1,3600);axes[1]=axis(2,-1800);
        settings.tolerance.progress=0.2;settings.tolerance.timeMs=50;
        settings.feedbackTimeoutMs=5000;settings.prepareTimeoutMs=10000;settings.stopTimeoutMs=2000;
        settings.responseBudgetMs=20;settings.completionTenths=2;
    }
    void tick(uint32_t now) {port.now=now;runtime.poll(now);}
    void prepare() {
        assert(!runtime.start(axes,2,settings,0));
        tick(0);assert(port.cached.empty()); // no old samples
        port.fresh(1);tick(1);tick(3);port.fresh(5);tick(5);
    }
};
int main() {
    {
        Rig r;r.prepare();assert(r.port.cached.size()==2 && r.port.triggers==1);
        assert(r.runtime.phase()==SyncRuntime::Phase::Monitoring);
        r.port.fresh(10,true);r.tick(10);assert(r.runtime.phase()==SyncRuntime::Phase::Monitoring);
        r.tick(11);assert(!r.runtime.member(0).done); // repeated sample isn't a second pair
        r.port.fresh(20,true);r.tick(20);
        assert(r.runtime.phase()==SyncRuntime::Phase::Complete && r.port.stopped.empty() && r.port.isolation);
        r.tick(30);assert(r.port.triggers==1);
    }
    {
        Rig r;r.port.isolation=false;
        assert(!strcmp(r.runtime.start(r.axes,2,r.settings,0),"sync_cache_isolation_unverified"));
        assert(r.port.cached.empty() && r.port.triggers==0);
    }
    {
        Rig r;r.axes[0].speedTenths=300;r.axes[1].speedTenths=300;
        assert(!strcmp(r.runtime.start(r.axes,2,r.settings,0),"sync_feedback_budget_insufficient"));
        assert(r.port.cached.empty());
    }
    {
        Rig r;r.port.failCache=2;r.prepare();
        assert(r.port.triggers==0 && r.port.stopped==std::vector<uint8_t>({1,2}));
        assert(!r.port.isolation && r.runtime.phase()==SyncRuntime::Phase::Stopping);
        r.port.fresh(10);r.tick(10);r.port.fresh(20);r.tick(20);
        assert(r.runtime.phase()==SyncRuntime::Phase::Failed);
        assert(r.runtime.member(0).stopped && r.runtime.member(1).stopped);
        assert(r.port.stopped.size()==2);
        assert(!strcmp(r.runtime.start(r.axes,2,r.settings,30),"sync_cache_isolation_unverified"));
    }
    {
        Rig r;r.port.failTrigger=true;r.prepare();r.tick(10);r.tick(20);
        assert(r.port.triggers==1 && r.port.stopped.size()==2);
        r.tick(3000);assert(r.runtime.phase()==SyncRuntime::Phase::Failed && !r.runtime.member(0).stopped);
    }
    {
        Rig r;r.prepare();r.port.fresh(10);r.port.feedback[1].position=1800;r.tick(10);
        assert(!strcmp(r.runtime.error(),"sync_coordination_error"));assert(r.port.stopped.size()==2);
    }
    {
        Rig r;r.prepare();r.tick(6000);
        assert(!strcmp(r.runtime.error(),"sync_feedback_lost") && r.port.stopped.size()==2);
    }
    {
        Rig r;assert(!r.runtime.start(r.axes,2,r.settings,0));r.port.fresh(1);
        r.port.feedback[2].flags=0;r.tick(1);
        assert(!strcmp(r.runtime.error(),"sync_member_disabled") && r.port.cached.empty());
    }
    {
        Rig r;assert(!r.runtime.start(r.axes,2,r.settings,0));r.port.fresh(1);r.tick(1);
        r.port.feedback[1].ack=0xE2;r.tick(3);
        assert(!strcmp(r.runtime.error(),"sync_cache_rejected") && r.port.triggers==0);
    }
    {
        Rig r;assert(!r.runtime.start(r.axes,2,r.settings,0));r.port.fresh(1);r.tick(1);r.tick(3);
        r.port.fresh(5);r.port.feedback[2].target+=100;r.tick(5);
        assert(!strcmp(r.runtime.error(),"sync_target_mismatch") && r.port.triggers==0);
    }
    {
        Rig r;r.prepare();r.port.feedback[1].ack=0xE2;r.tick(10);
        assert(!strcmp(r.runtime.error(),"sync_member_rejected"));
        r.runtime.abort("second_cancel",11);assert(r.port.stopped.size()==2);
    }
    {
        Rig r;assert(!r.runtime.start(r.axes,2,r.settings,100));
        r.port.fresh(101);r.port.feedback[1].positionRequestedAt=90;r.tick(101);
        assert(r.port.cached.empty()); // reply arrived later, but query predates this phase
        r.port.fresh(102);r.tick(102);assert(r.port.cached.size()==1);
    }
    {
        Rig r;assert(!r.runtime.start(r.axes,2,r.settings,0));r.port.fresh(1);
        r.port.feedback[1].target=3600;r.tick(1);
        assert(!strcmp(r.runtime.error(),"sync_target_association_uncertain") && r.port.cached.empty());
    }
    {
        Rig r;r.prepare();r.port.fresh(1000);
        r.port.feedback[1].position=60;r.port.feedback[2].position=0;
        r.port.feedback[2].positionAt=10;r.tick(1000);
        assert(r.runtime.phase()==SyncRuntime::Phase::Monitoring && r.runtime.errorLower()<.002);
        // Samples taken a second apart are not treated as simultaneous positions.
        assert(r.runtime.errorUpper()>r.runtime.errorLower());
    }
    {
        Rig r;r.prepare();r.runtime.abort("operator_cancel",10);r.port.fresh(20);r.tick(20);
        r.tick(21);assert(!r.runtime.member(0).stopped);
        r.port.fresh(30);r.tick(30);assert(r.runtime.member(0).stopped);
    }
    printf("PASS sync runtime: preflight, budget, cache/trigger failures, two-pair completion, mismatch, fault stop and unconfirmed stop; object %zu bytes\n",sizeof(SyncRuntime));
}
