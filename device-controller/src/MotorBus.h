#pragma once

#include "CanQueryScheduler.h"
#include "X42sProtocol.h"

namespace motion {

// Owns the single motor protocol driver and the one automatic-query budget.
// Explicit commands retain their existing order; only dispatchQueries() may
// send a budgeted read, after the controller has serviced stops and writers.
class MotorBus {
public:
    MotorBus(int tx, int rx, long bitrate) : driver_(tx, rx, bitrate) {
        driver_.setTraceSink(traceForwarder, this);
    }
    MotorBus(const MotorBus&) = delete;
    MotorBus& operator=(const MotorBus&) = delete;

    using TraceSink = X42sProtocol::TraceSink;
    using QuerySentObserver = void (*)(void*, uint8_t, uint8_t, bool);

    // Driver lifecycle, receive path and diagnostics.
    bool configure(int tx, int rx, long bitrate) { return driver_.configure(tx, rx, bitrate); }
    bool begin() { return driver_.begin(); }
    void end() { driver_.end(); }
    bool receive(CanRawFrame& frame, uint32_t timeoutMs) {
        return driver_.receive(frame, timeoutMs);
    }
    bool getBusStatus(CanBusStatus& status) const { return driver_.getBusStatus(status); }
    void setTraceSink(TraceSink sink, void* context) {
        traceSink_ = sink;
        traceContext_ = context;
    }
    bool hasTransmissionError() const { return driver_.hasTransmissionError(); }
    void clearTransmissionError() { driver_.clearTransmissionError(); }

    // All automatic reads, including Sync, share this budget. The scheduler
    // itself remains available only to the existing SyncRuntime contract and
    // legacy diagnostics; new clients should use the narrow methods below.
    CanQueryScheduler& syncScheduler() { return queries_; }
    const CanQueryScheduler::Config& queryBudget() const { return queries_.config(); }
    const CanQueryScheduler::Statistics& queryStatistics() const { return queries_.statistics(); }
    bool configureQueryBudget(const CanQueryScheduler::Config& value) {
        return queries_.configure(value);
    }
    uint8_t queryInflight() const { return queries_.inflight(); }
    void exclusiveSyncQueries(bool enabled) { queries_.exclusiveSync(enabled); }
    bool demandQuery(uint8_t id, uint8_t field, CanQueryScheduler::Owner owner,
                     uint32_t periodMs, uint32_t leaseMs, uint8_t priority, uint32_t now) {
        return queries_.demand(id, field, owner, periodMs, leaseMs, priority, now);
    }
    void releaseQueries(CanQueryScheduler::Owner owner) { queries_.release(owner); }
    void releaseQuery(uint8_t id, uint8_t field, CanQueryScheduler::Owner owner) {
        queries_.release(id, field, owner);
    }
    void receiveQuery(uint8_t id, uint8_t field, uint32_t now) {
        queries_.receive(id, field, now);
    }
    CanQueryScheduler::Evidence queryEvidence(uint8_t id, uint8_t field) const {
        return queries_.evidence(id, field);
    }
    void dispatchQueries(uint32_t now, bool ready,
                         QuerySentObserver observer = nullptr, void* context = nullptr) {
        queries_.poll(now);
        if (!ready) return;
        QueryDispatch dispatch{this, observer, context};
        queries_.dispatch(now, sendQuery, &dispatch);
    }

    // Exact TX forwarding. Validation and supervision still belong to the
    // existing caller; this layer adds no retries, delays or hidden commands.
    bool probeReadSysParams(uint8_t id, X42sSysParam param) {
        return driver_.probeReadSysParams(id, param);
    }
    bool sendValidatedCommand(const uint8_t* bytes, uint8_t length) {
        return driver_.sendValidatedCommand(bytes, length);
    }
    bool sendRawLogical(const uint8_t* bytes, uint8_t length) {
        return driver_.sendRawLogical(bytes, length);
    }
    bool sendRawFrame(uint32_t id, bool extended, const uint8_t* data, uint8_t length) {
        return driver_.sendRawFrame(id, extended, data, length);
    }
    void stopNow(uint8_t id, bool sync) { driver_.stopNow(id, sync); }
    void enableControl(uint8_t id, bool enabled, bool sync) {
        driver_.enableControl(id, enabled, sync);
    }
    void positionControlWithCurrentLimit(uint8_t id, uint8_t dir, uint16_t speed,
                                         uint16_t accel, uint16_t decel, uint32_t angle,
                                         uint8_t mode, bool sync, uint16_t currentMa) {
        driver_.positionControlWithCurrentLimit(id, dir, speed, accel, decel, angle,
                                                mode, sync, currentMa);
    }
    void passthroughPositionControl(uint8_t id, uint8_t dir, uint16_t speed,
                                    uint32_t angle, uint8_t mode, bool sync) {
        driver_.passthroughPositionControl(id, dir, speed, angle, mode, sync);
    }
    void passthroughPositionControlWithCurrentLimit(uint8_t id, uint8_t dir,
                                                    uint16_t speed, uint32_t angle,
                                                    uint8_t mode, bool sync,
                                                    uint16_t currentMa) {
        driver_.passthroughPositionControlWithCurrentLimit(id, dir, speed, angle,
                                                           mode, sync, currentMa);
    }

private:
    struct QueryDispatch {
        MotorBus* bus;
        QuerySentObserver observer;
        void* context;
    };

    static bool sendQuery(void* context, uint8_t id, uint8_t field) {
        const auto& dispatch = *static_cast<QueryDispatch*>(context);
        MotorBus& self = *dispatch.bus;
        const uint8_t bytes[] = {id, field, 0x6B};
        self.clearTransmissionError();
        bool sent = false;
        switch (field) {
            case 0x36: sent = self.probeReadSysParams(id, X42sSysParam::Cpos); break;
            case 0x35: sent = self.probeReadSysParams(id, X42sSysParam::Vel); break;
            case 0x33: sent = self.probeReadSysParams(id, X42sSysParam::Tpos); break;
            case 0x3A: sent = self.probeReadSysParams(id, X42sSysParam::Flag); break;
            case 0x3B: sent = self.probeReadSysParams(id, X42sSysParam::Org); break;
            case 0x27: sent = self.probeReadSysParams(id, X42sSysParam::Cpha); break;
            case 0x22: sent = self.sendRawLogical(bytes, sizeof(bytes)); break;
            default: break;
        }
        self.clearTransmissionError();
        if (dispatch.observer) dispatch.observer(dispatch.context, id, field, sent);
        return sent;
    }

    static void traceForwarder(void* context, const CanRawFrame& frame, bool tx) {
        MotorBus& self = *static_cast<MotorBus*>(context);
        if (tx) self.queries_.noteTraffic(millis());
        if (self.traceSink_) self.traceSink_(self.traceContext_, frame, tx);
    }

    X42sProtocol driver_;
    CanQueryScheduler queries_;
    TraceSink traceSink_ = nullptr;
    void* traceContext_ = nullptr;
};

}  // namespace motion
