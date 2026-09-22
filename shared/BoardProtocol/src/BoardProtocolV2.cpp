#include "BoardProtocolV2.h"
#include "BoardProtocol.h"
#include <cstring>
#include <cmath>

namespace babytech { namespace v2 {
void Writer::put(uint64_t value, size_t bytes) {
    if (bytes > 8 || size_ + bytes > capacity_) { ok_ = false; return; }
    for (size_t i = 0; i < bytes; ++i) data_[size_++] = uint8_t(value >> (8*i));
}
void Writer::raw(const uint8_t* value, size_t bytes) {
    if (size_ + bytes > capacity_) { ok_ = false; return; }
    if (bytes) std::memcpy(data_ + size_, value, bytes);
    size_ += bytes;
}
uint64_t Reader::get(size_t bytes) {
    if (bytes > 8 || offset_ + bytes > size_) { ok_ = false; return 0; }
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; ++i) value |= uint64_t(data_[offset_++]) << (8*i);
    return value;
}
size_t encode(const Frame& f, uint8_t* out, size_t capacity) {
    if (!out || f.length > kMaxPayload || capacity < kHeaderSize + f.length + 2 ||
        uint8_t(f.kind) > 2) return 0;
    Writer w(out, capacity);
    w.put(0x4D42,2); w.put(2,1); w.put(uint8_t(f.cmd),1); w.put(uint8_t(f.kind),1);
    w.put(f.length,2); w.put(f.session,8); w.put(f.sequence,4); w.raw(f.payload,f.length);
    const uint16_t crc = babytech::crc16(out,w.size()); w.put(crc,2);
    return w.ok() ? w.size() : 0;
}
void Parser::discard(size_t n) {
    size_ -= n;
    std::memmove(buffer_,buffer_+n,size_);
}
bool Parser::push(uint8_t byte, Frame& f) {
    if (size_ == sizeof(buffer_)) discard(1);
    buffer_[size_++] = byte;
    while (size_) {
        if (buffer_[0] != 0x42) { discard(1); continue; }
        if (size_ < 2) return false;
        if (buffer_[1] != 0x4D) { discard(1); continue; }
        if (size_ < 5) return false;
        if (buffer_[2] != 2 || buffer_[4] > 2) { discard(1); continue; }
        if (size_ < kHeaderSize) return false;
        Reader h(buffer_+3,kHeaderSize-3);
        const Cmd cmd = Cmd(h.get(1)); const Kind kind = Kind(h.get(1));
        const uint16_t length = h.get(2);
        const uint64_t session = h.get(8); const uint32_t seq = h.get(4);
        if (length > kMaxPayload) { discard(1); continue; }
        const size_t total = kHeaderSize + length + 2;
        if (size_ < total) return false;
        Reader tail(buffer_+total-2,2);
        if (tail.get(2) != babytech::crc16(buffer_,total-2)) { discard(1); continue; }
        f = Frame{}; f.cmd=cmd; f.kind=kind; f.session=session; f.sequence=seq; f.length=length;
        std::memcpy(f.payload,buffer_+kHeaderSize,length); discard(total); return true;
    }
    return false;
}
Frame reply(const Frame& q, Outcome outcome, Reason reason) {
    Frame f; f.cmd=q.cmd; f.kind=Kind::Response; f.session=q.session; f.sequence=q.sequence;
    Writer w(f.payload,sizeof(f.payload)); w.put(uint8_t(outcome),1); w.put(uint16_t(reason),2);
    f.length=w.size(); return f;
}
bool result(const Frame& f, Outcome& o, Reason& reason) {
    if (f.kind == Kind::Request || f.length < 3 || f.payload[0] > uint8_t(Outcome::Cancelled)) return false;
    Reader r(f.payload,3); o=Outcome(r.get(1)); reason=Reason(r.get(2));
    return uint16_t(reason) <= uint16_t(Reason::InternalError);
}
bool sameRequest(const Frame& a, const Frame& b) {
    return a.cmd==b.cmd && a.kind==b.kind && a.session==b.session && a.sequence==b.sequence &&
        a.length==b.length && std::memcmp(a.payload,b.payload,a.length)==0;
}
void writeStatus(Writer& w, const Status& s) {
    w.put(s.boot,8); w.put(s.uptime,4); w.put(s.revision,4); w.put(uint8_t(s.state),1);
    w.put(uint16_t(s.fault),2); w.put(s.motors,1); w.put(s.active.session,8); w.put(s.active.sequence,4);
    w.put(s.last.session,8); w.put(s.last.sequence,4); w.put(uint8_t(s.lastOutcome),1);
    w.put(uint16_t(s.lastReason),2); w.put(s.lastValid,1); w.put(s.stage,2);
}
bool readStatus(Reader& r, Status& s) {
    s.boot=r.get(8); s.uptime=r.get(4); s.revision=r.get(4); s.state=State(r.get(1));
    s.fault=Reason(r.get(2)); s.motors=r.get(1)!=0; s.active.session=r.get(8); s.active.sequence=r.get(4);
    s.last.session=r.get(8); s.last.sequence=r.get(4); s.lastOutcome=Outcome(r.get(1));
    s.lastReason=Reason(r.get(2)); s.lastValid=r.get(1)!=0; s.stage=r.get(2);
    return r.done() && s.boot && uint8_t(s.state)<=4 && uint8_t(s.lastOutcome)<=5 &&
        uint16_t(s.fault)<=14 && uint16_t(s.lastReason)<=14;
}
uint32_t Parameters::get(uint16_t field) const {
    switch(field) { case 1:return motor; case 2:return uint32_t(angle); case 3:return speed;
        case 4:return accel; case 5:return decel; case 6:return current; default:return 0; }
}
bool Parameters::set(uint16_t field,uint32_t value) {
    switch(field) { case 1:motor=value;break;
        case 2:angle=value<=0x7FFFFFFFu ? int32_t(value) : -1-int32_t(~value);break;
        case 3:speed=value;break; case 4:accel=value;break; case 5:decel=value;break;
        case 6:current=value;break; default:return false; } return true;
}
bool validParameters(const Parameters& p) {
    if (!p.motor || p.motor>255 || !p.angle || p.angle < -36000 || p.angle>36000 ||
        !p.speed || p.speed>1200 || !p.accel || p.accel>240 || !p.decel || p.decel>240 ||
        p.current<100 || p.current>5000) return false;
    const double d=std::abs(double(p.angle))/10, v=p.speed*0.6, a=p.accel*6.0, b=p.decel*6.0;
    const double ramp=v*v/(2*a)+v*v/(2*b);
    const double peak=std::sqrt(2*d*a*b/(a+b));
    return (ramp<=d ? v/a+v/b+(d-ramp)/v : peak/a+peak/b)<=60.0;
}
Frame readField(uint64_t session,uint32_t seq,uint64_t boot,uint8_t object,uint16_t instance,uint16_t field) {
    Frame q; q.session=session; q.sequence=seq; Writer w(q.payload,kMaxPayload);
    w.put(boot,8); w.put(1,1); w.put(object,1); w.put(instance,2); w.put(field,2); q.length=w.size(); return q;
}
} }
