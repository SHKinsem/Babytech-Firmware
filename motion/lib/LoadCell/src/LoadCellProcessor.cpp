#include "LoadCellProcessor.h"

#include <math.h>

namespace motion {
namespace {

bool validScale(float value) {
    return isfinite(value) && fabsf(value) >= 0.001f;
}

bool validConfig(const LoadCellProcessingConfig& config) {
    return config.filterDivisor > 0 && config.stableSampleCount >= 2 &&
        config.stableSampleCount <= LoadCellProcessor::kMaxStableSamples &&
        isfinite(config.stableToleranceG) && config.stableToleranceG > 0.0f &&
        config.sampleTimeoutMs >= 100 && config.sampleTimeoutMs < 0x80000000UL &&
        config.rawMin < config.rawMax && isfinite(config.countsPerGram);
}

}  // namespace

bool LoadCellProcessor::configure(
    const LoadCellProcessingConfig& config, uint32_t nowMs) {
    if (!validConfig(config)) return false;
    config_ = config;
    enabled_ = true;
    resetSamples(nowMs);
    return true;
}

void LoadCellProcessor::disable() {
    enabled_ = false;
    snapshot_ = LoadCellSnapshot{};
}

void LoadCellProcessor::resetSamples(uint32_t nowMs) {
    snapshot_ = LoadCellSnapshot{};
    snapshot_.status = LoadCellStatus::WarmingUp;
    snapshot_.calibrated = validScale(config_.countsPerGram);
    stableWindowCount_ = 0;
    stableWindowIndex_ = 0;
    startedAtMs_ = nowMs;
}

void LoadCellProcessor::ingestRaw(int32_t raw, uint32_t nowMs) {
    if (!enabled_) return;
    snapshot_.raw = raw;
    snapshot_.netRaw = raw - config_.tareOffsetRaw;
    snapshot_.sampledAtMs = nowMs;
    snapshot_.hasSample = true;
    snapshot_.sampleCount++;
    if (raw <= config_.rawMin || raw >= config_.rawMax) {
        snapshot_.status = LoadCellStatus::Fault;
        snapshot_.stable = false;
        stableWindowCount_ = 0;
        return;
    }
    if (!validScale(config_.countsPerGram)) {
        snapshot_.status = LoadCellStatus::Uncalibrated;
        snapshot_.calibrated = false;
        snapshot_.stable = false;
        return;
    }

    snapshot_.calibrated = true;
    snapshot_.rawWeightG = static_cast<float>(snapshot_.netRaw) / config_.countsPerGram;
    if (snapshot_.sampleCount == 1) {
        snapshot_.filteredWeightG = snapshot_.rawWeightG;
    } else {
        snapshot_.filteredWeightG +=
            (snapshot_.rawWeightG - snapshot_.filteredWeightG) / config_.filterDivisor;
    }
    updateStability(snapshot_.filteredWeightG);
}

void LoadCellProcessor::updateStability(float value) {
    stableWindow_[stableWindowIndex_] = value;
    stableWindowIndex_ = (stableWindowIndex_ + 1) % config_.stableSampleCount;
    if (stableWindowCount_ < config_.stableSampleCount) ++stableWindowCount_;
    if (stableWindowCount_ < config_.stableSampleCount) {
        snapshot_.stable = false;
        snapshot_.status = LoadCellStatus::WarmingUp;
        return;
    }

    float minimum = stableWindow_[0];
    float maximum = stableWindow_[0];
    for (uint8_t i = 1; i < stableWindowCount_; ++i) {
        if (stableWindow_[i] < minimum) minimum = stableWindow_[i];
        if (stableWindow_[i] > maximum) maximum = stableWindow_[i];
    }
    snapshot_.stable = maximum - minimum <= config_.stableToleranceG;
    snapshot_.status = snapshot_.stable ? LoadCellStatus::Stable : LoadCellStatus::Unstable;
}

void LoadCellProcessor::poll(uint32_t nowMs) {
    if (!enabled_ || snapshot_.status == LoadCellStatus::Fault) return;
    const uint32_t referenceMs = snapshot_.hasSample ? snapshot_.sampledAtMs : startedAtMs_;
    if (static_cast<uint32_t>(nowMs - referenceMs) > config_.sampleTimeoutMs) {
        snapshot_.status = LoadCellStatus::Stale;
        snapshot_.stable = false;
    }
}

void LoadCellProcessor::setTareOffset(int32_t tareOffsetRaw, uint32_t nowMs) {
    config_.tareOffsetRaw = tareOffsetRaw;
    resetSamples(nowMs);
}

void LoadCellProcessor::setCountsPerGram(float countsPerGram, uint32_t nowMs) {
    config_.countsPerGram = countsPerGram;
    resetSamples(nowMs);
}

bool LoadCellProcessor::calculateCountsPerGram(
    int32_t tareRaw, int32_t loadedRaw, float knownWeightG, float& result) {
    if (!isfinite(knownWeightG) || knownWeightG <= 0.0f || loadedRaw == tareRaw) return false;
    const float candidate = static_cast<float>(loadedRaw - tareRaw) / knownWeightG;
    if (!validScale(candidate)) return false;
    result = candidate;
    return true;
}

const char* loadCellStatusName(LoadCellStatus status) {
    switch (status) {
        case LoadCellStatus::Disabled: return "disabled";
        case LoadCellStatus::WarmingUp: return "warming_up";
        case LoadCellStatus::Uncalibrated: return "uncalibrated";
        case LoadCellStatus::Unstable: return "unstable";
        case LoadCellStatus::Stable: return "stable";
        case LoadCellStatus::Stale: return "stale";
        case LoadCellStatus::Fault: return "fault";
    }
    return "unknown";
}

}  // namespace motion
