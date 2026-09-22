#include "BoardMotion.h"
#include <cstring>
using namespace babytech::v2;
namespace {
Reason mapResult(const motion::Result& r) {
    if (r.code<300) return Reason::None;
    if (r.code==400) return Reason::InvalidParam;
    if (std::strcmp(r.message,"not_enabled")==0) return Reason::InvalidState;
    if (std::strcmp(r.message,"feedback_unavailable")==0) return Reason::FeedbackStale;
    return r.code==409 ? Reason::Busy : Reason::NotReady;
}
}
Reason BoardMotion::startMove(const Parameters& p) {
    motion::MoveRequest r{uint8_t(p.motor),p.angle/10.0f,p.speed/10.0f,
        float(p.accel),float(p.decel),uint16_t(p.current)};
    if (motor_.hasFault()) return Reason::FaultActive;
    const Reason reason=mapResult(motor_.move(r));
    if (reason==Reason::None) { moving_=true; id_=uint8_t(p.motor); }
    return reason;
}
Reason BoardMotion::enable(uint8_t id,bool desired) {
    const Reason reason=mapResult(motor_.enable(id,desired));
    if (reason==Reason::None) { moving_=false; id_=id; desired_=desired; operationAt_=millis(); }
    return reason;
}
Outcome BoardMotion::operation(Reason& reason) const {
    const auto s=motor_.snapshot(id_); reason=Reason::None;
    if (s.fault || !motor_.ready()) { reason=Reason::FaultActive; return Outcome::Failed; }
    if (moving_) {
        switch(motor_.moveOutcome()) {
            case motion::MotorControl::MoveOutcome::Running:return Outcome::Accepted;
            case motion::MotorControl::MoveOutcome::Done:return Outcome::Done;
            case motion::MotorControl::MoveOutcome::Cancelled:return Outcome::Cancelled;
            default:reason=Reason::FaultActive;return Outcome::Failed;
        }
    }
    if (s.enableTimedOut) { reason=Reason::Timeout; return Outcome::Failed; }
    if (uint32_t(millis())-operationAt_>=3000) { reason=Reason::StopUnconfirmed; return Outcome::Failed; }
    if (s.enablePending || (!desired_ && s.stopPending)) return Outcome::Accepted;
    if (s.enableAck && s.enabled==desired_) return Outcome::Done;
    return Outcome::Cancelled;
}
void BoardMotion::stop() {
    since_=millis();
    const auto r=motor_.stopAll(); stopSent_=r.code<300;
    for (uint16_t id=1;id<256;++id) stopTargets_[id]=motor_.snapshot(uint8_t(id)).stopPending;
}
bool BoardMotion::stopped() const {
    if (!stopSent_ || !motor_.ready() || motor_.operationBusy()) return false;
    bool any=false; const uint32_t age=uint32_t(millis())-since_;
    for (uint16_t id=1;id<256;++id) if (stopTargets_[id]) {
        any=true; const auto s=motor_.snapshot(uint8_t(id));
        if (s.stopPending || !s.positionValid || !s.velocityValid ||
            s.positionAge>=age || s.velocityAge>=age || s.velocity < -2 || s.velocity>2) return false;
    }
    return any; // no known target means no evidence of physical stop
}
size_t BoardMotion::motorFeedback(uint8_t id,uint8_t* data,size_t capacity) const {
    const auto s=motor_.snapshot(id); Writer w(data,capacity);
    w.put((s.positionValid?1:0)|(s.velocityValid?2:0)|(s.currentValid?4:0)|
        (s.enabled?8:0)|(s.enablePending?16:0)|(s.stopPending?32:0)|(s.fault?64:0),1);
    w.put(uint32_t(s.position),4); w.put(uint32_t(s.velocity),4); w.put(s.current,2);
    w.put(s.positionAge,4); w.put(s.velocityAge,4); w.put(s.currentAge,4);
    return w.ok() ? w.size() : 0;
}
