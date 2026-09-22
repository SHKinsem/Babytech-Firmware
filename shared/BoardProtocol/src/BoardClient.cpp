#include "BoardClient.h"

namespace babytech { namespace v2 {
bool Client::stamp(Frame& f) {
    if (sequence_==UINT32_MAX) return false; // caller must start a new session
    f.session=session_; f.sequence=++sequence_; f.kind=Kind::Request; return true;
}
bool Client::query(uint32_t now,Frame& out,bool force) {
    if (pendingQuery_ && now-queryAt_<500) return false;
    if (!force && query_.sequence && now-queryAt_<500) return false;
    // Re-discover after outage; a lower board reboot must not strand us using
    // its old boot ID. Discovery itself never marks the device online.
    if (!boot_ || (haveStatus_ && now-statusAt_>=kLinkTimeoutMs)) {
        out=readField(session_,0,0,kSystem,0,kInfo);
    } else if ((poll_++%2)==0) {
        out=readField(session_,0,boot_,kSystem,0,kStatus);
    } else if (controlBusy() && haveStatus_ &&
               status_.active.sequence!=control_.sequence) {
        out=readField(session_,0,boot_,kSystem,0,kFirstResult+(resultSlot_++%8));
    } else if (paramRevision_!=status_.revision) {
        out=Frame{}; Writer w(out.payload,kMaxPayload); w.put(boot_,8); w.put(6,1);
        for (uint16_t field=1;field<=6;++field) { w.put(kStage,1); w.put(kMoveStage,2); w.put(field,2); }
        out.length=w.size();
    } else out=readField(session_,0,boot_,kMotor,uint16_t(params_.motor),1);
    if (!stamp(out)) return false;
    query_=out; pendingQuery_=true; queryAt_=now; return true;
}
bool Client::submit(Frame& q,uint32_t now) {
    if (q.cmd==Cmd::Read || (q.cmd!=Cmd::Stop && (!connected(now) || controlBusy()))) return false;
    if (q.cmd==Cmd::Stop && controlBusy() && control_.cmd==Cmd::Stop) { q=control_; return true; }
    if (!stamp(q)) return false;
    control_=q; controlAt_=now; haveControl_=true; controlReplied_=false;
    controlKnownTerminal_=false; controlOutcome_=Outcome::Accepted; controlReason_=Reason::None;
    resultSlot_=0; return true;
}
void Client::applyOutcome(Outcome o,Reason reason) {
    // A delayed ACCEPTED must never overwrite a terminal EVENT/result snapshot.
    if (controlKnownTerminal_) return;
    controlOutcome_=o; controlReason_=reason; controlReplied_=true;
    controlKnownTerminal_=o!=Outcome::Accepted;
}
void Client::receive(const Frame& f,uint32_t now) {
    Outcome outcome; Reason reason;
    if (f.session!=session_ || !result(f,outcome,reason)) return;
    if (haveControl_ && f.sequence==control_.sequence && f.cmd==control_.cmd) {
        applyOutcome(outcome,reason); return;
    }
    if (!pendingQuery_ || f.kind!=Kind::Response || f.sequence!=query_.sequence || f.cmd!=Cmd::Read) return;
    pendingQuery_=false;
    if (outcome!=Outcome::Ok) {
        if (reason==Reason::BootMismatch) { boot_=0; haveStatus_=false; paramRevision_=0; }
        return;
    }
    Reader q(query_.payload,query_.length); q.get(8); const uint8_t count=q.get(1);
    Reader r(f.payload+3,f.length-3);
    if (r.get(1)!=count) return;
    Status next=status_; Parameters params=params_; bool gotStatus=false,gotParams=false;
    uint64_t discovered=boot_; uint8_t flags=motorFlags_; bool gotInfo=false;
    bool recovered=false; Outcome recoveredOutcome=Outcome::Accepted; Reason recoveredReason=Reason::None;
    for (uint8_t i=0;i<count;++i) {
        const uint8_t cls=q.get(1); const uint16_t instance=q.get(2),field=q.get(2);
        if (r.get(1)!=cls || r.get(2)!=instance || r.get(2)!=field) return;
        const uint16_t length=r.get(2);
        if (length>64 || r.remaining()<length) return;
        uint8_t value[64]; for (uint16_t n=0;n<length;++n) value[n]=r.get(1);
        Reader v(value,length);
        if (cls==kSystem && field==kInfo && length==13) {
            discovered=v.get(8); if (!discovered || v.get(1)!=2 || (v.get(4)&kCapabilities)!=kCapabilities) return;
            gotInfo=true;
        } else if (cls==kSystem && field==kStatus) {
            if (!readStatus(v,next) || next.boot!=boot_) return;
            gotStatus=true;
        } else if (cls==kStage && length==4 && params.set(field,uint32_t(v.get(4)))) gotParams=true;
        else if (cls==kMotor && length==23) {
            flags=value[0]; Reader feedback(value+11,12);
            uint32_t age=uint32_t(feedback.get(4)); const uint32_t velocityAge=feedback.get(4);
            if (velocityAge>age) age=velocityAge;
            const uint32_t currentAge=feedback.get(4);
            if ((flags&4) && currentAge>age) age=currentAge;
            motorAt_=now; motorFreshFor_=age<600 ? 600-age : 0;
        }
        else if (cls==kSystem && field>=kFirstResult && field<kFirstResult+8 && length==16) {
            const uint64_t session=v.get(8); const uint32_t seq=v.get(4); const Cmd cmd=Cmd(v.get(1));
            const Outcome o=Outcome(v.get(1)); const Reason rs=Reason(v.get(2));
            if (session==session_ && seq==control_.sequence && cmd==control_.cmd && uint8_t(o)<=5 && uint16_t(rs)<=14) {
                recovered=true; recoveredOutcome=o; recoveredReason=rs;
            }
        } else return;
    }
    if (!q.done() || !r.done()) return;
    ++responses_;
    if (gotInfo) { haveStatus_=false; poll_=0; }
    if (discovered!=boot_) {
        const bool restarted=boot_!=0 || haveControl_;
        boot_=discovered; haveStatus_=false; paramRevision_=0; poll_=0; motorFlags_=0;
        // Old command result is unknown after reset, never successful/failed by
        // inference. Release it so a fresh user action can be made after discovery.
        if (restarted && haveControl_ && !controlKnownTerminal_) {
            controlKnownTerminal_=true; controlReplied_=false; controlReason_=Reason::BootMismatch;
        }
    }
    if (gotParams && validParameters(params)) { params_=params; paramRevision_=status_.revision; }
    motorFlags_=flags;
    if (gotStatus) {
        status_=next; haveStatus_=true; statusAt_=now;
        if (controlBusy() && next.lastValid && next.last.session==session_ && next.last.sequence==control_.sequence)
            applyOutcome(next.lastOutcome,next.lastReason);
        if (controlBusy() && next.active.session==session_ && next.active.sequence==control_.sequence)
            applyOutcome(Outcome::Accepted,Reason::None);
    }
    if (recovered) applyOutcome(recoveredOutcome,recoveredReason);
}
} }
