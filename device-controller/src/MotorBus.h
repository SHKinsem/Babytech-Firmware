#pragma once

#include "X42sProtocol.h"

namespace motion {

// Owns the single motor protocol driver. All motor TX, RX and bus diagnostics
// pass through this boundary; future arbitration can live here without giving
// each caller its own driver. This first slice deliberately forwards every call
// immediately and preserves the driver's existing timing and error semantics.
class MotorBus {
public:
    MotorBus(int tx, int rx, long bitrate) : driver_(tx, rx, bitrate) {}
    MotorBus(const MotorBus&) = delete;
    MotorBus& operator=(const MotorBus&) = delete;

    using TraceSink = X42sProtocol::TraceSink;

    // Driver lifecycle, receive path and diagnostics.
    bool configure(int tx, int rx, long bitrate) { return driver_.configure(tx, rx, bitrate); }
    bool begin() { return driver_.begin(); }
    void end() { driver_.end(); }
    bool receive(CanRawFrame& frame, uint32_t timeoutMs) {
        return driver_.receive(frame, timeoutMs);
    }
    bool getBusStatus(CanBusStatus& status) const { return driver_.getBusStatus(status); }
    void setTraceSink(TraceSink sink, void* context) { driver_.setTraceSink(sink, context); }
    bool hasTransmissionError() const { return driver_.hasTransmissionError(); }
    void clearTransmissionError() { driver_.clearTransmissionError(); }

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
    X42sProtocol driver_;
};

}  // namespace motion
