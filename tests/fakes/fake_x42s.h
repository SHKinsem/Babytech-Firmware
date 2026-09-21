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

enum class TxKind { ReadSysParam, Enable, Stop, Move };

// Semantic view of one transmitted command, in emission order.
struct TxRecord {
    TxKind kind = TxKind::ReadSysParam;
    uint8_t addr = 0;
    X42sSysParam param = X42sSysParam::Cpos;  // ReadSysParam
    bool enableState = false;                 // Enable
    bool sync = false;                        // Enable / Stop
    uint8_t dir = 0;                          // Move
    uint16_t vel = 0;                         // Move
    uint16_t accel = 0;                       // Move
    uint16_t decel = 0;                       // Move
    uint32_t magnitude = 0;                   // Move
    uint8_t motionMode = 0;                   // Move
    uint16_t currentMa = 0;                   // Move
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
CanRawFrame makeVelocity(uint8_t addr, int32_t tenthsRpm);
CanRawFrame makeCurrent(uint8_t addr, uint16_t milliamps);
CanRawFrame makeAck(uint8_t addr, uint8_t function, uint8_t status);

// Small query helpers over txLog.
uint32_t countTx(TxKind kind);
uint32_t countTxTo(uint8_t addr, TxKind kind);
TxRecord lastTx();
bool sawStopFor(uint8_t addr);

}  // namespace fakecan
