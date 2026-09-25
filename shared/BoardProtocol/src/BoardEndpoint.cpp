#include "BoardEndpoint.h"
#include <cstring>

namespace babytech { namespace v2 {
namespace {
bool keyMatches(const Frame& a,const Frame& b) { return a.session==b.session && a.sequence==b.sequence; }
Frame reject(const Frame& q,Reason r) { return reply(q,Outcome::Rejected,r); }
}
Status Endpoint::status(uint32_t now) const {
    Status s; s.boot=boot_; s.uptime=now; s.revision=revision_; s.motors=backend_.motorsAvailable();
    s.state=(stop_.valid || backend_.stopping()) ? State::Stopping : backend_.fault() ? State::Fault :
        (exec_.valid || backend_.busy()) ? State::Running : State::Idle;
    s.fault=backend_.fault() ? Reason::FaultActive : Reason::None;
    const Record& active=stop_.valid ? stop_ : exec_;
    if (active.valid) { s.active.session=active.request.session; s.active.sequence=active.request.sequence; }
    if (exec_.valid && exec_.request.payload[8]==kStage) s.stage=kMoveStage;
    const Record& last=results_[(nextResult_+7)%8];
    if (last.valid) {
        s.lastValid=true; s.last.session=last.request.session; s.last.sequence=last.request.sequence;
        result(last.response,s.lastOutcome,s.lastReason);
    }
    return s;
}
bool Endpoint::cached(const Frame& q,Frame& out) const {
    const auto check=[&](const Record& r,bool pending) {
        if (!r.valid || !keyMatches(q,r.request)) return false;
        if (!sameRequest(q,r.request)) out=reject(q,Reason::RequestConflict);
        else if (pending && backend_.unverifiedMode()) {
            // The prior supervised command was already submitted to CAN. A
            // retry receives the send-only outcome now; tick still emits its
            // one terminal event for the original Brain owner.
            const bool cancelled=&r==&exec_ &&
                (stop_.valid || r.response.payload[0]==uint8_t(Outcome::Cancelled));
            const bool sent=&r!=&stop_ ||
                (pendingStopSentKnown_ ? pendingStopSent_ : backend_.stopped());
            out=reply(q,cancelled ? Outcome::Cancelled :
                          (sent ? Outcome::Done : Outcome::Failed),
                      sent ? Reason::None : Reason::NotReady);
        } else out=r.response;
        out.kind=Kind::Response; return true;
    };
    if (check(exec_,true) || check(stop_,true)) return true;
    for (const auto& r:results_) if (check(r,false)) return true;
    for (const auto& s:sessions_) if (check(s.last,false)) return true;
    return false;
}
Frame Endpoint::finish(Record& r,Outcome outcome,Reason reason) {
    r.response=reply(r.request,outcome,reason); r.response.kind=Kind::Event;
    results_[nextResult_]=r; nextResult_=(nextResult_+1)%8;
    for (auto& s:sessions_) {
        if (s.last.valid && keyMatches(s.last.request,r.request)) s.last.response=r.response;
    }
    Frame event=r.response; r.valid=false;
    if (&r==&stop_) pendingStopSentKnown_=false;
    return event;
}
bool Endpoint::cancelPending(Frame& event) {
    if (exec_.valid) { event=finish(exec_,Outcome::Cancelled,Reason::None); return true; }
    if (stop_.valid) { event=finish(stop_,Outcome::Cancelled,Reason::StopUnconfirmed); return true; }
    linkLost_=false;
    return false;
}
Frame Endpoint::read(const Frame& q,Reader& r,uint32_t now,bool discovery) {
    const uint8_t count=r.get(1);
    if (!count || r.remaining()!=size_t(count)*5) return reject(q,Reason::InvalidParam);
    Frame out=reply(q,Outcome::Ok); Writer w(out.payload,kMaxPayload);
    w.put(0,1); w.put(0,2); w.put(count,1);
    for (uint8_t i=0;i<count;++i) {
        const uint8_t cls=r.get(1); const uint16_t instance=r.get(2), field=r.get(2);
        if (discovery && (cls!=kSystem || instance!=0 || field!=kInfo || count!=1))
            return reject(q,Reason::BootMismatch);
        uint8_t value[64]{}; Writer v(value,sizeof(value));
        if (cls==kSystem && instance==0) {
            if (field==kInfo) { v.put(boot_,8); v.put(2,1); v.put(kCapabilities,4); }
            else if (field==kStatus) writeStatus(v,status(now));
            else if (field>=kFirstResult && field<kFirstResult+8) {
                const Record& record=results_[(nextResult_+7-(field-kFirstResult))%8];
                if (!record.valid) return reject(q,Reason::ResultExpired);
                v.put(record.request.session,8); v.put(record.request.sequence,4);
                v.put(uint8_t(record.request.cmd),1); v.raw(record.response.payload,3);
            } else return reject(q,Reason::Unsupported);
        } else if (cls==kStage && instance==kMoveStage && field>=1 && field<=6) {
            v.put(params_.get(field),4);
        } else if (cls==kMotor && instance>=1 && instance<=255 && field==1) {
            // While an operation owns the controller, a read must not change its
            // polling target. Otherwise reading another node could starve it.
            if (!busy() && !backend_.busy()) backend_.watch(uint8_t(instance));
            const size_t n=backend_.motorFeedback(uint8_t(instance),value,sizeof(value));
            if (!n || n>sizeof(value)) return reject(q,Reason::NotReady);
            w.put(cls,1); w.put(instance,2); w.put(field,2); w.put(n,2); w.raw(value,n);
            continue;
        } else return reject(q,Reason::Unsupported);
        w.put(cls,1); w.put(instance,2); w.put(field,2); w.put(v.size(),2); w.raw(value,v.size());
    }
    if (!w.ok() || !r.done()) return reject(q,Reason::InvalidParam);
    out.length=w.size(); return out;
}
Frame Endpoint::write(const Frame& q,Reader& r) {
    const uint32_t expected=r.get(4); const uint8_t count=r.get(1);
    if (!count || count>6 || r.remaining()!=size_t(count)*11) return reject(q,Reason::InvalidParam);
    const bool unverified=backend_.unverifiedMode();
    if (!unverified && expected!=revision_) return reject(q,Reason::ConfigMismatch);
    if (!unverified && (busy() || backend_.busy())) return reject(q,Reason::Busy);
    if (!unverified && backend_.fault()) return reject(q,Reason::FaultActive);
    if (revision_==UINT32_MAX) return reject(q,Reason::InvalidState);
    Parameters next=params_; uint8_t seen=0;
    for (uint8_t i=0;i<count;++i) {
        const uint8_t cls=r.get(1); const uint16_t instance=r.get(2),field=r.get(2),length=r.get(2);
        if (cls!=kStage || instance!=kMoveStage) return reject(q,Reason::Unsupported);
        if (field<1 || field>6 || length!=4 || (seen & (1u<<(field-1)))) return reject(q,Reason::InvalidParam);
        seen|=1u<<(field-1); next.set(field,uint32_t(r.get(4)));
    }
    if (!r.done() || !(unverified ? representableParameters(next) : validParameters(next)))
        return reject(q,Reason::InvalidParam);
    params_=next; ++revision_; backend_.watch(uint8_t(params_.motor));
    Frame out=reply(q,Outcome::Ok); Writer w(out.payload+3,kMaxPayload-3); w.put(revision_,4);
    out.length=7; return out;
}
bool Endpoint::handle(const Frame& q,uint32_t now,Frame& out) {
    if (q.kind!=Kind::Request || !q.session || !q.sequence || q.length>kMaxPayload) return false;
    if (q.length<8) { out=reject(q,Reason::InvalidParam); return true; }
    Reader r(q.payload,q.length); const uint64_t target=r.get(8);
    const bool discovery=q.cmd==Cmd::Read && target==0;
    if (target!=boot_ && !(target==0 && q.cmd==Cmd::Stop) && !discovery) {
        out=reject(q,Reason::BootMismatch); return true;
    }
    if (backend_.unverifiedMode() && stop_.valid && !pendingStopSentKnown_) {
        // Snapshot the old STOP before a fresh UART STOP can overwrite the
        // backend's latest TX result. Its terminal event belongs to this key.
        pendingStopSent_=backend_.stopped(); pendingStopSentKnown_=true;
    }
    if (cached(q,out)) return true;
    Session* session=nullptr;
    for (auto& s:sessions_) if (s.id==q.session) session=&s;
    if (!session) for (auto& s:sessions_) if (!s.id) { session=&s; s.id=q.session; break; }
    if (!session && q.cmd!=Cmd::Stop) { out=reject(q,Reason::Busy); return true; }
    if (session && q.sequence<=session->high && q.cmd!=Cmd::Stop) {
        out=reject(q,Reason::ResultExpired); return true;
    }
    // Register before invoking any backend side effect. Old STOP requests are
    // idempotent and remain permitted after their cache expires.
    if (session && q.sequence>session->high) session->high=q.sequence;
    if (q.cmd==Cmd::Read) out=read(q,r,now,discovery);
    else if (q.cmd==Cmd::Write) out=write(q,r);
    else if (q.cmd==Cmd::Exec) {
        const uint8_t cls=r.get(1); const uint16_t instance=r.get(2),op=r.get(2);
        const uint32_t expected=r.get(4);
        const bool unverified=backend_.unverifiedMode();
        const bool disable=cls==kMotor && instance>=1 && instance<=255 && op==kDisable;
        if (!r.done()) out=reject(q,Reason::InvalidParam);
        else if (!unverified && expected!=revision_) out=reject(q,Reason::ConfigMismatch);
        else if (!unverified && !disable && (busy() || backend_.busy())) out=reject(q,Reason::Busy);
        else {
            // Disabling may preempt a live UART owner. Stop its other motors
            // before transferring ownership, and retain the cancelled result.
            if (!unverified && disable && exec_.valid) {
                backend_.stop();
                finish(exec_,Outcome::Cancelled,Reason::None);
            }
            Reason reason=Reason::Unsupported;
            if (cls==kStage && instance==kMoveStage && op==kRun) reason=backend_.startMove(params_);
            else if (cls==kMotor && instance>=1 && instance<=255 && (op==kEnable || op==kDisable))
                reason=backend_.enable(uint8_t(instance),op==kEnable);
            // Done in unverified mode means the CAN send was admitted. It is
            // never evidence of a motor ACK, arrival or physical disable.
            out=reason==Reason::None ? reply(q,unverified ? Outcome::Done : Outcome::Accepted) : reject(q,reason);
            if (reason==Reason::None && !unverified) {
                exec_.valid=true; exec_.request=q; exec_.response=out; lastOwnerAt_=now; linkLost_=false;
            }
        }
    } else if (q.cmd==Cmd::Stop) {
        if (!r.done()) out=reject(q,Reason::InvalidParam);
        else if (stop_.valid && !backend_.unverifiedMode())
            out=reject(q,Reason::Busy); // supervised stop already owns the path
        else if (backend_.unverifiedMode()) {
            if (exec_.valid) exec_.response=reply(exec_.request,Outcome::Cancelled);
            backend_.stop();
            const bool sent=backend_.stopped();
            out=reply(q,sent ? Outcome::Done : Outcome::Failed,
                      sent ? Reason::None : Reason::NotReady);
        }
        else {
            pendingStopSentKnown_=false;
            stop_.valid=true; stop_.request=q; stop_.response=reply(q,Outcome::Accepted);
            stopAt_=now; backend_.stop(); out=stop_.response;
        }
    } else out=reject(q,Reason::Unsupported);
    if (session) { session->last.valid=true; session->last.request=q; session->last.response=out; }
    Outcome outcome; Reason reason;
    if (q.cmd!=Cmd::Read && uint8_t(q.cmd)<=4 && result(out,outcome,reason) && outcome!=Outcome::Accepted) {
        Record& saved=results_[nextResult_]; saved.valid=true; saved.request=q; saved.response=out;
        nextResult_=(nextResult_+1)%8;
    }
    if (exec_.valid && q.session==exec_.request.session && target==boot_ &&
        result(out,outcome,reason) && (outcome==Outcome::Ok || outcome==Outcome::Accepted)) lastOwnerAt_=now;
    return true;
}
bool Endpoint::tick(uint32_t now,Frame& event) {
    if (backend_.unverifiedMode()) {
        // Mode may be enabled while a supervised UART command is pending.
        // Complete the old key without polling motor evidence or link timeout.
        linkLost_=false;
        if (exec_.valid) {
            const bool cancelled=stop_.valid ||
                exec_.response.payload[0]==uint8_t(Outcome::Cancelled);
            event=finish(exec_,cancelled ? Outcome::Cancelled : Outcome::Done,Reason::None);
            return true;
        }
        if (stop_.valid) {
            if (!pendingStopSentKnown_) {
                pendingStopSent_=backend_.stopped(); pendingStopSentKnown_=true;
            }
            const bool sent=pendingStopSent_;
            event=finish(stop_,sent ? Outcome::Done : Outcome::Failed,
                         sent ? Reason::None : Reason::NotReady);
            return true;
        }
        return false;
    }
    if (exec_.valid) {
        if (!linkLost_ && now-lastOwnerAt_>=kLinkTimeoutMs && !stop_.valid) {
            linkLost_=true; backend_.stop();
        }
        Reason reason=Reason::None; Outcome outcome=backend_.operation(reason);
        if (linkLost_) { outcome=Outcome::Failed; reason=Reason::Timeout; }
        else if (stop_.valid) { outcome=Outcome::Cancelled; reason=Reason::None; }
        if (outcome!=Outcome::Accepted) { event=finish(exec_,outcome,reason); return true; }
    }
    if (stop_.valid) {
        if (backend_.stopped()) { event=finish(stop_,Outcome::Done,Reason::None); return true; }
        if (now-stopAt_>=3000) { event=finish(stop_,Outcome::Failed,Reason::StopUnconfirmed); return true; }
    }
    return false;
}
} }
