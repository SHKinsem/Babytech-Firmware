#pragma once
#include "SyncPlanner.h"
#include "CanQueryScheduler.h"

namespace motion {
struct SyncSettings {
    SyncTolerance tolerance;
    uint32_t feedbackTimeoutMs=0, prepareTimeoutMs=0, stopTimeoutMs=0, responseBudgetMs=0;
    uint16_t completionTenths=0;
    bool valid() const {
        return isfinite(tolerance.progress) && tolerance.progress>0 && tolerance.progress<=0.25 &&
            tolerance.timeMs>0 && tolerance.timeMs<=60000 && completionTenths>0 && completionTenths<=1000 &&
            responseBudgetMs>=1 && responseBudgetMs<=5000 && feedbackTimeoutMs>=200 && feedbackTimeoutMs<=30000 &&
            prepareTimeoutMs>=1000 && prepareTimeoutMs<=60000 && stopTimeoutMs>=500 && stopTimeoutMs<=30000;
    }
};
struct SyncFeedback {
    int32_t position=0,velocity=0,target=0;
    uint32_t positionAt=0,velocityAt=0,flagsAt=0,homeAt=0,targetAt=0,ackSequence=0,ackAt=0;
    uint32_t positionRequestedAt=0,velocityRequestedAt=0,flagsRequestedAt=0,homeRequestedAt=0,targetRequestedAt=0;
    uint8_t flags=0,homeFlags=0,ack=0;
    bool positionValid=false,velocityValid=false,flagsValid=false,homeValid=false,targetValid=false;
};
class SyncPort {
public:
    virtual ~SyncPort() {}
    virtual SyncFeedback syncFeedback(uint8_t id) const=0;
    virtual bool syncSendMove(const QueueStep& step)=0;
    virtual bool syncTrigger()=0;
    virtual bool syncStop(uint8_t id)=0;
    virtual void syncObserve(uint8_t id,bool value)=0;
    virtual bool syncIsolationReady() const=0;
    virtual void syncInvalidateIsolation()=0;
    virtual void syncCompletedIsolation()=0;
};
class SyncRuntime {
public:
    enum class Phase : uint8_t {Idle,Checking,Caching,Confirming,Triggering,Monitoring,Complete,Stopping,Failed};
    struct Member {
        int32_t start=0,previousTarget=0,lastTargetReadback=0;
        int64_t target=0;
        uint32_t sentAt=0,ackBaseline=0,positionCountedAt=0,velocityCountedAt=0;
        uint8_t doneSamples=0,stopSamples=0;
        bool sent=false,accepted=false,targetObserved=false,targetDeferred=false,targetReadbackValid=false;
        bool targetConfirmed=false,done=false,stopSent=false,stopped=false;
    };
    explicit SyncRuntime(SyncPort& port,CanQueryScheduler& scheduler):port_(port),scheduler_(scheduler) {}
    bool active() const {return phase_!=Phase::Idle && phase_!=Phase::Complete && phase_!=Phase::Failed;}
    Phase phase() const {return phase_;}
    const char* error() const {return error_;}
    const SyncPlan& plan() const {return plan_;}
    const Member& member(uint8_t index) const {return members_[index];}
    double errorLower() const {return errorLower_;}
    double errorUpper() const {return errorUpper_;}
    double maxObservableError() const {return maxObservableError_;}
    uint32_t sampleBudgetMs() const {return sampleBudgetMs_;}
    bool triggerOnly() const {return triggerOnly_;}
    static const char* phaseName(Phase p) {
        switch(p) {
            case Phase::Idle:return "idle";case Phase::Checking:return "checking";
            case Phase::Caching:return "caching";case Phase::Confirming:return "confirming";
            case Phase::Triggering:return "triggering";case Phase::Monitoring:return "monitoring";
            case Phase::Complete:return "complete";case Phase::Stopping:return "stop_requested";
            case Phase::Failed:return "failed";
        }return "failed";
    }
    void reset() {
        if(active()) return;
        phase_=Phase::Idle;plan_.count=0;error_=nullptr;triggerOnly_=false;
    }
    const char* validate(const QueueStep* axes,uint8_t count,const SyncSettings& settings,
                         SyncPlan* output=nullptr,bool triggerOnly=false) {
        static SyncPlan scratch;
        SyncPlan& checked=output ? *output : scratch;
        if(!settings.valid()) return "sync_settings_unconfigured";
        const char* error=planSync(axes,count,settings.tolerance,checked);
        if(error) return error;
        const auto& budget=scheduler_.config();
        const uint32_t gap=uint32_t(fmax(budget.gapMs,ceil(1000.0/budget.queriesPerSecond)));
        // Position, velocity, flags, home and (until verified) target share one
        // service. Some X42S drives expose a cached target only after FF.
        sampleBudgetMs_=gap*count*5+settings.responseBudgetMs;
        double quantization=0;
        for(uint8_t i=0;i<count;++i) quantization=fmax(quantization,(1.0+settings.completionTenths)/fabs(double(axes[i].distanceTenths)));
        const double uncertainty=2*checked.common.speed*sampleBudgetMs_/1000.0+2*quantization;
        if(sampleBudgetMs_>=settings.feedbackTimeoutMs ||
           settings.responseBudgetMs>=budget.timeoutMs ||
           (!triggerOnly && uncertainty+checked.maxProgressError>settings.tolerance.progress))
            return "sync_feedback_budget_insufficient";
        return nullptr;
    }
    const char* start(const QueueStep* axes,uint8_t count,const SyncSettings& settings,
                      uint32_t now,bool triggerOnly=false) {
        if(active()) return "sync_busy";
        const char* error=validate(axes,count,settings,&plan_,triggerOnly);
        if(error) return error;
        if(!port_.syncIsolationReady()) return "sync_cache_isolation_unverified";
        settings_=settings;triggerOnly_=triggerOnly;phase_=Phase::Checking;phaseAt_=now;startedAt_=0;cacheIndex_=0;
        error_=nullptr;errorLower_=errorUpper_=maxObservableError_=0;
        uncertain_=false;triggerAttempted_=false;
        scheduler_.release(CanQueryScheduler::Sync);scheduler_.exclusiveSync(true);
        for(uint8_t i=0;i<count;++i) {members_[i]=Member{};port_.syncObserve(axes[i].id,true);}
        return nullptr;
    }
    // One authorized or fault-generated stop request per member, never disable.
    // Even members whose cache was not sent are included; no stop is retried.
    void abort(const char* error,uint32_t now) {
        if(phase_==Phase::Stopping || phase_==Phase::Failed) return;
        error_=error;phase_=Phase::Stopping;phaseAt_=now;
        port_.syncInvalidateIsolation();scheduler_.release(CanQueryScheduler::Sync);
        for(uint8_t i=0;i<plan_.count;++i) {
            auto& m=members_[i];m.stopSent=port_.syncStop(plan_.axes[i].id);
            m.stopSamples=0;m.stopped=false;m.positionCountedAt=m.velocityCountedAt=now;
        }
    }
    void poll(uint32_t now) {
        if(!active()) return;
        if(phase_==Phase::Stopping) {pollStopping(now);return;}
        if(phase_!=Phase::Monitoring && now-phaseAt_>settings_.prepareTimeoutMs) {
            abort("sync_prepare_timeout",now);return;
        }
        if(phase_==Phase::Checking) {
            bool ready=true;
            for(uint8_t i=0;i<plan_.count;++i) {
                const uint8_t id=plan_.axes[i].id;const auto f=port_.syncFeedback(id);
                demandStationary(id,now);
                demand(id,0x33,500,now);
                if(f.flagsValid && f.homeValid && fault(f)) {abort("sync_member_fault",now);return;}
                if(!stationary(f,now,phaseAt_)) {ready=false;continue;}
                if(!f.targetValid || !newer(f.targetAt,phaseAt_) || int32_t(f.targetRequestedAt-phaseAt_)<0) {ready=false;continue;}
                if(!(f.flags&1)) {abort("sync_member_disabled",now);return;}
                if(fault(f)) {abort("sync_member_fault",now);return;}
                members_[i].start=f.position;
                members_[i].target=int64_t(f.position)+plan_.axes[i].distanceTenths;
                members_[i].previousTarget=f.target;
                // No transaction IDs exist in the drive ACK. Distinct old and
                // new target windows make a later readback unambiguous.
                // The drive may use a slightly different internal position
                // when applying a relative move. Keep old and new targets
                // separated by more than both completion windows.
                if(targetNear(members_[i].target,f.target,2*settings_.completionTenths)) {
                    abort("sync_target_association_uncertain",now);return;
                }
                if(members_[i].target>INT32_MAX || members_[i].target<INT32_MIN) {
                    abort("sync_target_out_of_range",now);return;
                }
            }
            if(!ready) return;
            scheduler_.release(CanQueryScheduler::Sync);
            port_.syncInvalidateIsolation();phase_=Phase::Caching;
            lastCacheAt_=now-2;
        }
        if(phase_==Phase::Caching || phase_==Phase::Confirming) {
            for(uint8_t i=0;i<cacheIndex_;++i) {
                const auto f=port_.syncFeedback(plan_.axes[i].id);auto& m=members_[i];
                if(f.ackSequence==m.ackBaseline) continue;
                if(f.ack==0xE2 || f.ack==0xEE) {abort("sync_cache_rejected",now);return;}
                // 9F describes a completed earlier motion, not cache acceptance.
                if(f.ack==2 && int32_t(f.ackAt-m.sentAt)>=0) m.accepted=true;
            }
            if(phase_==Phase::Caching) {
                if(now-lastCacheAt_<2) return;
                auto& m=members_[cacheIndex_];const auto& axis=plan_.axes[cacheIndex_];
                m.ackBaseline=port_.syncFeedback(axis.id).ackSequence;m.sentAt=now;
                m.sent=port_.syncSendMove(axis);lastCacheAt_=now;
                ++cacheIndex_;
                if(!m.sent) {abort("sync_cache_tx_failed",now);return;}
                if(cacheIndex_==plan_.count) phase_=Phase::Confirming;
                return;
            }
            bool ready=true;
            for(uint8_t i=0;i<plan_.count;++i) {
                auto& m=members_[i];const uint8_t id=plan_.axes[i].id;const auto f=port_.syncFeedback(id);
                if(!m.accepted) {ready=false;continue;}
                // A fresh readback may be the new target or the old target:
                // this drive applies its cached target only when FF arrives.
                // Any third value is an association error before the trigger.
                if(!m.targetObserved && f.targetValid && newer(f.targetAt,m.sentAt) && int32_t(f.targetRequestedAt-m.sentAt)>=0) {
                    m.lastTargetReadback=f.target;m.targetReadbackValid=true;
                    if(targetNear(f.target,m.target,settings_.completionTenths)) m.targetConfirmed=true;
                    else if(targetNear(f.target,m.previousTarget,settings_.completionTenths)) m.targetDeferred=true;
                    else {abort("sync_target_mismatch",now);return;}
                    m.targetObserved=true;
                }
                if(!m.targetObserved) demand(id,0x33,500,now,3);
                else scheduler_.release(id,0x33,CanQueryScheduler::Sync);
                demandStationary(id,now);
                if(f.flagsValid && f.homeValid && fault(f)) {abort("sync_prepare_member_fault",now);return;}
                if(!m.targetObserved || !stationary(f,now,m.sentAt)) {ready=false;continue;}
                if(!(f.flags&1) || fault(f)) {abort("sync_prepare_member_fault",now);return;}
                if(fabs(double(int64_t(f.position)-m.start))>settings_.completionTenths) {
                    abort("sync_start_position_changed",now);return;
                }
            }
            if(!ready) return;
            phase_=Phase::Triggering;scheduler_.release(CanQueryScheduler::Sync);
        }
        if(phase_==Phase::Triggering) {
            if(triggerAttempted_) {abort("sync_trigger_duplicate",now);return;}
            triggerAttempted_=true;
            if(!port_.syncTrigger()) {abort("sync_trigger_tx_failed",now);return;}
            startedAt_=now;phase_=Phase::Monitoring;
            for(uint8_t i=0;i<plan_.count;++i) {
                members_[i].positionCountedAt=members_[i].velocityCountedAt=now;
            }
            return;
        }
        if(phase_==Phase::Monitoring) pollMotion(now);
    }
private:
    static bool newer(uint32_t a,uint32_t b) {return int32_t(a-b)>0;}
    static bool targetNear(int64_t observed,int64_t expected,int64_t tolerance) {
        return observed>=expected-tolerance && observed<=expected+tolerance;
    }
    bool fresh(uint32_t at,uint32_t now) const {return now-at<settings_.feedbackTimeoutMs;}
    static bool fault(const SyncFeedback& f) {return (f.flags&0x0C) || (f.homeFlags&0x3C);}
    bool stationary(const SyncFeedback& f,uint32_t now,uint32_t since) const {
        return f.positionValid && f.velocityValid && f.flagsValid && f.homeValid &&
            newer(f.positionAt,since) && newer(f.velocityAt,since) && newer(f.flagsAt,since) && newer(f.homeAt,since) &&
            int32_t(f.positionRequestedAt-since)>=0 && int32_t(f.velocityRequestedAt-since)>=0 &&
            int32_t(f.flagsRequestedAt-since)>=0 && int32_t(f.homeRequestedAt-since)>=0 &&
            fresh(f.positionAt,now) && fresh(f.velocityAt,now) && fresh(f.flagsAt,now) && fresh(f.homeAt,now) &&
            f.velocity>=-5 && f.velocity<=5 && !(f.homeFlags&4);
    }
    void demand(uint8_t id,uint8_t field,uint32_t period,uint32_t now,uint8_t priority=2) {
        scheduler_.demand(id,field,CanQueryScheduler::Sync,period,1000,priority,now);
    }
    void demandStationary(uint8_t id,uint32_t now) {
        demand(id,0x36,200,now);demand(id,0x35,200,now);
        demand(id,0x3A,500,now);demand(id,0x3B,500,now);
    }
    void release() {
        scheduler_.release(CanQueryScheduler::Sync);scheduler_.exclusiveSync(false);
        for(uint8_t i=0;i<plan_.count;++i) port_.syncObserve(plan_.axes[i].id,false);
    }
    void pollStopping(uint32_t now) {
        bool all=true;
        for(uint8_t i=0;i<plan_.count;++i) {
            auto& m=members_[i];if(m.stopped) continue;
            const uint8_t id=plan_.axes[i].id;const auto f=port_.syncFeedback(id);
            demandStationary(id,now);
            if(stationary(f,now,phaseAt_) && newer(f.positionAt,m.positionCountedAt) && newer(f.velocityAt,m.velocityCountedAt)) {
                m.positionCountedAt=f.positionAt;m.velocityCountedAt=f.velocityAt;
                if(++m.stopSamples>=2) {m.stopped=true;continue;}
            } else if(f.velocityValid && (f.velocity>5 || f.velocity<-5)) m.stopSamples=0;
            all=false;
        }
        if(all || now-phaseAt_>=settings_.stopTimeoutMs) {phase_=Phase::Failed;release();}
    }
    void pollMotion(uint32_t now) {
        bool all=true,comparable=true;
        double progress[8]={},uncertainty[8]={};
        const bool near=double(now-startedAt_)+sampleBudgetMs_>=plan_.common.total*1000;
        for(uint8_t i=0;i<plan_.count;++i) {
            auto& m=members_[i];const uint8_t id=plan_.axes[i].id;const auto f=port_.syncFeedback(id);
            demand(id,0x3A,1000,now,1);demand(id,0x3B,1000,now,1);
            if(f.ackSequence!=m.ackBaseline && (f.ack==0xE2 || f.ack==0xEE)) {
                abort("sync_member_rejected",now);return;
            }
            if(f.flagsValid && f.homeValid && (!(f.flags&1) || fault(f))) {abort("sync_member_fault",now);return;}
            if(!m.targetConfirmed) {
                demand(id,0x33,200,now,3);
                if(f.targetValid && newer(f.targetAt,startedAt_) &&
                   int32_t(f.targetRequestedAt-startedAt_)>=0) {
                    m.lastTargetReadback=f.target;m.targetReadbackValid=true;
                    if(targetNear(f.target,m.target,settings_.completionTenths)) {
                        m.targetConfirmed=true;
                        scheduler_.release(id,0x33,CanQueryScheduler::Sync);
                    } else if(!targetNear(f.target,m.previousTarget,settings_.completionTenths)) {
                        abort("sync_target_mismatch",now);return;
                    }
                }
                if(!m.targetConfirmed && now-startedAt_>=settings_.feedbackTimeoutMs) {
                    abort("sync_target_not_applied",now);return;
                }
            }
            // Keep observing early finishers until every axis has completed.
            // Otherwise a member can drift while the remaining axes run.
            demand(id,0x36,200,now,3);
            if(near || m.done) demand(id,0x35,200,now,3);
            const bool positionFresh=f.positionValid && newer(f.positionAt,startedAt_) && fresh(f.positionAt,now);
            const bool velocityFresh=f.velocityValid && newer(f.velocityAt,startedAt_) && fresh(f.velocityAt,now);
            const bool completionFresh=positionFresh && velocityFresh &&
                now-f.positionAt<=sampleBudgetMs_ && now-f.velocityAt<=sampleBudgetMs_;
            if(m.done && !completionFresh) {m.done=false;m.doneSamples=0;}
            if(completionFresh && newer(f.positionAt,m.positionCountedAt) &&
               newer(f.velocityAt,m.velocityCountedAt) && int32_t(f.positionRequestedAt-startedAt_)>=0 &&
               int32_t(f.velocityRequestedAt-startedAt_)>=0) {
                m.positionCountedAt=f.positionAt;m.velocityCountedAt=f.velocityAt;
                const bool atTarget=fabs(double(int64_t(f.position)-m.target))<=settings_.completionTenths &&
                    f.velocity>=-5 && f.velocity<=5;
                if(m.done && !atTarget) {abort("sync_member_left_target",now);return;}
                if(atTarget) {
                    if(m.doneSamples<2) ++m.doneSamples;
                    m.done=m.doneSamples>=2;
                } else {m.done=false;m.doneSamples=0;}
            }
            const bool valid=positionFresh && (!m.done || velocityFresh) && f.flagsValid && f.homeValid &&
                fresh(f.flagsAt,now) && fresh(f.homeAt,now);
            if(!m.done || !completionFresh || !valid || !m.targetConfirmed) all=false;
            if(!valid && now-startedAt_>=settings_.feedbackTimeoutMs) {abort("sync_feedback_lost",now);return;}
            if(!valid) comparable=false;
            const double length=double(plan_.axes[i].distanceTenths);
            progress[i]=double(int64_t(f.position)-m.start)/length;
            const double endpointMargin=(settings_.completionTenths+1)/fabs(length);
            if(!m.done && valid && (progress[i]<-endpointMargin || progress[i]>1+endpointMargin)) {
                abort("sync_travel_envelope_exceeded",now);return;
            }
            // Compare possible progress at a common time, with reception delay
            // and sample age covered by the commanded speed bound. This does
            // not assert continuous actual accuracy between samples.
            uncertainty[i]=plan_.encoded[i].speed*(now-f.positionAt+settings_.responseBudgetMs)/1000.0+
                1/fabs(length);
        }
        if(all) {phase_=Phase::Complete;port_.syncCompletedIsolation();release();return;}
        if(comparable) {
            errorLower_=errorUpper_=0;
            for(uint8_t i=0;i<plan_.count;++i) for(uint8_t j=0;j<i;++j) {
                const double difference=fabs(progress[i]-progress[j]),radius=uncertainty[i]+uncertainty[j];
                errorLower_=fmax(errorLower_,fmax(0.0,difference-radius));
                errorUpper_=fmax(errorUpper_,difference+radius);
            }
            maxObservableError_=fmax(maxObservableError_,errorLower_);
            if(errorLower_>settings_.tolerance.progress) {abort("sync_coordination_error",now);return;}
            if(!triggerOnly_ && errorUpper_>settings_.tolerance.progress) {
                if(!uncertain_) {uncertain_=true;uncertainAt_=now;}
                else if(now-uncertainAt_>=settings_.feedbackTimeoutMs) {abort("sync_feedback_indeterminate",now);return;}
            } else uncertain_=false;
        }
        if(double(now-startedAt_)>plan_.common.total*1000+settings_.feedbackTimeoutMs*2+settings_.tolerance.timeMs)
            abort("sync_motion_timeout",now);
    }
    SyncPort& port_;CanQueryScheduler& scheduler_;
    SyncPlan plan_;
    SyncSettings settings_;
    Member members_[8];
    Phase phase_=Phase::Idle;
    const char* error_=nullptr;
    uint32_t phaseAt_=0,startedAt_=0,lastCacheAt_=0,sampleBudgetMs_=0,uncertainAt_=0;
    uint8_t cacheIndex_=0;
    bool triggerAttempted_=false,uncertain_=false,triggerOnly_=false;
    double errorLower_=0,errorUpper_=0,maxObservableError_=0;
};
} // namespace motion
