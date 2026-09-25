#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "display_model.h"

namespace motion {
using babytech::display::DisplayStage;
using babytech::display::DisplayError;

constexpr size_t kDemoMaxAxes = 5;
constexpr size_t kDemoMaxJsonBytes = 16384;

struct DemoAxis {
    uint8_t id = 0;
    int32_t tolerance = 10; // 0.1 degree
    double rotationMm = 0;
    bool zero = false;
};
struct DemoScript {
    uint32_t timeoutMs = 60000;
    std::vector<std::string> commands;
};
struct DemoConfig {
    std::string name = "unconfigured";
    std::string baby = "DEMO", brand = "NOT FOR FEEDING";
    uint16_t waterMl = 180;
    int16_t temperatureC = 45;
    std::vector<DemoAxis> axes;
    DemoScript initialization;
    std::array<DemoScript, 5> stages;
    bool configured = false;
};
struct DemoEvidence {
    bool fresh = false, stationary = false, fault = false;
    int32_t position = 0;
};
enum class DemoExecution { Running, Done, Failed };

// Hardware adapter owns queue transport. Controller never treats transmission
// or elapsed time as physical completion.
class DemoExecutor {
public:
    virtual ~DemoExecutor() = default;
    virtual bool healthy() const = 0;
    virtual bool available() const = 0;
    virtual bool configurationValid() const { return true; }
    virtual DemoEvidence evidence(uint8_t id) const = 0;
    virtual bool start(const DemoScript& script, bool initializing,
                       const std::array<int32_t, 256>& zeros, uint32_t now) = 0;
    virtual DemoExecution execution() const = 0;
    virtual DisplayError failureError() const { return DisplayError::Unknown; }
    virtual bool stop() = 0;
    virtual bool reset() = 0;
};

class DemoFlowController {
public:
    explicit DemoFlowController(DemoExecutor& executor) : executor_(executor) {}
    const DemoConfig& config() const { return config_; }
    bool busy() const { return running_ || stopping_ || resetPending_ || stage_ == DisplayStage::Complete; }
    bool referenceValid() const { return reference_; }
    bool initializing() const { return running_ && index_ == -1; }
    DisplayStage stage() const { return stage_; }
    DisplayError error() const { return stage_ == DisplayStage::Error ? error_ : DisplayError::None; }
    bool startEnabled() const { return stage_ == DisplayStage::Ready; }
    const char* reason() const { return reason_; }
    bool apply(DemoConfig config);
    bool initialize(uint32_t now);
    bool start(uint32_t now);
    bool single(uint8_t index, uint32_t now);
    void stop(uint32_t now);
    void invalidate();
    void tick(uint32_t now);
    babytech::display::DisplaySnapshot snapshot() const;
private:
    bool settled(bool zero) const;
    bool begin(int index, uint32_t now);
    void fail(DisplayError error, const char* reason, uint32_t now);
    DemoExecutor& executor_;
    DemoConfig config_;
    std::array<int32_t, 256> zeros_{};
    DisplayStage stage_ = DisplayStage::NotReady;
    DisplayError error_ = DisplayError::None;
    bool reference_ = false, running_ = false, stopping_ = false, full_ = false;
    bool launchPending_ = false;
    bool resetPending_ = false;
    int index_ = -1;
    uint32_t began_ = 0, stopAt_ = 0, completeAt_ = 0;
    const char* reason_ = "configuration_required";
};
} // namespace motion
