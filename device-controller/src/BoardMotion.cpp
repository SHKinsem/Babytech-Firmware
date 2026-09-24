#include "BoardMotion.h"
#include <cstring>
using namespace babytech::v2;
namespace {
Reason mapResult(const motion::DeviceReceipt& r) {
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
    const Reason reason=mapResult(api_.requestMove(r));
    if (reason==Reason::None) { moving_=true; id_=uint8_t(p.motor); }
    return reason;
}
Reason BoardMotion::enable(uint8_t id,bool desired) {
    const Reason reason=mapResult(api_.requestEnable(id,desired));
    if (reason==Reason::None) { moving_=false; id_=id; desired_=desired; operationAt_=millis(); }
    return reason;
}
Outcome BoardMotion::operation(Reason& reason) const {
    const auto s=api_.readSnapshot(id_); reason=Reason::None;
    if (s.motor.fault || !s.busReady) { reason=Reason::FaultActive; return Outcome::Failed; }
    if (moving_) {
        switch(s.manualMove) {
            case motion::DeviceMoveStage::Running:return Outcome::Accepted;
            case motion::DeviceMoveStage::Reached:return Outcome::Done;
            case motion::DeviceMoveStage::Cancelled:return Outcome::Cancelled;
            default:reason=Reason::FaultActive;return Outcome::Failed;
        }
    }
    if (s.motor.enableTimedOut) { reason=Reason::Timeout; return Outcome::Failed; }
    if (uint32_t(millis())-operationAt_>=3000) { reason=Reason::StopUnconfirmed; return Outcome::Failed; }
    if (s.motor.enablePending || (!desired_ && s.motor.stopPending)) return Outcome::Accepted;
    if (s.motor.enableProtocolAck && s.motor.enabledConfirmed==desired_) return Outcome::Done;
    return Outcome::Cancelled;
}
void BoardMotion::stop() {
    since_=millis();
    const auto r=api_.requestStopAll(); stopSent_=r.code<300;
    for (uint16_t id=1;id<256;++id) stopTargets_[id]=api_.readMotorObservation(uint8_t(id)).stopPending;
}
bool BoardMotion::stopped() const {
    if (!stopSent_ || !motor_.ready() || motor_.operationBusy()) return false;
    bool any=false; const uint32_t age=uint32_t(millis())-since_;
    for (uint16_t id=1;id<256;++id) if (stopTargets_[id]) {
        any=true; const auto s=api_.readMotorObservation(uint8_t(id));
        if (s.stopPending || !s.positionValid || !s.velocityValid ||
            s.positionAgeMs>=age || s.velocityAgeMs>=age || s.velocityTenthsRpm < -2 || s.velocityTenthsRpm>2) return false;
    }
    return any; // no known target means no evidence of physical stop
}
size_t BoardMotion::motorFeedback(uint8_t id,uint8_t* data,size_t capacity) const {
    const auto s=api_.readMotorObservation(id); Writer w(data,capacity);
    w.put((s.positionValid?1:0)|(s.velocityValid?2:0)|(s.currentValid?4:0)|
        (s.enabledConfirmed?8:0)|(s.enablePending?16:0)|(s.stopPending?32:0)|(s.fault?64:0),1);
    w.put(uint32_t(s.positionTenths),4); w.put(uint32_t(s.velocityTenthsRpm),4); w.put(s.currentMa,2);
    w.put(s.positionAgeMs,4); w.put(s.velocityAgeMs,4); w.put(s.currentAgeMs,4);
    return w.ok() ? w.size() : 0;
}
