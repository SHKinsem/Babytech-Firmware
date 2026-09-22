#pragma once

#include <Arduino.h>

#include "LoadCellProcessor.h"

namespace motion {

struct Hx711ScaleConfig {
    int8_t doutPin = -1;
    int8_t sckPin = -1;
    uint8_t startupDiscardSamples = 4;
    LoadCellProcessingConfig processing;
};

class Hx711Scale {
public:
    bool begin(const Hx711ScaleConfig& config, uint32_t nowMs);
    void end();
    void poll(uint32_t nowMs);
    bool startTare(uint8_t sampleCount = 16);
    bool calibrate(float knownWeightG, uint32_t nowMs);

    const LoadCellSnapshot& snapshot() const { return processor_.snapshot(); }
    bool initialized() const { return initialized_; }
    bool tareInProgress() const { return tareRemaining_ > 0; }
    bool tareCompleted() const { return tareCompleted_; }
    bool calibrationPersisted() const { return calibrationPersisted_; }
    int32_t tareOffsetRaw() const { return config_.processing.tareOffsetRaw; }
    float countsPerGram() const { return config_.processing.countsPerGram; }
    uint32_t sampleTimeoutMs() const { return config_.processing.sampleTimeoutMs; }

private:
    bool readRaw(int32_t& result);
    bool loadCalibration();
    bool saveCalibration();

    Hx711ScaleConfig config_;
    LoadCellProcessor processor_;
    bool initialized_ = false;
    uint8_t startupDiscardRemaining_ = 0;
    uint8_t tareRemaining_ = 0;
    uint8_t tareTargetSamples_ = 0;
    int64_t tareSum_ = 0;
    uint32_t tareStartedAtMs_ = 0;
    uint32_t tareTimeoutMs_ = 0;
    bool tareCompleted_ = false;
    bool calibrationPersisted_ = false;
};

}  // namespace motion
