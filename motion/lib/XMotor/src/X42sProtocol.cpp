#include "X42sProtocol.h"

#include "x42s_can_id.h"

namespace {

bool buildReadSysParamsCommand(
    uint8_t addr, X42sSysParam param, uint8_t* cmd, uint8_t& length) {
    if (cmd == nullptr) return false;
    length = 0;
    cmd[length++] = addr;

    switch (param) {
        case X42sSysParam::Ver: cmd[length++] = 0x1F; break;
        case X42sSysParam::Rl: cmd[length++] = 0x20; break;
        case X42sSysParam::Pid: cmd[length++] = 0x21; break;
        case X42sSysParam::Vbus: cmd[length++] = 0x24; break;
        case X42sSysParam::Cpha: cmd[length++] = 0x27; break;
        case X42sSysParam::Encl: cmd[length++] = 0x31; break;
        case X42sSysParam::Tpos: cmd[length++] = 0x33; break;
        case X42sSysParam::Vel: cmd[length++] = 0x35; break;
        case X42sSysParam::Cpos: cmd[length++] = 0x36; break;
        case X42sSysParam::Perr: cmd[length++] = 0x37; break;
        case X42sSysParam::Flag: cmd[length++] = 0x3A; break;
        case X42sSysParam::Org: cmd[length++] = 0x3B; break;
        case X42sSysParam::Conf:
            cmd[length++] = 0x42;
            cmd[length++] = 0x6C;
            break;
        case X42sSysParam::State:
            cmd[length++] = 0x43;
            cmd[length++] = 0x7A;
            break;
        default:
            return false;
    }
    cmd[length++] = 0x6B;
    return true;
}

}  // namespace

X42sProtocol::X42sProtocol(int txPin, int rxPin, long canSpeed)
    : txPin_(txPin), rxPin_(rxPin), canSpeed_(canSpeed) {}

bool X42sProtocol::configure(int txPin, int rxPin, long canSpeed) {
    if (initialized_) return false;
    txPin_ = txPin;
    rxPin_ = rxPin;
    canSpeed_ = canSpeed;
    return true;
}

bool X42sProtocol::begin() {
    // 1. 强制重置引脚状态
    pinMode(txPin_, OUTPUT);
    pinMode(rxPin_, INPUT_PULLUP);

    // 2. 基础配置
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
        (gpio_num_t)txPin_, 
        (gpio_num_t)rxPin_, 
        TWAI_MODE_NORMAL
    );
    g_config.tx_queue_len = 20;
    g_config.rx_queue_len = 20;

    twai_timing_config_t t_config;
    if (canSpeed_ == 1000000) {
        t_config = TWAI_TIMING_CONFIG_1MBITS();
    } else if (canSpeed_ == 500000) {
        t_config = TWAI_TIMING_CONFIG_500KBITS();
    } else {
        Serial.printf("Unsupported CAN speed: %ld\n", canSpeed_);
        return false;
    }

    // 4. 过滤器配置：接收所有帧
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    // 5. 先尝试卸载
    twai_stop();
    twai_driver_uninstall();
    delay(10);

    // 6. 安装驱动
    esp_err_t err = twai_driver_install(&g_config, &t_config, &f_config);
    if (err != ESP_OK) {
        Serial.print("twai_driver_install failed: ");
        Serial.println(err);
        return false;
    }

    // 7. 启动驱动
    err = twai_start();
    if (err != ESP_OK) {
        Serial.print("twai_start failed: ");
        Serial.println(err);
        twai_driver_uninstall();
        return false;
    }

    initialized_ = true;
    return true;
}



void X42sProtocol::end() {
    if (initialized_) {
        twai_stop();
        twai_driver_uninstall();
        initialized_ = false;
    }
}

bool X42sProtocol::hasTransmissionError() const {
    return transmissionError_;
}

void X42sProtocol::clearTransmissionError() {
    transmissionError_ = false;
}

bool X42sProtocol::getBusStatus(CanBusStatus& status) const {
    status = CanBusStatus{};
    if (!initialized_) return false;

    twai_status_info_t raw{};
    if (twai_get_status_info(&raw) != ESP_OK) return false;
    switch (raw.state) {
        case TWAI_STATE_STOPPED:
            status.state = CanControllerState::Stopped;
            break;
        case TWAI_STATE_RUNNING:
            status.state = CanControllerState::Running;
            break;
        case TWAI_STATE_BUS_OFF:
            status.state = CanControllerState::BusOff;
            break;
        case TWAI_STATE_RECOVERING:
            status.state = CanControllerState::Recovering;
            break;
    }
    status.txErrorCounter = raw.tx_error_counter;
    status.rxErrorCounter = raw.rx_error_counter;
    status.errorPassive =
        raw.tx_error_counter >= 128 || raw.rx_error_counter >= 128;
    status.txFailedCount = raw.tx_failed_count;
    status.rxMissedCount = raw.rx_missed_count;
    status.rxOverrunCount = raw.rx_overrun_count;
    status.busErrorCount = raw.bus_error_count;
    return true;
}

bool X42sProtocol::receive(CanRawFrame& frame, uint32_t timeout_ms) {
    if (!initialized_) return false;
    twai_message_t msg{};
    if (twai_receive(&msg, pdMS_TO_TICKS(timeout_ms)) == ESP_OK) {
        frame = CanRawFrame{};
        frame.identifier = msg.identifier;
        frame.length = msg.data_length_code;
        frame.extended = msg.extd != 0;
        frame.remote = msg.rtr != 0;
        memcpy(frame.data, msg.data, msg.data_length_code);
        return true;
    }
    return false;
}

bool X42sProtocol::receive(
    uint32_t *id, uint8_t *data, uint8_t *len, uint32_t timeout_ms) {
    CanRawFrame frame;
    if (!receive(frame, timeout_ms)) return false;
    if (id) *id = frame.identifier;
    if (len) *len = frame.length;
    if (data) memcpy(data, frame.data, frame.length);
    return true;
}

uint16_t X42sProtocol::constrainCurrentMa(uint16_t value) {
    return static_cast<uint16_t>(constrain(static_cast<unsigned long>(value), 0UL, 5000UL));
}

// 核心改动点：只替换底层发送函数，分包逻辑与原始协议完全一致
bool X42sProtocol::sendCommand(
    const uint8_t *cmd, uint8_t len, bool recordError, bool singleShot) {
    if (!initialized_ || !cmd || len < 3) {
        if (recordError) transmissionError_ = true;
        return false;
    }

    uint8_t i = 0;
    const uint8_t payloadLen = static_cast<uint8_t>(len - 2);
    uint8_t packNum = 0;
    bool queued = true;

    while (i < payloadLen) {
        const uint8_t remain = payloadLen - i;

        twai_message_t frame{};
        frame.extd = 1; // 对应原协议的 frame.extended = true
        frame.ss = singleShot ? 1 : 0;
        frame.identifier = x42sCanFrameId(cmd[0], packNum);
        frame.data[0] = cmd[1];

        if (remain < 8) {
            uint8_t l = 0;
            for (; l < remain; ++l, ++i) {
                frame.data[l + 1] = cmd[i + 2];
            }
            frame.data_length_code = l + 1;
        } else {
            uint8_t l = 0;
            for (; l < 7; ++l, ++i) {
                frame.data[l + 1] = cmd[i + 2];
            }
            frame.data_length_code = 8;
        }

        // 对应原协议的 bus_->sendFrame(frame)
        
        esp_err_t err = twai_transmit(&frame, pdMS_TO_TICKS(50));
        if (err == ESP_OK && traceSink_) {
            CanRawFrame observed;
            observed.identifier = frame.identifier;
            observed.extended = true;
            observed.length = frame.data_length_code;
            memcpy(observed.data, frame.data, observed.length);
            traceSink_(traceContext_, observed, true);
        }
        if (err != ESP_OK) {
            queued = false;
            if (recordError) transmissionError_ = true;
            Serial.print("TWAI TX ERROR: ");
            Serial.println(err);
        }
        
        ++packNum;
        delay(2); // 延时加大到 2ms，给电机一点处理缓冲

    }
    return queued;
}

void X42sProtocol::resetCurPosToZero(uint8_t addr) {
    uint8_t cmd[] = {addr, 0x0A, 0x6D, 0x6B};
    sendCommand(cmd, sizeof(cmd));
}

void X42sProtocol::resetClogProtection(uint8_t addr) {
    uint8_t cmd[] = {addr, 0x0E, 0x52, 0x6B};
    sendCommand(cmd, sizeof(cmd));
}

bool X42sProtocol::readSysParams(uint8_t addr, X42sSysParam param) {
    uint8_t cmd[8] = {0};
    uint8_t length = 0;
    if (!buildReadSysParamsCommand(addr, param, cmd, length)) return false;
    return sendCommand(cmd, length);
}

bool X42sProtocol::probeReadSysParams(uint8_t addr, X42sSysParam param) {
    uint8_t cmd[8] = {0};
    uint8_t length = 0;
    if (!buildReadSysParamsCommand(addr, param, cmd, length)) return false;
    return sendCommand(cmd, length, false, true);
}

void X42sProtocol::modifyCtrlMode(uint8_t addr, bool save, uint8_t ctrlMode) {
    uint8_t cmd[] = {addr, 0x46, 0x69, static_cast<uint8_t>(save), ctrlMode, 0x6B};
    sendCommand(cmd, sizeof(cmd));
}

void X42sProtocol::configureRealtimePositionFeedback(uint8_t addr, uint16_t intervalMs) {
    uint8_t cmd[] = {
        addr,
        0x11,
        0x18,
        0x36,
        static_cast<uint8_t>((intervalMs >> 8) & 0xFF),
        static_cast<uint8_t>(intervalMs & 0xFF),
        0x6B,
    };
    sendCommand(cmd, sizeof(cmd));
}

void X42sProtocol::enableControl(uint8_t addr, bool state, bool sync) {
    uint8_t cmd[] = {addr, 0xF3, 0xAB, static_cast<uint8_t>(state), static_cast<uint8_t>(sync), 0x6B};
    sendCommand(cmd, sizeof(cmd));
}

void X42sProtocol::torqueControl(uint8_t addr, uint8_t dir, uint16_t accelMaPerSec, uint16_t currentMa, bool sync) {
    const uint16_t current = constrainCurrentMa(currentMa);
    uint8_t cmd[] = {
        addr,
        0xF5,
        dir,
        static_cast<uint8_t>((accelMaPerSec >> 8) & 0xFF),
        static_cast<uint8_t>(accelMaPerSec & 0xFF),
        static_cast<uint8_t>((current >> 8) & 0xFF),
        static_cast<uint8_t>(current & 0xFF),
        static_cast<uint8_t>(sync),
        0x6B,
    };
    sendCommand(cmd, sizeof(cmd));
}

void X42sProtocol::torqueControlWithSpeedLimit(uint8_t addr, uint8_t dir, uint16_t accelMaPerSec, uint16_t currentMa, bool sync, uint16_t maxSpeed) {
    const uint16_t current = constrainCurrentMa(currentMa);
    uint8_t cmd[] = {
        addr,
        0xC5,
        dir,
        static_cast<uint8_t>((accelMaPerSec >> 8) & 0xFF),
        static_cast<uint8_t>(accelMaPerSec & 0xFF),
        static_cast<uint8_t>((current >> 8) & 0xFF),
        static_cast<uint8_t>(current & 0xFF),
        static_cast<uint8_t>(sync),
        static_cast<uint8_t>((maxSpeed >> 8) & 0xFF),
        static_cast<uint8_t>(maxSpeed & 0xFF),
        0x6B,
    };
    sendCommand(cmd, sizeof(cmd));
}

void X42sProtocol::velocityControl(uint8_t addr, uint8_t dir, uint16_t vel, uint16_t acc, bool sync) {
    uint8_t cmd[] = {
        addr,
        0xF6,
        dir,
        static_cast<uint8_t>((acc >> 8) & 0xFF),
        static_cast<uint8_t>(acc & 0xFF),
        static_cast<uint8_t>((vel >> 8) & 0xFF),
        static_cast<uint8_t>(vel & 0xFF),
        static_cast<uint8_t>(sync),
        0x6B,
    };
    sendCommand(cmd, sizeof(cmd));
}

void X42sProtocol::velocityControlWithCurrentLimit(uint8_t addr, uint8_t dir, uint16_t acc, uint16_t vel, bool sync, uint16_t maxCurrentMa) {
    const uint16_t current = constrainCurrentMa(maxCurrentMa);
    uint8_t cmd[] = {
        addr,
        0xC6,
        dir,
        static_cast<uint8_t>((acc >> 8) & 0xFF),
        static_cast<uint8_t>(acc & 0xFF),
        static_cast<uint8_t>((vel >> 8) & 0xFF),
        static_cast<uint8_t>(vel & 0xFF),
        static_cast<uint8_t>(sync),
        static_cast<uint8_t>((current >> 8) & 0xFF),
        static_cast<uint8_t>(current & 0xFF),
        0x6B,
    };
    sendCommand(cmd, sizeof(cmd));
}

void X42sProtocol::passthroughPositionControl(uint8_t addr, uint8_t dir, uint16_t vel, uint32_t clk, uint8_t motionMode, bool sync) {
    uint8_t cmd[] = {
        addr,
        0xFB,
        dir,
        static_cast<uint8_t>((vel >> 8) & 0xFF),
        static_cast<uint8_t>(vel & 0xFF),
        static_cast<uint8_t>((clk >> 24) & 0xFF),
        static_cast<uint8_t>((clk >> 16) & 0xFF),
        static_cast<uint8_t>((clk >> 8) & 0xFF),
        static_cast<uint8_t>(clk & 0xFF),
        motionMode,
        static_cast<uint8_t>(sync),
        0x6B,
    };
    sendCommand(cmd, sizeof(cmd));
}

void X42sProtocol::passthroughPositionControlWithCurrentLimit(uint8_t addr, uint8_t dir, uint16_t vel, uint32_t clk, uint8_t motionMode, bool sync, uint16_t maxCurrentMa) {
    const uint16_t current = constrainCurrentMa(maxCurrentMa);
    uint8_t cmd[] = {
        addr,
        0xCB,
        dir,
        static_cast<uint8_t>((vel >> 8) & 0xFF),
        static_cast<uint8_t>(vel & 0xFF),
        static_cast<uint8_t>((clk >> 24) & 0xFF),
        static_cast<uint8_t>((clk >> 16) & 0xFF),
        static_cast<uint8_t>((clk >> 8) & 0xFF),
        static_cast<uint8_t>(clk & 0xFF),
        motionMode,
        static_cast<uint8_t>(sync),
        static_cast<uint8_t>((current >> 8) & 0xFF),
        static_cast<uint8_t>(current & 0xFF),
        0x6B,
    };
    sendCommand(cmd, sizeof(cmd));
}

void X42sProtocol::positionControl(uint8_t addr, uint8_t dir, uint16_t vel, uint16_t accel, uint16_t decel, uint32_t clk, uint8_t motionMode, bool sync) {
    const uint16_t accelWord = static_cast<uint16_t>(constrain(static_cast<unsigned long>(accel), 0UL, 65535UL));
    const uint16_t decelWord = static_cast<uint16_t>(constrain(static_cast<unsigned long>(decel), 0UL, 65535UL));
    uint8_t cmd[] = {
        addr,
        0xFD,
        dir,
        static_cast<uint8_t>((accelWord >> 8) & 0xFF),
        static_cast<uint8_t>(accelWord & 0xFF),
        static_cast<uint8_t>((decelWord >> 8) & 0xFF),
        static_cast<uint8_t>(decelWord & 0xFF),
        static_cast<uint8_t>((vel >> 8) & 0xFF),
        static_cast<uint8_t>(vel & 0xFF),
        static_cast<uint8_t>((clk >> 24) & 0xFF),
        static_cast<uint8_t>((clk >> 16) & 0xFF),
        static_cast<uint8_t>((clk >> 8) & 0xFF),
        static_cast<uint8_t>(clk & 0xFF),
        motionMode,
        static_cast<uint8_t>(sync),
        0x6B,
    };
    sendCommand(cmd, sizeof(cmd));
}

void X42sProtocol::positionControlWithCurrentLimit(uint8_t addr, uint8_t dir, uint16_t vel, uint16_t accel, uint16_t decel, uint32_t clk, uint8_t motionMode, bool sync, uint16_t maxCurrentMa) {
    const uint16_t accelWord = static_cast<uint16_t>(constrain(static_cast<unsigned long>(accel), 0UL, 65535UL));
    const uint16_t decelWord = static_cast<uint16_t>(constrain(static_cast<unsigned long>(decel), 0UL, 65535UL));
    const uint16_t current = constrainCurrentMa(maxCurrentMa);
    uint8_t cmd[] = {
        addr,
        0xCD,
        dir,
        static_cast<uint8_t>((accelWord >> 8) & 0xFF),
        static_cast<uint8_t>(accelWord & 0xFF),
        static_cast<uint8_t>((decelWord >> 8) & 0xFF),
        static_cast<uint8_t>(decelWord & 0xFF),
        static_cast<uint8_t>((vel >> 8) & 0xFF),
        static_cast<uint8_t>(vel & 0xFF),
        static_cast<uint8_t>((clk >> 24) & 0xFF),
        static_cast<uint8_t>((clk >> 16) & 0xFF),
        static_cast<uint8_t>((clk >> 8) & 0xFF),
        static_cast<uint8_t>(clk & 0xFF),
        motionMode,
        static_cast<uint8_t>(sync),
        static_cast<uint8_t>((current >> 8) & 0xFF),
        static_cast<uint8_t>(current & 0xFF),
        0x6B,
    };
    sendCommand(cmd, sizeof(cmd));
}

void X42sProtocol::stopNow(uint8_t addr, bool sync) {
    uint8_t cmd[] = {addr, 0xFE, 0x98, static_cast<uint8_t>(sync), 0x6B};
    sendCommand(cmd, sizeof(cmd));
}

void X42sProtocol::synchronousMotion(uint8_t addr) {
    uint8_t cmd[] = {addr, 0xFF, 0x66, 0x6B};
    sendCommand(cmd, sizeof(cmd));
}

void X42sProtocol::originSetZero(uint8_t addr, bool save) {
    uint8_t cmd[] = {addr, 0x93, 0x88, static_cast<uint8_t>(save), 0x6B};
    sendCommand(cmd, sizeof(cmd));
}

void X42sProtocol::originModifyParams(uint8_t addr, bool save, uint8_t mode, uint8_t dir, uint16_t vel, uint32_t timeoutMs, uint16_t stallVel, uint16_t stallMa, uint16_t stallMs, bool powerOnTrigger) {
    uint8_t cmd[] = {
        addr,
        0x4C,
        0xAE,
        static_cast<uint8_t>(save),
        mode,
        dir,
        static_cast<uint8_t>((vel >> 8) & 0xFF),
        static_cast<uint8_t>(vel & 0xFF),
        static_cast<uint8_t>((timeoutMs >> 24) & 0xFF),
        static_cast<uint8_t>((timeoutMs >> 16) & 0xFF),
        static_cast<uint8_t>((timeoutMs >> 8) & 0xFF),
        static_cast<uint8_t>(timeoutMs & 0xFF),
        static_cast<uint8_t>((stallVel >> 8) & 0xFF),
        static_cast<uint8_t>(stallVel & 0xFF),
        static_cast<uint8_t>((stallMa >> 8) & 0xFF),
        static_cast<uint8_t>(stallMa & 0xFF),
        static_cast<uint8_t>((stallMs >> 8) & 0xFF),
        static_cast<uint8_t>(stallMs & 0xFF),
        static_cast<uint8_t>(powerOnTrigger),
        0x6B,
    };
    sendCommand(cmd, sizeof(cmd));
}

void X42sProtocol::originTriggerReturn(uint8_t addr, uint8_t mode, bool sync) {
    uint8_t cmd[] = {addr, 0x9A, mode, static_cast<uint8_t>(sync), 0x6B};
    sendCommand(cmd, sizeof(cmd));
}

void X42sProtocol::originInterrupt(uint8_t addr) {
    uint8_t cmd[] = {addr, 0x9C, 0x48, 0x6B};
    sendCommand(cmd, sizeof(cmd));
}

// Raw logical command: same packet layout as sendCommand, but a failed packet
// ends the command instead of continuing with a partial one.
bool X42sProtocol::sendRawLogical(const uint8_t* bytes, uint8_t length) {
    if (!initialized_ || !bytes || length < 3 || length > 30) {
        transmissionError_ = true;
        return false;
    }
    const uint8_t payloadLen = static_cast<uint8_t>(length - 2);
    uint8_t offset = 0, packet = 0;
    while (offset < payloadLen) {
        twai_message_t frame{};
        frame.extd = 1;
        frame.ss = 1;  // single shot: no automatic retransmission
        frame.identifier = x42sCanFrameId(bytes[0], packet);
        frame.data[0] = bytes[1];
        const uint8_t remain = static_cast<uint8_t>(payloadLen - offset);
        const uint8_t take = remain < 7 ? remain : 7;
        uint8_t i = 0;
        for (; i < take; ++i, ++offset) frame.data[i + 1] = bytes[offset + 2];
        frame.data_length_code = i + 1;
        const esp_err_t err = twai_transmit(&frame, pdMS_TO_TICKS(50));
        if (err != ESP_OK) {
            transmissionError_ = true;
            return false;  // never send the later parts of a partial command
        }
        if (traceSink_) {
            CanRawFrame observed;
            observed.identifier = frame.identifier;
            observed.extended = true;
            observed.length = frame.data_length_code;
            memcpy(observed.data, frame.data, observed.length);
            traceSink_(traceContext_, observed, true);
        }
        ++packet;
        // Same bounded inter-packet gap sendCommand uses: the motor needs a
        // moment between the packets of one multi-packet command.
        delay(2);
    }
    return true;
}

bool X42sProtocol::sendRawFrame(uint32_t id, bool extended, const uint8_t* data, uint8_t length) {
    if (!initialized_ || length > 8 || (length > 0 && data == nullptr) ||
        (extended ? id > 0x1FFFFFFFu : id > 0x7FFu)) {
        transmissionError_ = true;
        return false;
    }
    twai_message_t frame{};
    frame.extd = extended ? 1 : 0;
    frame.ss = 1;  // single shot: no automatic retransmission
    frame.identifier = id;
    frame.data_length_code = length;
    for (uint8_t i = 0; i < length; ++i) frame.data[i] = data[i];
    if (twai_transmit(&frame, pdMS_TO_TICKS(50)) != ESP_OK) {
        transmissionError_ = true;
        return false;
    }
    if (traceSink_) {
        CanRawFrame observed;
        observed.identifier = id;
        observed.extended = extended;
        observed.length = length;
        memcpy(observed.data, frame.data, length);
        traceSink_(traceContext_, observed, true);
    }
    return true;
}
