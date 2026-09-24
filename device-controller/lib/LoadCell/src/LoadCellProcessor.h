#pragma once

#include <stdint.h>

namespace motion {

enum class LoadCellStatus : uint8_t {
    Disabled,
    WarmingUp,
    Uncalibrated,
    Unstable,
    Stable,
    Stale,
    Fault,
};

struct LoadCellProcessingConfig {
    int32_t tareOffsetRaw = 0;
    float countsPerGram = 0.0f;
    uint8_t filterDivisor = 4;
    uint8_t stableSampleCount = 8;
    float stableToleranceG = 1.0f;
    uint32_t sampleTimeoutMs = 1500;
    int32_t rawMin = -8300000;
    int32_t rawMax = 8300000;
};

struct LoadCellSnapshot {
    LoadCellStatus status = LoadCellStatus::Disabled;
    int32_t raw = 0;
    int32_t netRaw = 0;
    float rawWeightG = 0.0f;
    float filteredWeightG = 0.0f;
    bool hasSample = false;
    bool calibrated = false;
    bool stable = false;
    uint32_t sampledAtMs = 0;
    uint32_t sampleCount = 0;
};

class LoadCellProcessor {
public:
    static constexpr uint8_t kMaxStableSamples = 16;

    bool configure(const LoadCellProcessingConfig& config, uint32_t nowMs);
    void disable();
    void ingestRaw(int32_t raw, uint32_t nowMs);
    void poll(uint32_t nowMs);
    void setTareOffset(int32_t tareOffsetRaw, uint32_t nowMs);
    void setCountsPerGram(float countsPerGram, uint32_t nowMs);

    const LoadCellSnapshot& snapshot() const { return snapshot_; }
    static bool calculateCountsPerGram(
        int32_t tareRaw, int32_t loadedRaw, float knownWeightG, float& result);

private:
    void resetSamples(uint32_t nowMs);
    void updateStability(float value);

    LoadCellProcessingConfig config_;
    LoadCellSnapshot snapshot_;
    float stableWindow_[kMaxStableSamples] = {};
    uint8_t stableWindowCount_ = 0;
    uint8_t stableWindowIndex_ = 0;
    uint32_t startedAtMs_ = 0;
    bool enabled_ = false;
};

const char* loadCellStatusName(LoadCellStatus status);

}  // namespace motion
