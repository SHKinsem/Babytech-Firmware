#pragma once
#include <stdint.h>

namespace motion {

// Passive, fixed-size evidence. This class has no transport and cannot delay,
// retry or stop a command. History bits survive ring eviction and new runs:
// the protocol has no request ID, so a second same-opcode ACK is ambiguous.
class QueueDiagnostics {
public:
    enum class Confirmation : uint8_t {
        Submitted, Unconfirmed, Accepted, CompletionReported, Rejected,
        AssociationUncertain, SendFailed, RawSubmitted, NoMotionReported
    };
    struct Event {
        uint32_t sequence=0, run=0, iteration=0, sentAt=0, responseAt=0;
        uint16_t line=0;
        uint8_t id=0, function=0, code=0;
        Confirmation confirmation=Confirmation::Submitted;
        bool ambiguous=false, responseSeen=false, raw=false;
    };
    static constexpr uint8_t capacity=64;
    static constexpr uint32_t observationMs=2000;
    void beginRun(uint32_t run) {currentRun_=run;hasAlert_=false;}
    const Event* alert() const {return hasAlert_ ? &alert_ : nullptr;}

    static const char* name(Confirmation value) {
        switch (value) {
            case Confirmation::Submitted: return "submitted";
            case Confirmation::Unconfirmed: return "receive_unconfirmed";
            case Confirmation::Accepted: return "driver_accepted";
            case Confirmation::CompletionReported: return "driver_completion_reported";
            case Confirmation::Rejected: return "driver_rejected";
            case Confirmation::AssociationUncertain: return "association_uncertain";
            case Confirmation::SendFailed: return "send_failed";
            case Confirmation::RawSubmitted: return "raw_submitted";
            case Confirmation::NoMotionReported: return "driver_no_motion_reported";
        }
        return "receive_unconfirmed";
    }
    static uint8_t functionBit(uint8_t function) {
        switch (function) {
            case 0xF3: return 1; case 0xCD: return 2; case 0x9A: return 4;
            case 0xC5: return 8; case 0xC6: return 16; case 0xFE: return 32;
            default: return 0;
        }
    }
    void submit(uint32_t run, uint32_t iteration, uint16_t line,
                uint8_t id, uint8_t function, uint32_t now, bool sent, bool raw=false) {
        if (!raw && (!id || !functionBit(function))) return;
        Event& e=events_[next_];
        e=Event{};
        e.sequence=++sequence_; e.run=run; e.iteration=iteration; e.line=line;
        e.id=id; e.function=function; e.sentAt=now;
        e.raw=raw;
        e.ambiguous=(history_[id] & functionBit(function)) != 0;
        history_[id] |= functionBit(function);
        e.confirmation=!sent ? Confirmation::SendFailed : raw ? Confirmation::RawSubmitted : Confirmation::Submitted;
        retainAlert(e);
        next_=(next_+1)%capacity;
        if (count_<capacity) ++count_; else ++dropped_;
    }
    // Called for valid control replies BEFORE the controller's interest filter.
    void response(uint8_t id, uint8_t function, uint8_t code, uint32_t now) {
        for (uint8_t i=0;i<count_;++i) {
            Event& e=events_[(next_+capacity-1-i)%capacity];
            if (e.raw || e.id!=id || e.function!=function) continue;
            e.responseSeen=true; e.responseAt=now;
            // A subsequent success cannot erase a rejection. Raw code is kept
            // even when attribution is uncertain, without failing the sender.
            if (e.code!=0xE2 && e.code!=0xEE) e.code=code;
            if (e.confirmation==Confirmation::SendFailed) return;
            if (e.ambiguous) e.confirmation=Confirmation::AssociationUncertain;
            else if (e.code==0xE2 || e.code==0xEE) e.confirmation=Confirmation::Rejected;
            else if (code==0x9F) e.confirmation=Confirmation::CompletionReported;
            else if (function==0x9A && (code==0x12 || code==0x22)) e.confirmation=Confirmation::NoMotionReported;
            else if (code==2 && e.confirmation!=Confirmation::CompletionReported)
                e.confirmation=Confirmation::Accepted;
            retainAlert(e);
            return;
        }
    }
    void poll(uint32_t now) {
        for (uint8_t i=0;i<count_;++i) {
            Event& e=events_[i];
            if (e.confirmation==Confirmation::Submitted && now-e.sentAt>=observationMs)
                e.confirmation=Confirmation::Unconfirmed;
            retainAlert(e);
        }
    }
    // Unknown raw traffic can invalidate association for any control opcode.
    void invalidate(uint8_t id) {
        if (id) history_[id]=0xFF;
        else for (unsigned i=1;i<256;++i) history_[i]=0xFF;
        for(auto& e:events_) if(e.sequence && !e.raw && (!id || e.id==id)) {
            e.ambiguous=true;
            if(e.responseSeen && e.confirmation!=Confirmation::SendFailed && e.confirmation!=Confirmation::Rejected)
                e.confirmation=Confirmation::AssociationUncertain;
            retainAlert(e);
        }
    }
    uint8_t count() const { return count_; }
    uint32_t dropped() const { return dropped_; }
    const Event& at(uint8_t index) const {
        return events_[(next_+capacity-count_+index)%capacity];
    }
private:
    void retainAlert(const Event& e) {
        if (hasAlert_ || e.run!=currentRun_) return;
        if(e.confirmation==Confirmation::Unconfirmed || e.confirmation==Confirmation::Rejected ||
           e.confirmation==Confirmation::AssociationUncertain || e.confirmation==Confirmation::SendFailed) {
            alert_=e;hasAlert_=true;
        }
    }
    uint32_t currentRun_=0;
    Event alert_;
    bool hasAlert_=false;
    Event events_[capacity];
    uint8_t history_[256]={};
    uint8_t next_=0, count_=0;
    uint32_t sequence_=0, dropped_=0;
};
} // namespace motion
