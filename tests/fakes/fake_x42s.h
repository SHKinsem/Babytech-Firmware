#pragma once
// Fake X42sProtocol "driver" for the host tests.
//
// The real X42sProtocol talks to the ESP-IDF TWAI peripheral. For the host
// build the used X42sProtocol methods are re-implemented here (fake_x42s.cpp)
// on top of a tiny in-memory bus, so the tests can:
//   * capture every frame the motor module transmits (capturedTX / txLog)
//   * inject RX frames that the module will drain (rxQueue)
//   * drive millis() and the reported controller state / bus-off (busState)
//
// The wire encoding mirrors the real codec (command byte first, 0x6B checksum)
// so assertions on the transmitted frames are meaningful.

#include <stdint.h>

#include <deque>
#include <vector>

#include "X42sProtocol.h"
#include "x42s_can_id.h"

namespace fakecan {

enum class TxKind { ReadSysParam, Enable, Stop, Move, Direct };

// Semantic view of one transmitted command, in emission order.
struct TxRecord {
    TxKind kind = TxKind::ReadSysParam;
    uint8_t addr = 0;
    X42sSysParam param = X42sSysParam::Cpos;  // ReadSysParam
    bool enableState = false;                 // Enable
    bool sync = false;                        // Enable / Stop / Move / Direct
    uint8_t dir = 0;                          // Move / Direct
    uint16_t vel = 0;                         // Move / Direct
    uint16_t accel = 0;                       // Move
    uint16_t decel = 0;                       // Move
    uint32_t magnitude = 0;                   // Move / Direct
    uint8_t motionMode = 0;                   // Move / Direct
    uint16_t currentMa = 0;                   // Move / Direct (CB only)
    // Direct only: the complete logical command as it goes on the wire
    // ([addr][FB|CB][dir][speed][angle][mode][sync]([current])[6B]), so a test
    // can assert the exact bytes of one FB/CB frame.
    std::vector<uint8_t> bytes;
    bool withCurrentLimit = false;  // Direct: CB (true) or FB (false)
};

extern std::vector<CanRawFrame> capturedTX;  // every transmitted frame, raw
extern std::vector<TxRecord> txLog;          // semantic view, one per command
extern std::deque<CanRawFrame> rxQueue;      // frames waiting to be received
extern CanControllerState busState;          // what getBusStatus() reports
extern bool busStatusAvailable;              // false -> getBusStatus() fails
extern uint32_t txErrorCounter;              // reported TX error counter
extern bool failNextMoveTx;                  // make the next move TX set an error

// Resets clocks, queues, captured traffic and controller state.
void fakeReset();

// Deterministic millis() clock.
void setMillis(uint32_t now);
void advanceMillis(uint32_t delta);

// Queues one frame for the module to receive on the next poll().
void injectRx(const CanRawFrame& frame);

// Frame builders. All produce single-packet extended data frames (address in
// bits 8..15, packet index 0), matching x42sCanIsSinglePacketDataFrame().
CanRawFrame makeFrame(uint8_t addr, const uint8_t* data, uint8_t length);
CanRawFrame makePosition(uint8_t addr, int32_t tenthsDeg);
// 0x33: the driver's target position (manual V1.0.5 p70), same layout as 0x36.
// This is the value mode-0 commands resolve against and the completion proof.
CanRawFrame makeTarget(uint8_t addr, int32_t tenthsDeg);
// 0x34: the real-time setpoint (manual V1.0.5 p71), which may be an
// intermediate trajectory value. Provided so tests can prove it is never
// accepted in place of a 0x33 target sample.
CanRawFrame makeSetpoint(uint8_t addr, int32_t tenthsDeg);
CanRawFrame makeVelocity(uint8_t addr, int32_t tenthsRpm);
CanRawFrame makeCurrent(uint8_t addr, uint16_t milliamps);
CanRawFrame makeAck(uint8_t addr, uint8_t function, uint8_t status);

// Small query helpers over txLog.
uint32_t countTx(TxKind kind);
uint32_t countTxTo(uint8_t addr, TxKind kind);
TxRecord lastTx();
bool sawStopFor(uint8_t addr);

}  // namespace fakecan
