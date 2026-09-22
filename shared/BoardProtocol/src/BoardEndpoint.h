#pragma once
#include "BoardProtocolV2.h"

namespace babytech { namespace v2 {
// Implementations must report actual feedback-backed completion, not elapsed
// time or successful transmission. All methods run on the firmware loop thread.
class Backend {
public:
    virtual ~Backend() {}
    virtual bool busy() const = 0;
    virtual bool stopping() const { return false; }
    virtual bool fault() const = 0;
    virtual bool motorsAvailable() const = 0;
    virtual Reason startMove(const Parameters& p) = 0;
    virtual Reason enable(uint8_t motor, bool enabled) = 0;
    virtual Outcome operation(Reason& reason) const = 0;
    virtual void stop() = 0;
    virtual bool stopped() const = 0;
    virtual void watch(uint8_t motor) = 0;
    virtual size_t motorFeedback(uint8_t motor, uint8_t* data, size_t capacity) const = 0;
};
class Endpoint {
public:
    explicit Endpoint(Backend& backend) : backend_(backend) {}
    void begin(uint64_t boot) { boot_=boot ? boot : 1; }
    bool handle(const Frame& request, uint32_t now, Frame& response);
    // Produces at most one event per call; call repeatedly until false.
    bool tick(uint32_t now, Frame& event);
    // Local operator reset: preserve deduplication/results, terminate ownership.
    // Caller cancels/stops the backend separately. No physical-stop claim.
    bool cancelPending(Frame& event);
    bool busy() const { return exec_.valid || stop_.valid; }
    Status status(uint32_t now) const;
    const Parameters& parameters() const { return params_; }
private:
    struct Record { bool valid=false; Frame request, response; };
    struct Session { uint64_t id=0; uint32_t high=0; Record last; };
    Backend& backend_;
    uint64_t boot_=1; uint32_t revision_=1;
    Parameters params_;
    Session sessions_[4]; Record results_[8], exec_, stop_;
    uint8_t nextResult_=0; uint32_t lastOwnerAt_=0, stopAt_=0;
    bool linkLost_=false;
    Frame finish(Record& record, Outcome outcome, Reason reason);
    bool cached(const Frame& request, Frame& response) const;
    Frame read(const Frame& request, Reader& reader, uint32_t now, bool discovery);
    Frame write(const Frame& request, Reader& reader);
};
} }
