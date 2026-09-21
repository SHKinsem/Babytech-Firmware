// Host implementation of the used X42sProtocol methods plus the fake bus.
// This file replaces the real X42sProtocol.cpp for the host test build, so the
// real driver/twai.h is never needed (the fake twai.h is empty).

#include "fake_x42s.h"

#include <cstddef>
#include <cstring>

namespace fakecan {

std::vector<CanRawFrame> capturedTX;
std::vector<TxRecord> txLog;
std::deque<CanRawFrame> rxQueue;
CanControllerState busState = CanControllerState::Running;
bool busStatusAvailable = true;
uint32_t txErrorCounter = 0;
bool failNextMoveTx = false;

namespace {
uint32_t g_millis = 0;

void record(const CanRawFrame& frame, const TxRecord& rec) {
    capturedTX.push_back(frame);
    txLog.push_back(rec);
}

void emit(uint8_t addr, const uint8_t* data, uint8_t length, const TxRecord& rec) {
    record(makeFrame(addr, data, length), rec);
}
}  // namespace

void fakeReset() {
    capturedTX.clear();
    txLog.clear();
    rxQueue.clear();
    busState = CanControllerState::Running;
    busStatusAvailable = true;
    txErrorCounter = 0;
    failNextMoveTx = false;
    g_millis = 0;
}

void setMillis(uint32_t now) { g_millis = now; }
void advanceMillis(uint32_t delta) { g_millis += delta; }

void injectRx(const CanRawFrame& frame) { rxQueue.push_back(frame); }

CanRawFrame makeFrame(uint8_t addr, const uint8_t* data, uint8_t length) {
    CanRawFrame frame;
    frame.identifier = x42sCanFrameId(addr, 0);
    frame.extended = true;
    frame.remote = false;
    frame.length = length > 8 ? 8 : length;
    for (uint8_t i = 0; i < frame.length; ++i) frame.data[i] = data[i];
    return frame;
}

CanRawFrame makePosition(uint8_t addr, int32_t tenths) {
    const uint32_t magnitude =
        tenths < 0 ? static_cast<uint32_t>(-static_cast<int64_t>(tenths))
                   : static_cast<uint32_t>(tenths);
    const uint8_t data[7] = {
        0x36,
        static_cast<uint8_t>(tenths < 0 ? 1 : 0),
        static_cast<uint8_t>((magnitude >> 24) & 0xFF),
        static_cast<uint8_t>((magnitude >> 16) & 0xFF),
        static_cast<uint8_t>((magnitude >> 8) & 0xFF),
        static_cast<uint8_t>(magnitude & 0xFF),
        0x6B,
    };
    return makeFrame(addr, data, sizeof(data));
}

CanRawFrame makeVelocity(uint8_t addr, int32_t tenths) {
    const uint32_t magnitude =
        tenths < 0 ? static_cast<uint32_t>(-static_cast<int64_t>(tenths))
                   : static_cast<uint32_t>(tenths);
    const uint8_t data[5] = {
        0x35,
        static_cast<uint8_t>(tenths < 0 ? 1 : 0),
        static_cast<uint8_t>((magnitude >> 8) & 0xFF),
        static_cast<uint8_t>(magnitude & 0xFF),
        0x6B,
    };
    return makeFrame(addr, data, sizeof(data));
}

CanRawFrame makeCurrent(uint8_t addr, uint16_t milliamps) {
    const uint8_t data[4] = {
        0x27,
        static_cast<uint8_t>((milliamps >> 8) & 0xFF),
        static_cast<uint8_t>(milliamps & 0xFF),
        0x6B,
    };
    return makeFrame(addr, data, sizeof(data));
}

CanRawFrame makeAck(uint8_t addr, uint8_t function, uint8_t status) {
    const uint8_t data[3] = {function, status, 0x6B};
    return makeFrame(addr, data, sizeof(data));
}

uint32_t countTx(TxKind kind) {
    uint32_t n = 0;
    for (size_t i = 0; i < txLog.size(); ++i) {
        if (txLog[i].kind == kind) ++n;
    }
    return n;
}

uint32_t countTxTo(uint8_t addr, TxKind kind) {
    uint32_t n = 0;
    for (size_t i = 0; i < txLog.size(); ++i) {
        if (txLog[i].kind == kind && txLog[i].addr == addr) ++n;
    }
    return n;
}

TxRecord lastTx() { return txLog.empty() ? TxRecord{} : txLog.back(); }

bool sawStopFor(uint8_t addr) {
    for (size_t i = 0; i < txLog.size(); ++i) {
        if (txLog[i].kind == TxKind::Stop && txLog[i].addr == addr) return true;
    }
    return false;
}

}  // namespace fakecan

// millis() is declared in the fake Arduino.h and read by MotorControl.cpp.
unsigned long millis() { return fakecan::g_millis; }

// --- Fake X42sProtocol implementation -------------------------------------

namespace {

// Mirrors X42sProtocol.cpp's buildReadSysParamsCommand() mapping.
bool sysParamCode(X42sSysParam param, uint8_t& first, uint8_t& second) {
    switch (param) {
        case X42sSysParam::Ver: first = 0x1F; return true;
        case X42sSysParam::Rl: first = 0x20; return true;
        case X42sSysParam::Pid: first = 0x21; return true;
        case X42sSysParam::Vbus: first = 0x24; return true;
        case X42sSysParam::Cpha: first = 0x27; return true;
        case X42sSysParam::Encl: first = 0x31; return true;
        case X42sSysParam::Tpos: first = 0x33; return true;
        case X42sSysParam::Vel: first = 0x35; return true;
        case X42sSysParam::Cpos: first = 0x36; return true;
        case X42sSysParam::Perr: first = 0x37; return true;
        case X42sSysParam::Flag: first = 0x3A; return true;
        case X42sSysParam::Org: first = 0x3B; return true;
        case X42sSysParam::Conf: first = 0x42; second = 0x6C; return true;
        case X42sSysParam::State: first = 0x43; second = 0x7A; return true;
        default: return false;
    }
}

bool isTwoCodeParam(X42sSysParam param) {
    return param == X42sSysParam::Conf || param == X42sSysParam::State;
}

}  // namespace

X42sProtocol::X42sProtocol(int txPin, int rxPin, long canSpeed)
    : txPin_(txPin), rxPin_(rxPin), canSpeed_(canSpeed) {}

bool X42sProtocol::configure(int txPin, int rxPin, long canSpeed) {
    txPin_ = txPin;
    rxPin_ = rxPin;
    canSpeed_ = canSpeed;
    return true;
}

bool X42sProtocol::begin() {
    initialized_ = true;
    return true;
}

void X42sProtocol::end() { initialized_ = false; }

bool X42sProtocol::hasTransmissionError() const { return transmissionError_; }

void X42sProtocol::clearTransmissionError() { transmissionError_ = false; }

bool X42sProtocol::getBusStatus(CanBusStatus& status) const {
    status = CanBusStatus{};
    if (!fakecan::busStatusAvailable) return false;
    status.state = fakecan::busState;
    status.txErrorCounter = fakecan::txErrorCounter;
    status.rxErrorCounter = 0;
    return true;
}

bool X42sProtocol::receive(CanRawFrame& frame, uint32_t timeout_ms) {
    (void)timeout_ms;
    if (fakecan::rxQueue.empty()) return false;
    frame = fakecan::rxQueue.front();
    fakecan::rxQueue.pop_front();
    return true;
}

bool X42sProtocol::probeReadSysParams(uint8_t addr, X42sSysParam param) {
    uint8_t first = 0;
    uint8_t second = 0;
    if (!sysParamCode(param, first, second)) return false;

    uint8_t data[3];
    uint8_t length = 0;
    data[length++] = first;
    if (isTwoCodeParam(param)) data[length++] = second;
    data[length++] = 0x6B;

    fakecan::TxRecord rec;
    rec.kind = fakecan::TxKind::ReadSysParam;
    rec.addr = addr;
    rec.param = param;
    fakecan::emit(addr, data, length, rec);
    return true;
}

void X42sProtocol::enableControl(uint8_t addr, bool state, bool sync) {
    const uint8_t data[4] = {0xF3, 0xAB, static_cast<uint8_t>(state ? 1 : 0),
                             static_cast<uint8_t>(sync ? 1 : 0)};
    fakecan::TxRecord rec;
    rec.kind = fakecan::TxKind::Enable;
    rec.addr = addr;
    rec.enableState = state;
    rec.sync = sync;
    fakecan::emit(addr, data, sizeof(data), rec);
}

void X42sProtocol::stopNow(uint8_t addr, bool sync) {
    const uint8_t data[3] = {0xFE, 0x98, static_cast<uint8_t>(sync ? 1 : 0)};
    fakecan::TxRecord rec;
    rec.kind = fakecan::TxKind::Stop;
    rec.addr = addr;
    rec.sync = sync;
    fakecan::emit(addr, data, sizeof(data), rec);
}

void X42sProtocol::positionControlWithCurrentLimit(
    uint8_t addr, uint8_t dir, uint16_t vel, uint16_t accel, uint16_t decel,
    uint32_t clk, uint8_t motionMode, bool sync, uint16_t maxCurrentMa) {
    fakecan::TxRecord rec;
    rec.kind = fakecan::TxKind::Move;
    rec.addr = addr;
    rec.dir = dir;
    rec.vel = vel;
    rec.accel = accel;
    rec.decel = decel;
    rec.magnitude = clk;
    rec.motionMode = motionMode;
    rec.sync = sync;
    rec.currentMa = maxCurrentMa;
    fakecan::txLog.push_back(rec);

    // First packet of the 0xCD command as it would hit the wire (the real
    // driver splits the full command across several packets).
    const uint8_t head[8] = {
        0xCD, dir,
        static_cast<uint8_t>((accel >> 8) & 0xFF), static_cast<uint8_t>(accel & 0xFF),
        static_cast<uint8_t>((decel >> 8) & 0xFF), static_cast<uint8_t>(decel & 0xFF),
        static_cast<uint8_t>((vel >> 8) & 0xFF), static_cast<uint8_t>(vel & 0xFF),
    };
    fakecan::capturedTX.push_back(fakecan::makeFrame(addr, head, sizeof(head)));

    if (fakecan::failNextMoveTx) {
        // Simulate a partial/failed transmission: the frame may have reached
        // the motor, so the module must stop it rather than assume nothing ran.
        fakecan::failNextMoveTx = false;
        transmissionError_ = true;
    }
}


bool X42sProtocol::sendCommand(const uint8_t* b, uint8_t n, bool recordError, bool singleShot) {
    (void)recordError; (void)singleShot;
    if (!initialized_ || !b || n < 3) return false;
    if (fakecan::failNextMoveTx) { fakecan::failNextMoveTx = false; return false; }
    for (uint8_t offset = 2, packet = 0; offset < n; ++packet) {
        CanRawFrame f; f.identifier = x42sCanFrameId(b[0], packet); f.extended = true;
        f.data[0] = b[1]; f.length = 1;
        while (offset < n && f.length < 8) f.data[f.length++] = b[offset++];
        fakecan::capturedTX.push_back(f);
        if (traceSink_) traceSink_(traceContext_, f, true);
    }
    return true;
}
