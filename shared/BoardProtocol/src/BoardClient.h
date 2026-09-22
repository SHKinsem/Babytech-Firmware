#pragma once
#include "BoardProtocolV2.h"

namespace babytech { namespace v2 {
class Client {
public:
    void begin(uint64_t session) { session_=session ? session : 1; }
    bool query(uint32_t now,Frame& out,bool force=false);
    bool submit(Frame& request,uint32_t now);
    void receive(const Frame& frame,uint32_t now);
    bool connected(uint32_t now) const { return haveStatus_ && now-statusAt_<kLinkTimeoutMs; }
    bool controlBusy() const { return haveControl_ && !controlKnownTerminal_; }
    bool controlUnknown(uint32_t now) const {
        return haveControl_ && (!controlKnownTerminal_ &&
            (!connected(now) || (!controlReplied_ && now-controlAt_>=kLinkTimeoutMs)));
    }
    uint64_t boot() const { return boot_; }
    const Status& status() const { return status_; }
    const Parameters& parameters() const { return params_; }
    uint32_t parameterRevision() const { return paramRevision_; }
    uint32_t responses() const { return responses_; }
    uint32_t statusAt() const { return statusAt_; }
    uint32_t controlSequence() const { return control_.sequence; }
    Outcome controlOutcome() const { return controlOutcome_; }
    Reason controlReason() const { return controlReason_; }
    bool haveControl() const { return haveControl_; }
    bool controlReplied() const { return controlReplied_; }
    uint8_t motorFlags(uint32_t now) const {
        return now-motorAt_<=motorFreshFor_ ? motorFlags_ : uint8_t(motorFlags_ & ~7u);
    }
private:
    uint64_t session_=1,boot_=0; uint32_t sequence_=0,statusAt_=0,queryAt_=0,controlAt_=0,responses_=0;
    bool haveStatus_=false,pendingQuery_=false,haveControl_=false;
    bool controlReplied_=false,controlKnownTerminal_=false;
    Frame query_,control_;
    Outcome controlOutcome_=Outcome::Accepted; Reason controlReason_=Reason::None;
    Status status_; Parameters params_; uint32_t paramRevision_=0;
    uint8_t poll_=0,motorFlags_=0,resultSlot_=0;
    uint32_t motorAt_=0,motorFreshFor_=0;
    bool stamp(Frame& f);
    void applyOutcome(Outcome o,Reason reason);
};
} }
