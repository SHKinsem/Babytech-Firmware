#pragma once

#include <Arduino.h>
#include "driver/twai.h"

enum class X42sSysParam : uint8_t {
    Ver = 0,
    Rl = 1,
    Pid = 2,
    Vbus = 3,
    Cpha = 5,
    Encl = 7,
    Tpos = 8,
    Vel = 9,
    Cpos = 10,
    Perr = 11,
    Flag = 13,
    Conf = 14,
    State = 15,
    Org = 16,
};

enum class CanControllerState : uint8_t {
    Unavailable,
    Stopped,
    Running,
    BusOff,
    Recovering,
};

struct CanBusStatus {
    CanControllerState state = CanControllerState::Unavailable;
    bool errorPassive = false;
    uint32_t txErrorCounter = 0;
    uint32_t rxErrorCounter = 0;
    uint32_t txFailedCount = 0;
    uint32_t rxMissedCount = 0;
    uint32_t rxOverrunCount = 0;
    uint32_t busErrorCount = 0;
};

struct CanRawFrame {
    uint32_t identifier = 0;
    uint8_t data[8] = {};
    uint8_t length = 0;
    bool extended = false;
    bool remote = false;
};

class X42sProtocol {
public:
    // 替换了原来的 CanBusManager，改用引脚和波特率初始化
    explicit X42sProtocol(int txPin, int rxPin, long canSpeed = 500E3);

    // 新增：底层驱动初始化和接收接口
    bool configure(int txPin, int rxPin, long canSpeed);
    bool begin();
    void end();
    bool receive(CanRawFrame& frame, uint32_t timeout_ms = 0);
    bool receive(uint32_t *id, uint8_t *data, uint8_t *len, uint32_t timeout_ms = 0);
    bool hasTransmissionError() const;
    void clearTransmissionError();
    bool getBusStatus(CanBusStatus& status) const;

    // Shared X firmware command API used by the X28S and X42S motors.
    void resetCurPosToZero(uint8_t addr);
    void resetClogProtection(uint8_t addr);
    bool readSysParams(uint8_t addr, X42sSysParam param);
    bool probeReadSysParams(uint8_t addr, X42sSysParam param);
    void modifyCtrlMode(uint8_t addr, bool save, uint8_t ctrlMode);
    void configureRealtimePositionFeedback(uint8_t addr, uint16_t intervalMs);
    void enableControl(uint8_t addr, bool state, bool sync);
    void torqueControl(uint8_t addr, uint8_t dir, uint16_t accelMaPerSec, uint16_t currentMa, bool sync);
    void torqueControlWithSpeedLimit(uint8_t addr, uint8_t dir, uint16_t accelMaPerSec, uint16_t currentMa, bool sync, uint16_t maxSpeed);
    void velocityControl(uint8_t addr, uint8_t dir, uint16_t vel, uint16_t acc, bool sync);
    void velocityControlWithCurrentLimit(uint8_t addr, uint8_t dir, uint16_t acc, uint16_t vel, bool sync, uint16_t maxCurrentMa);
    void passthroughPositionControl(uint8_t addr, uint8_t dir, uint16_t vel, uint32_t clk, uint8_t motionMode, bool sync);
    void passthroughPositionControlWithCurrentLimit(uint8_t addr, uint8_t dir, uint16_t vel, uint32_t clk, uint8_t motionMode, bool sync, uint16_t maxCurrentMa);
    void positionControl(uint8_t addr, uint8_t dir, uint16_t vel, uint16_t accel, uint16_t decel, uint32_t clk, uint8_t motionMode, bool sync);
    void positionControlWithCurrentLimit(uint8_t addr, uint8_t dir, uint16_t vel, uint16_t accel, uint16_t decel, uint32_t clk, uint8_t motionMode, bool sync, uint16_t maxCurrentMa);
    void stopNow(uint8_t addr, bool sync);
    void synchronousMotion(uint8_t addr);
    void originSetZero(uint8_t addr, bool save);
    void originModifyParams(uint8_t addr, bool save, uint8_t mode, uint8_t dir, uint16_t vel, uint32_t timeoutMs, uint16_t stallVel, uint16_t stallMa, uint16_t stallMs, bool powerOnTrigger);
    void originTriggerReturn(uint8_t addr, uint8_t mode, bool sync);
    void originInterrupt(uint8_t addr);

private:
    bool sendCommand(
        const uint8_t *cmd,
        uint8_t len,
        bool recordError = true,
        bool singleShot = false);
    static uint16_t constrainCurrentMa(uint16_t value);

    int txPin_;
    int rxPin_;
    long canSpeed_;
    bool initialized_ = false;
    bool transmissionError_ = false;
};
