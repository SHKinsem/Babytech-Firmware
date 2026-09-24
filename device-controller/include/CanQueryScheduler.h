#pragma once
#include <stdint.h>

namespace motion {

// One board-wide budget. Call demand() from clients; only dispatch() may issue
// an automatic query. No catch-up credit accumulates and no retries are made
// inside dispatch. Explicit raw traffic bypasses this class, but noteTraffic()
// defers optional queries. All durations must be < 2^31 ms.
class CanQueryScheduler {
public:
    enum Owner : uint8_t { Controller, Await, Sync, Page, OwnerCount };
    struct Config {
        uint16_t queriesPerSecond=10, gapMs=100, timeoutMs=500, cooldownMs=500;
        uint8_t maxInflight=2;
        bool valid() const {
            return queriesPerSecond>=1 && queriesPerSecond<=100 && gapMs>=2 &&
                timeoutMs>=20 && timeoutMs<=10000 && cooldownMs>=gapMs &&
                cooldownMs<=10000 && maxInflight>=1 && maxInflight<=8;
        }
    };
    struct Statistics {
        uint32_t queries=0, responses=0, unanswered=0, sendErrors=0;
        uint32_t latencySumMs=0, latencyMaxMs=0, rejectedDemands=0;
    };
    struct Evidence {
        uint32_t sentAt=0, receivedAt=0, sampleRequestAt=0;
        bool pending=false, received=false;
    };
    using Sender=bool (*)(void*,uint8_t,uint8_t);
    static constexpr uint8_t capacity=64;
    const Config& config() const {return config_;}
    const Statistics& statistics() const {return stats_;}
    bool configure(const Config& value) {
        if (!value.valid() || inflight()) return false;
        config_=value; return true;
    }
    // Exclusive synchronization phases suspend unrelated owners rather than
    // granting synchronization a second budget. Outstanding queries still count.
    void exclusiveSync(bool value) {exclusive_=value;}
    bool demand(uint8_t id,uint8_t field,Owner owner,uint32_t periodMs,
                uint32_t leaseMs,uint8_t priority,uint32_t now) {
        if (!id || owner>=OwnerCount || !supported(field) || !periodMs ||
            periodMs>60000 || !leaseMs || leaseMs>60000) return false;
        Entry* e=find(id,field);
        if (!e) {
            for (auto& candidate:entries_) {
                if (!candidate.id || (!candidate.evidence.pending &&
                    !hasDemand(candidate,now) && (!candidate.attempted ||
                    elapsed(now,candidate.attemptAt)>=config_.timeoutMs+config_.cooldownMs))) {
                    e=&candidate; *e=Entry{}; e->id=id; e->field=field; e->createdAt=now; break;
                }
            }
        }
        if (!e) {++stats_.rejectedDemands;return false;}
        auto& d=e->demands[owner];
        d.active=true; d.renewed=now; d.lease=leaseMs; d.period=periodMs; d.priority=priority;
        return true;
    }
    void release(Owner owner) {
        if (owner>=OwnerCount) return;
        for (auto& e:entries_) e.demands[owner].active=false;
    }
    void release(uint8_t id,uint8_t field,Owner owner) {
        auto* e=find(id,field);
        if (e && owner<OwnerCount) e->demands[owner].active=false;
    }
    void noteTraffic(uint32_t now) {lastTraffic_=now; trafficSeen_=true;}
    // Feed only successfully parsed response frames, not ACKs or malformed RX.
    void receive(uint8_t id,uint8_t field,uint32_t now) {
        Entry* e=find(id,field);
        if (!e || !e->evidence.pending) return;
        const uint32_t latency=elapsed(now,e->evidence.sentAt);
        if (latency>=config_.timeoutMs) {expire(*e,now);return;}
        e->evidence.pending=false; e->evidence.received=true; e->evidence.receivedAt=now;
        e->evidence.sampleRequestAt=e->evidence.sentAt;
        ++stats_.responses; stats_.latencySumMs+=latency;
        if(latency>stats_.latencyMaxMs) stats_.latencyMaxMs=latency;
    }
    Evidence evidence(uint8_t id,uint8_t field) const {
        for (const auto& e:entries_) if(e.id==id && e.field==field) return e.evidence;
        return {};
    }
    uint8_t inflight() const {
        uint8_t count=0;for(const auto& e:entries_) if(e.evidence.pending) ++count;return count;
    }
    void poll(uint32_t now) {
        for(auto& e:entries_)
            if(e.evidence.pending && elapsed(now,e.evidence.sentAt)>=config_.timeoutMs) expire(e,now);
    }
    // At most one query; stopping/ordinary operations run before this service.
    bool dispatch(uint32_t now,Sender send,void* context) {
        poll(now);
        if (!send || inflight()>=config_.maxInflight) return false;
        const uint32_t rateGap=(1000u+config_.queriesPerSecond-1)/config_.queriesPerSecond;
        const uint32_t gap=rateGap>config_.gapMs?rateGap:config_.gapMs;
        if ((dispatchSeen_ && elapsed(now,lastDispatch_)<gap) ||
            (trafficSeen_ && elapsed(now,lastTraffic_)<config_.gapMs)) return false;
        Entry* best=nullptr; uint32_t bestScore=0;
        for (uint8_t n=0;n<capacity;++n) {
            Entry& e=entries_[(cursor_+n)%capacity];
            if(!e.id || e.evidence.pending || (e.cooling && elapsed(now,e.coolAt)<config_.cooldownMs)) continue;
            for(uint8_t owner=0;owner<OwnerCount;++owner) {
                const Demand& d=e.demands[owner];
                if(exclusive_ && owner!=Sync) continue;
                if(!active(d,now) || (e.attempted && elapsed(now,e.attemptAt)<d.period)) continue;
                const uint32_t waited=e.attempted ? elapsed(now,e.attemptAt)-d.period : elapsed(now,e.createdAt);
                const uint32_t score=waited+uint32_t(d.priority)*gap;
                if(!best || score>bestScore) {best=&e;bestScore=score;}
            }
        }
        if(!best) return false;
        cursor_=uint8_t((best-entries_+1)%capacity);
        lastDispatch_=now;dispatchSeen_=true;
        best->attempted=true;best->attemptAt=now;best->cooling=false;
        best->evidence.received=false;
        if(!send(context,best->id,best->field)) {
            ++stats_.sendErrors;best->cooling=true;best->coolAt=now;return false;
        }
        ++stats_.queries;best->evidence.pending=true;best->evidence.sentAt=now;
        return true;
    }
    static bool supported(uint8_t field) {
        return field==0x36 || field==0x35 || field==0x3A || field==0x3B ||
            field==0x33 || field==0x27 || field==0x22;
    }
private:
    struct Demand {
        uint32_t renewed=0,lease=0,period=0;
        uint8_t priority=0; bool active=false;
    };
    struct Entry {
        uint8_t id=0,field=0;
        Demand demands[OwnerCount];
        Evidence evidence;
        uint32_t attemptAt=0,coolAt=0,createdAt=0;
        bool attempted=false,cooling=false;
    };
    static uint32_t elapsed(uint32_t a,uint32_t b) {return a-b;}
    static bool active(const Demand& d,uint32_t now) {return d.active && elapsed(now,d.renewed)<d.lease;}
    static bool hasDemand(const Entry& e,uint32_t now) {
        for (const auto& d : e.demands) {
            if (active(d, now)) return true;
        }
        return false;
    }
    Entry* find(uint8_t id,uint8_t field) {
        for (auto& e : entries_) {
            if (e.id == id && e.field == field) return &e;
        }
        return nullptr;
    }
    void expire(Entry& e,uint32_t now) {
        e.evidence.pending=false;e.cooling=true;e.coolAt=now;++stats_.unanswered;
    }
    Config config_;
    Statistics stats_;
    Entry entries_[capacity];
    uint32_t lastDispatch_=0,lastTraffic_=0;
    bool dispatchSeen_=false,trafficSeen_=false,exclusive_=false;
    uint8_t cursor_=0;
};
} // namespace motion
