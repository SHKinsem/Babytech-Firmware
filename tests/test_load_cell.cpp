#include <assert.h>
#include <math.h>

#include "LoadCellProcessor.h"

using motion::LoadCellProcessingConfig;
using motion::LoadCellProcessor;
using motion::LoadCellSnapshot;
using motion::LoadCellStatus;

LoadCellProcessingConfig testConfig() {
    LoadCellProcessingConfig config;
    config.tareOffsetRaw = 1000;
    config.countsPerGram = 100.0f;
    config.filterDivisor = 1;
    config.stableSampleCount = 4;
    config.stableToleranceG = 0.2f;
    config.sampleTimeoutMs = 1000;
    return config;
}

void testStableWeight() {
    LoadCellProcessor processor;
    assert(processor.configure(testConfig(), 0));
    processor.ingestRaw(11000, 100);
    processor.ingestRaw(11005, 200);
    processor.ingestRaw(10995, 300);
    processor.ingestRaw(11000, 400);
    const LoadCellSnapshot& snapshot = processor.snapshot();
    assert(snapshot.status == LoadCellStatus::Stable);
    assert(snapshot.stable);
    assert(fabsf(snapshot.filteredWeightG - 100.0f) < 0.01f);
}

void testUnstableStaleAndFault() {
    LoadCellProcessor processor;
    const LoadCellProcessingConfig config = testConfig();
    assert(processor.configure(config, 0));
    processor.ingestRaw(11000, 100);
    processor.ingestRaw(12000, 200);
    processor.ingestRaw(10000, 300);
    processor.ingestRaw(13000, 400);
    assert(processor.snapshot().status == LoadCellStatus::Unstable);
    processor.poll(1401);
    assert(processor.snapshot().status == LoadCellStatus::Stale);
    processor.ingestRaw(config.rawMax, 1500);
    assert(processor.snapshot().status == LoadCellStatus::Fault);
}

void testCalibrationMathAndUncalibratedStatus() {
    LoadCellProcessor processor;
    LoadCellProcessingConfig config = testConfig();
    config.countsPerGram = 0.0f;
    assert(processor.configure(config, 0));
    processor.ingestRaw(2000, 100);
    assert(processor.snapshot().status == LoadCellStatus::Uncalibrated);

    float factor = 0.0f;
    assert(LoadCellProcessor::calculateCountsPerGram(1000, 51000, 500.0f, factor));
    assert(fabsf(factor - 100.0f) < 0.001f);
    assert(!LoadCellProcessor::calculateCountsPerGram(1000, 1000, 500.0f, factor));
    assert(!LoadCellProcessor::calculateCountsPerGram(1000, 51000, 0.0f, factor));
}

int main() {
    testStableWeight();
    testUnstableStaleAndFault();
    testCalibrationMathAndUncalibratedStatus();
    return 0;
}
