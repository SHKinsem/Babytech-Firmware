#pragma once
#include "DemoFlowConfig.h"
#include "CommandQueue.h"
#include <memory>

namespace motion {
class DemoMotorExecutor final : public DemoExecutor {
public:
    DemoMotorExecutor(MotorControl& motor, CommandQueue& queue, const QueueRotationSource& rotation,
                      bool (*externalAvailable)() = nullptr)
        : motor_(motor), queue_(queue), rotation_(rotation), externalAvailable_(externalAvailable) {}
    // Axis monitoring is also needed when boot rejects executable parameters.
    void configureStopAxes(const DemoConfig& config) {
        for (unsigned id = 1; id < 256; ++id) motor_.demoWatch(static_cast<uint8_t>(id), false);
        stopAxisCount_ = 0;
        if (config.axes.size() <= stopAxes_.size()) for (const auto& axis : config.axes) {
            stopAxes_[stopAxisCount_++] = axis.id;
            motor_.demoWatch(axis.id, true);
        }
        postStop_ = false; probe_ = 0;
    }
    void configure(const DemoConfig& config) {
        motor_.queries().release(CanQueryScheduler::Demo);
        configureStopAxes(config);
        config_ = &config; armed_.fill(false);
    }
    void poll(uint32_t now) {
        if (!stopAxisCount_ || uint32_t(now - probeAt_) < 20) return;
        probeAt_ = now;
        const auto count = stopAxisCount_;
        motor_.demoProbe(stopAxes_[probe_ % count], (probe_ / count) % 4);
        probe_ = (probe_ + 1) % (count * 4);
        if (config_ && marking_ && queue_.state() == QueueState::Done) {
            while (markIndex_ < count && !config_->axes[markIndex_].zero) ++markIndex_;
            if (markIndex_ >= count) { marking_ = false; return; }
            const auto id = config_->axes[markIndex_].id;
            if (!markSent_) {
                // X manual p77: volatile 0x50 marker resets on driver reboot.
                const uint8_t command[] = {id, 0x50, 1, 0x6B};
                if (!motor_.queueSendLogical(command, sizeof(command))) { markerFailed_ = true; marking_ = false; return; }
                markAt_ = now; markSent_ = true;
            } else {
                uint8_t flags; uint32_t age;
                if (motor_.demoFlags(id, flags, age) && age < uint32_t(now - markAt_) && (flags & 0x80)) {
                    armed_[id] = true; ++markIndex_; markSent_ = false;
                }
            }
        }
    }
    bool healthy() const override {
        if (!motor_.ready() || motor_.hasFault()) return false;
        if (config_) for (const auto& a : config_->axes)
            if (motor_.demoDriverFault(a.id) || driverRestarted(a.id)) return false;
        return !markerFailed_;
    }
    bool available() const override {
        return !queue_.active() && !motor_.operationBusy() &&
            (!externalAvailable_ || externalAvailable_());
    }
    bool configurationValid() const override {
        return !config_ || demoRotationMatches(*config_, rotation_);
    }
    DemoEvidence evidence(uint8_t id) const override {
        return observation(id, 600);
    }
    DemoEvidence stopEvidence(uint8_t id) const override {
        return observation(id, stopWindowMs_);
    }
private:
    DemoEvidence observation(uint8_t id, uint16_t windowMs) const {
        const auto s = motor_.snapshot(id);
        DemoEvidence e;
        if (postStop_) e.fresh = motor_.stopEvidence(id, stopAt_, windowMs);
        else {
            uint8_t flags; uint32_t age;
            e.fresh = s.positionValid && s.velocityValid && motor_.demoFlags(id, flags, age);
        }
        e.stationary = s.velocity >= -5 && s.velocity <= 5;
        e.position = s.position; e.fault = motor_.demoDriverFault(id);
        return e;
    }
public:
    // Stop confirmation is not Ready: enabled holding drivers, invalid zeros,
    // script/rotation mismatch and latched faults do not erase fresh stop proof.
    bool stopConfirmed() const {
        if (!postStop_ || !motor_.ready() || !stopAxisCount_ ||
            queue_.active() || motor_.operationBusy()) return false;
        for (size_t i = 0; i < stopAxisCount_; ++i) {
            if (!motor_.stopEvidence(stopAxes_[i], stopAt_, stopWindowMs_)) return false;
        }
        return true;
    }
    bool start(const DemoScript& script, bool initializing,
               const std::array<int32_t, 256>& zeros, uint32_t now) override {
        if (!config_ || !healthy() || !demoRotationMatches(*config_, rotation_)) return false;
        std::unique_ptr<QueueProgram> program(new QueueProgram);
        std::string error;
        if (!buildDemoProgram(script, *config_, initializing, zeros, *program, error)) return false;
        postStop_ = false;
        const bool accepted = queue_.startDemo(*program, now).code < 300;
        if (accepted && initializing) {
            armed_.fill(false); marking_ = true; markIndex_ = 0; markSent_ = false;
        }
        return accepted;
    }
    DemoExecution execution() const override {
        if (markerFailed_) return DemoExecution::Failed;
        return queue_.active() || (marking_ && queue_.state() == QueueState::Done) ? DemoExecution::Running :
            queue_.state() == QueueState::Done ? DemoExecution::Done : DemoExecution::Failed;
    }
    DisplayError failureError() const override {
        const char* reason = queue_.message();
        return !healthy() || std::strcmp(reason, "tx_failed") == 0 ||
            std::strcmp(reason, "driver_rejected") == 0 || std::strcmp(reason, "driver_command_error") == 0 ||
            std::strcmp(reason, "home_failed") == 0 ? DisplayError::CanFault : DisplayError::Unknown;
    }
    bool stop() override {
        marking_ = false;
        stopAt_ = millis(); postStop_ = true;
        stopWindowMs_ = motor_.stopEvidenceWindow();
        return queue_.cancel("demo_stop").code < 300;
    }
    bool reset() override {
        if (config_) for (const auto& axis : config_->axes)
            if (driverRestarted(axis.id)) armed_[axis.id] = false;
        const bool sent = queue_.clearControlState().code < 300;
        stopAt_ = millis(); postStop_ = true; markerFailed_ = false; marking_ = false;
        stopWindowMs_ = motor_.stopEvidenceWindow();
        return sent;
    }
private:
    bool driverRestarted(uint8_t id) const {
        uint8_t flags; uint32_t age;
        return armed_[id] && motor_.demoFlags(id, flags, age) && !(flags & 0x80);
    }
    MotorControl& motor_;
    CommandQueue& queue_;
    const QueueRotationSource& rotation_;
    bool (*externalAvailable_)() = nullptr;
    const DemoConfig* config_ = nullptr;
    std::array<uint8_t, kDemoMaxAxes> stopAxes_{};
    size_t stopAxisCount_ = 0;
    size_t probe_ = 0;
    uint32_t probeAt_ = 0, stopAt_ = 0;
    uint16_t stopWindowMs_ = 600;
    bool postStop_ = false;
    std::array<bool, 256> armed_{};
    bool marking_ = false, markSent_ = false, markerFailed_ = false;
    size_t markIndex_ = 0;
    uint32_t markAt_ = 0;
};
}
