#include "Hx711Scale.h"

#include <Preferences.h>
#include <math.h>

namespace motion {
namespace {

constexpr char kPreferencesNamespace[] = "scale-cfg";
constexpr char kSchemaKey[] = "schema";
constexpr char kTareKey[] = "tare";
constexpr char kFactorKey[] = "factor";
constexpr uint16_t kSchemaVersion = 1;

bool validPin(int8_t pin) {
    return (pin >= 0 && pin <= 21) || (pin >= 38 && pin <= 48);
}

}  // namespace

bool Hx711Scale::begin(const Hx711ScaleConfig& config, uint32_t nowMs) {
    end();
    tareRemaining_ = 0;
    tareCompleted_ = false;
    calibrationPersisted_ = false;
    config_ = config;
    if (!validPin(config_.doutPin) || !validPin(config_.sckPin) ||
        config_.doutPin == config_.sckPin || config_.startupDiscardSamples > 32) {
        return false;
    }
    loadCalibration();
    if (!processor_.configure(config_.processing, nowMs)) return false;
    pinMode(config_.doutPin, INPUT_PULLUP);
    pinMode(config_.sckPin, OUTPUT);
    digitalWrite(config_.sckPin, LOW);
    startupDiscardRemaining_ = config_.startupDiscardSamples;
    initialized_ = true;
    return true;
}

void Hx711Scale::end() {
    if (initialized_) {
        digitalWrite(config_.sckPin, LOW);
        pinMode(config_.sckPin, INPUT);
        pinMode(config_.doutPin, INPUT);
    }
    initialized_ = false;
    tareRemaining_ = 0;
    tareTargetSamples_ = 0;
    tareSum_ = 0;
}

void Hx711Scale::poll(uint32_t nowMs) {
    if (!initialized_) return;
    if (tareRemaining_ > 0 &&
        static_cast<uint32_t>(nowMs - tareStartedAtMs_) > tareTimeoutMs_) {
        tareRemaining_ = 0;
        tareTargetSamples_ = 0;
        tareSum_ = 0;
    }

    int32_t raw = 0;
    if (readRaw(raw)) {
        if (startupDiscardRemaining_ > 0) {
            --startupDiscardRemaining_;
        } else if (tareRemaining_ > 0) {
            tareSum_ += raw;
            --tareRemaining_;
            if (tareRemaining_ == 0) {
                config_.processing.tareOffsetRaw =
                    static_cast<int32_t>(tareSum_ / tareTargetSamples_);
                processor_.setTareOffset(config_.processing.tareOffsetRaw, nowMs);
                tareCompleted_ = true;
                calibrationPersisted_ = saveCalibration() &&
                    fabsf(config_.processing.countsPerGram) >= 0.001f;
            }
        } else {
            processor_.ingestRaw(raw, nowMs);
        }
    }
    processor_.poll(nowMs);
}

bool Hx711Scale::startTare(uint8_t sampleCount) {
    if (!initialized_ || sampleCount == 0 || tareRemaining_ > 0) return false;
    tareTargetSamples_ = sampleCount;
    tareRemaining_ = sampleCount;
    tareSum_ = 0;
    tareStartedAtMs_ = millis();
    const uint32_t sampleBudgetMs = static_cast<uint32_t>(sampleCount) * 250U;
    const uint32_t staleBudgetMs = config_.processing.sampleTimeoutMs * 2U;
    tareTimeoutMs_ = sampleBudgetMs > staleBudgetMs ? sampleBudgetMs : staleBudgetMs;
    tareCompleted_ = false;
    calibrationPersisted_ = false;
    return true;
}

bool Hx711Scale::calibrate(float knownWeightG, uint32_t nowMs) {
    const LoadCellSnapshot& current = processor_.snapshot();
    if (!initialized_ || tareRemaining_ > 0 || !tareCompleted_ ||
        !current.hasSample ||
        current.status == LoadCellStatus::Stale ||
        current.status == LoadCellStatus::Fault ||
        static_cast<uint32_t>(nowMs - current.sampledAtMs) > config_.processing.sampleTimeoutMs) {
        return false;
    }
    float factor = 0.0f;
    if (!LoadCellProcessor::calculateCountsPerGram(
            config_.processing.tareOffsetRaw, current.raw, knownWeightG, factor)) {
        return false;
    }
    const float previousFactor = config_.processing.countsPerGram;
    config_.processing.countsPerGram = factor;
    calibrationPersisted_ = saveCalibration();
    if (!calibrationPersisted_) {
        config_.processing.countsPerGram = previousFactor;
        return false;
    }
    processor_.setCountsPerGram(factor, nowMs);
    return true;
}

bool Hx711Scale::loadCalibration() {
    Preferences preferences;
    if (!preferences.begin(kPreferencesNamespace, true)) return false;
    const uint16_t schema = preferences.getUShort(kSchemaKey, 0);
    if (schema == kSchemaVersion) {
        const int32_t tare = preferences.getInt(kTareKey, config_.processing.tareOffsetRaw);
        const float factor = preferences.getFloat(kFactorKey, config_.processing.countsPerGram);
        if (tare > config_.processing.rawMin && tare < config_.processing.rawMax &&
            isfinite(factor)) {
            config_.processing.tareOffsetRaw = tare;
            config_.processing.countsPerGram = factor;
            calibrationPersisted_ = fabsf(factor) >= 0.001f;
        }
    }
    preferences.end();
    return schema == kSchemaVersion;
}

bool Hx711Scale::saveCalibration() {
    Preferences preferences;
    if (!preferences.begin(kPreferencesNamespace, false)) return false;
    const bool saved =
        preferences.putUShort(kSchemaKey, kSchemaVersion) == sizeof(uint16_t) &&
        preferences.putInt(kTareKey, config_.processing.tareOffsetRaw) == sizeof(int32_t) &&
        preferences.putFloat(kFactorKey, config_.processing.countsPerGram) == sizeof(float);
    preferences.end();
    return saved;
}

bool Hx711Scale::readRaw(int32_t& result) {
    if (digitalRead(config_.doutPin) != LOW) return false;
    uint32_t bits = 0;
    noInterrupts();
    for (uint8_t bit = 0; bit < 24; ++bit) {
        digitalWrite(config_.sckPin, HIGH);
        delayMicroseconds(1);
        bits = (bits << 1) | static_cast<uint32_t>(digitalRead(config_.doutPin));
        digitalWrite(config_.sckPin, LOW);
        delayMicroseconds(1);
    }
    // One extra pulse selects channel A, gain 128 for the next conversion.
    digitalWrite(config_.sckPin, HIGH);
    delayMicroseconds(1);
    digitalWrite(config_.sckPin, LOW);
    delayMicroseconds(1);
    interrupts();
    if ((bits & 0x00800000UL) != 0) bits |= 0xFF000000UL;
    result = static_cast<int32_t>(bits);
    return true;
}

}  // namespace motion
