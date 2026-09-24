#pragma once
#include <math.h>
#include <stdint.h>
#include "QueueProgram.h"

namespace motion {

// Positions are normalized progress, not simultaneous motor samples. The
// analytic profile is used for planning/quantization checks only; it is never
// streamed as setpoints. Encoded driver parameters are returned in axes[].
struct ProgressProfile {
    double speed=0,accel=0,decel=0,up=0,cruise=0,down=0,total=0;
    bool build(double v,double a,double d) {
        if(!isfinite(v) || !isfinite(a) || !isfinite(d) || v<=0 || a<=0 || d<=0) return false;
        accel=a;decel=d;
        speed=fmin(v,sqrt(2*a*d/(a+d)));
        up=speed/a;down=speed/d;
        cruise=fmax(0.0,(1-0.5*speed*(up+down))/speed);
        total=up+cruise+down;
        return isfinite(total) && total>0 && total<=3600;
    }
    struct State {double position,velocity,acceleration;};
    State at(double t) const {
        if(t<0) return {0,0,0};
        if(t<up) return {0.5*accel*t*t,accel*t,accel};
        if(t<up+cruise) return {0.5*speed*up+speed*(t-up),speed,0};
        if(t<total) {
            const double remaining=total-t;
            return {1-0.5*decel*remaining*remaining,decel*remaining,-decel};
        }
        return {1,0,0};
    }
};

// Exact maximum progress difference between piecewise quadratic profiles:
// segment boundaries plus each derivative root, not a fixed-rate sample grid
// which could miss a short-path quantization error.
inline double maxProgressDifference(const ProgressProfile& a,const ProgressProfile& b) {
    double cuts[]={0,a.up,a.up+a.cruise,a.total,b.up,b.up+b.cruise,b.total};
    for(unsigned i=1;i<7;++i) {
        const double key=cuts[i];unsigned j=i;
        while(j && cuts[j-1]>key) {cuts[j]=cuts[j-1];--j;}cuts[j]=key;
    }
    double error=0;
    for(unsigned i=0;i<7;++i) {
        error=fmax(error,fabs(a.at(cuts[i]).position-b.at(cuts[i]).position));
        if(!i || cuts[i]<=cuts[i-1]) continue;
        const double mid=(cuts[i]+cuts[i-1])*0.5;
        const auto av=a.at(mid),bv=b.at(mid);
        const double acceleration=av.acceleration-bv.acceleration;
        if(fabs(acceleration)<1e-15) continue;
        const double root=mid-(av.velocity-bv.velocity)/acceleration;
        if(root>cuts[i-1] && root<cuts[i])
            error=fmax(error,fabs(a.at(root).position-b.at(root).position));
    }
    return error;
}

struct SyncPlan {
    QueueStep axes[8];
    ProgressProfile encoded[8],common;
    uint8_t count=0;
    double maxProgressError=0,maxTimeErrorMs=0;
};
struct SyncTolerance {
    // Explicit user configuration; all zeros means unconfigured, never guessed.
    double progress=0;
    uint32_t timeMs=0;
};

inline const char* planSync(const QueueStep* members,uint8_t count,
                            const SyncTolerance& tolerance,SyncPlan& out) {
    out.count=0;out.maxProgressError=out.maxTimeErrorMs=0;
    if(!members || count<2 || count>8) return "sync_member_count";
    if(!isfinite(tolerance.progress) || tolerance.progress<=0 || tolerance.progress>0.25 ||
       !tolerance.timeMs || tolerance.timeMs>60000) return "sync_tolerance_unconfigured";
    double v=1e30,a=1e30,d=1e30;
    for(uint8_t i=0;i<count;++i) {
        const auto& axis=members[i];
        if(axis.action!=QueueAction::Move || !axis.id || !axis.distanceTenths ||
           !axis.speedTenths || !axis.accelRpmS || !axis.decelRpmS) return "sync_invalid_member";
        for(uint8_t j=0;j<i;++j) if(members[j].id==axis.id) return "sync_duplicate_id";
        const double length=fabs(double(axis.distanceTenths));
        v=fmin(v,axis.speedTenths*6.0/length);
        a=fmin(a,axis.accelRpmS*60.0/length);
        d=fmin(d,axis.decelRpmS*60.0/length);
    }
    if(!out.common.build(v,a,d)) return "sync_duration_out_of_range";
    for(uint8_t i=0;i<count;++i) {
        out.axes[i]=members[i];auto& axis=out.axes[i];
        const double length=fabs(double(axis.distanceTenths));
        // Round down so encoded motion never exceeds any requested axis limit.
        // The finite nonzero field check rejects unrepresentable short paths.
        const double speed=floor(out.common.speed*length/6.0+1e-9);
        const double accel=floor(a*length/60.0+1e-9);
        const double decel=floor(d*length/60.0+1e-9);
        if(speed<1 || speed>members[i].speedTenths || accel<1 || accel>members[i].accelRpmS ||
           decel<1 || decel>members[i].decelRpmS) return "sync_quantization_unrepresentable";
        axis.speedTenths=uint16_t(speed);axis.accelRpmS=uint16_t(accel);axis.decelRpmS=uint16_t(decel);
        if(!out.encoded[i].build(speed*6/length,accel*60/length,decel*60/length))
            return "sync_duration_out_of_range";
        out.maxTimeErrorMs=fmax(out.maxTimeErrorMs,1000*fabs(out.encoded[i].total-out.common.total));
        out.maxProgressError=fmax(out.maxProgressError,maxProgressDifference(out.common,out.encoded[i]));
        for(uint8_t j=0;j<i;++j) {
            out.maxProgressError=fmax(out.maxProgressError,maxProgressDifference(out.encoded[j],out.encoded[i]));
            out.maxTimeErrorMs=fmax(out.maxTimeErrorMs,1000*fabs(out.encoded[j].total-out.encoded[i].total));
        }
    }
    if(out.maxProgressError>tolerance.progress || out.maxTimeErrorMs>tolerance.timeMs)
        return "sync_quantization_tolerance";
    out.count=count;return nullptr;
}

struct HelixGeometry {
    double leadMmPerCapRev=0, motorRevPerCapRev=0, linearMmPerMotorRev=0;
    int8_t rotaryDirection=0,linearDirection=0;
};
// Caller supplies explicit axis motion limits and IDs; geometry only resolves
// displacement. Signed cap turns reverse both configured motor directions.
inline const char* resolveHelix(double capTurns,const HelixGeometry& g,QueueStep& rotary,QueueStep& linear) {
    if(!isfinite(capTurns) || capTurns==0 || !isfinite(g.leadMmPerCapRev) || g.leadMmPerCapRev<=0 ||
       !isfinite(g.motorRevPerCapRev) || g.motorRevPerCapRev<=0 ||
       !isfinite(g.linearMmPerMotorRev) || g.linearMmPerMotorRev<=0 ||
       (g.rotaryDirection!=1 && g.rotaryDirection!=-1) ||
       (g.linearDirection!=1 && g.linearDirection!=-1)) return "helix_geometry_unconfigured";
    if(!rotary.id || !linear.id || rotary.id==linear.id) return "sync_duplicate_id";
    const double positions[]={capTurns*g.motorRevPerCapRev*g.rotaryDirection*3600,
        g.leadMmPerCapRev*capTurns/g.linearMmPerMotorRev*g.linearDirection*3600};
    for(double position:positions)
        if(!isfinite(position) || fabs(position)>kQueueMaxMoveTenths || fabs(position)<0.5)
            return "helix_distance_unrepresentable";
    rotary.action=linear.action=QueueAction::Move;
    rotary.distanceTenths=int32_t(round(positions[0]));linear.distanceTenths=int32_t(round(positions[1]));
    return nullptr;
}
} // namespace motion
