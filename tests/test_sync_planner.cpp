#include "SyncPlanner.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace motion;
static QueueStep axis(uint8_t id,int32_t distance) {
    QueueStep s;s.action=QueueAction::Move;s.id=id;s.distanceTenths=distance;
    s.speedTenths=300;s.accelRpmS=60;s.decelRpmS=60;s.currentMa=800;return s;
}
int main() {
    {
        QueueStep fast[]={axis(1,-10800),axis(2,54000)};
        fast[0].speedTenths=1000;fast[1].speedTenths=5000;
        fast[0].accelRpmS=fast[0].decelRpmS=300;
        fast[1].accelRpmS=fast[1].decelRpmS=1500;
        SyncTolerance tolerance;tolerance.progress=.02;tolerance.timeMs=1000;
        SyncPlan plan;
        assert(!planSync(fast,2,tolerance,plan));
        assert(plan.axes[0].speedTenths==1000 && plan.axes[1].speedTenths==5000);
        assert(plan.axes[0].accelRpmS==300 && plan.axes[1].accelRpmS==1500);
        assert(plan.maxProgressError<1e-12 && plan.maxTimeErrorMs<1e-9);
        assert(fabs(plan.common.total-2.1333333333333333)<1e-9);
    }
    {
        QueueStep eight[8];for(uint8_t i=0;i<8;++i) {
            eight[i]=axis(i+1,(i%2?-1:1)*3600*(i+1));
            eight[i].speedTenths=320;eight[i].accelRpmS=eight[i].decelRpmS=64;
        }
        SyncTolerance tolerance;tolerance.progress=.02;tolerance.timeMs=100;
        SyncPlan plan;assert(!planSync(eight,8,tolerance,plan));assert(plan.count==8);
        assert(planSync(eight,9,tolerance,plan));
    }
    QueueStep axes[]={axis(1,3600),axis(2,-1800)};
    SyncTolerance tolerance;tolerance.progress=0.01;tolerance.timeMs=50;
    SyncPlan plan;
    assert(!planSync(axes,2,tolerance,plan));
    assert(plan.count==2 && plan.axes[1].speedTenths==150 && plan.axes[1].accelRpmS==30);
    assert(plan.axes[1].distanceTenths==-1800);
    assert(plan.maxProgressError<1e-12 && plan.maxTimeErrorMs<1e-9);
    assert(plan.common.cruise>0);
    axes[0].distanceTenths=36;axes[1].distanceTenths=-18;
    assert(!planSync(axes,2,tolerance,plan));assert(plan.common.cruise==0);
    // Actual analytic maximum agrees with a dense reference sampling test.
    ProgressProfile a,b;assert(a.build(0.35,0.8,0.7));assert(b.build(0.34,0.7,0.6));
    const double exact=maxProgressDifference(a,b);double sampled=0;
    for(int i=0;i<=10000;++i) {const double t=fmax(a.total,b.total)*i/10000;
        sampled=fmax(sampled,fabs(a.at(t).position-b.at(t).position));}
    assert(exact>=sampled-1e-12 && exact-sampled<1e-6);
    axes[1].id=1;assert(!strcmp(planSync(axes,2,tolerance,plan),"sync_duplicate_id"));axes[1].id=2;
    axes[1].accelRpmS=0;assert(planSync(axes,2,tolerance,plan));axes[1].accelRpmS=60;
    axes[0].distanceTenths=1000000;axes[1].distanceTenths=1;
    assert(!strcmp(planSync(axes,2,tolerance,plan),"sync_quantization_unrepresentable"));
    axes[0]=axis(1,3600);axes[1]=axis(2,1234);
    tolerance.progress=1e-8;tolerance.timeMs=1;
    assert(!strcmp(planSync(axes,2,tolerance,plan),"sync_quantization_tolerance"));
    HelixGeometry geometry;geometry.leadMmPerCapRev=2;geometry.motorRevPerCapRev=3;
    geometry.linearMmPerMotorRev=8;geometry.rotaryDirection=-1;geometry.linearDirection=1;
    assert(!resolveHelix(2,geometry,axes[0],axes[1]));
    assert(axes[0].distanceTenths==-21600 && axes[1].distanceTenths==1800);
    assert(!resolveHelix(-2,geometry,axes[0],axes[1]));
    assert(axes[0].distanceTenths==21600 && axes[1].distanceTenths==-1800);
    geometry.leadMmPerCapRev=0;assert(resolveHelix(2,geometry,axes[0],axes[1]));
    puts("PASS sync planner: opposite directions, ratios, triangle/trapezoid, analytic quantization envelope, tiny paths, helix geometry");
}
