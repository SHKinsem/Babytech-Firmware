#include "DemoFlowController.h"
#include <cstring>
#include <utility>

namespace motion {
namespace {
constexpr DisplayStage stages[] = {DisplayStage::UnscrewingCap,
    DisplayStage::DispensingWater, DisplayStage::DispensingPowder,
    DisplayStage::ScrewingCap, DisplayStage::Mixing};
constexpr DisplayError timeouts[] = {DisplayError::CapUnscrewTimeout,
    DisplayError::WaterDispenseTimeout, DisplayError::PowderDispenseTimeout,
    DisplayError::CapScrewTimeout, DisplayError::MixingTimeout};
}
bool DemoFlowController::apply(DemoConfig config) {
    if (busy() || !executor_.available()) return false;
    config_ = std::move(config);
    reference_ = false;
    if (stage_ != DisplayStage::Error) stage_ = DisplayStage::NotReady;
    reason_ = config_.configured ? "initialization_required" : "configuration_required";
    return true;
}
bool DemoFlowController::settled(bool zero) const {
    if (!executor_.healthy() || !executor_.configurationValid() || config_.axes.empty()) return false;
    for (const auto& axis : config_.axes) {
        if (zero && !axis.zero) continue;
        const auto e = executor_.evidence(axis.id);
        if (!e.fresh || !e.stationary || e.fault) return false;
        const int64_t delta = int64_t(e.position) - zeros_[axis.id];
        if (zero && axis.zero && (delta > axis.tolerance || delta < -axis.tolerance)) return false;
    }
    return true;
}
bool DemoFlowController::begin(int index, uint32_t now) {
    index_ = index;
    began_ = now;
    launchPending_ = true;
    running_ = true;
    stage_ = index < 0 ? DisplayStage::NotReady : stages[index];
    error_ = DisplayError::None;
    reason_ = index < 0 ? "initializing" : "running";
    return true;
}
bool DemoFlowController::initialize(uint32_t now) {
    if (busy() || !config_.configured || !executor_.available()) return false;
    if (stage_ == DisplayStage::Error || !executor_.healthy()) {
        if (!executor_.healthy()) reference_ = false;
        resetPending_ = executor_.reset();
        stopAt_ = now;
        reason_ = resetPending_ ? "reset_wait_feedback" : "reset_failed";
        return resetPending_;
    }
    if (!executor_.healthy()) return false;
    full_ = false;
    if (reference_) {
        if (!settled(true)) { reason_ = "zero_or_feedback_required"; return false; }
        stage_ = DisplayStage::Ready;
        error_ = DisplayError::None;
        reason_ = "ready";
        return true;
    }
    return begin(-1, now);
}
bool DemoFlowController::start(uint32_t now) {
    tick(now); // revalidate Ready before accepting an intent
    if (stage_ != DisplayStage::Ready || busy() || !executor_.available()) return false;
    full_ = true;
    return begin(0, now);
}
bool DemoFlowController::single(uint8_t index, uint32_t now) {
    if (index >= 5 || busy() || stage_ == DisplayStage::Error || !reference_ ||
        !config_.configured || !executor_.available() || !executor_.healthy()) return false;
    full_ = false;
    return begin(index, now);
}
void DemoFlowController::invalidate() {
    reference_ = false;
    if (!busy() && stage_ != DisplayStage::Error) stage_ = DisplayStage::NotReady;
    reason_ = "reference_invalid";
}
void DemoFlowController::fail(DisplayError error, const char* reason, uint32_t now) {
    running_ = launchPending_ = false;
    stage_ = DisplayStage::Error;
    error_ = error;
    reason_ = reason;
    stopping_ = true;
    stopAt_ = now;
    if (!executor_.stop()) { error_ = DisplayError::CanFault; reference_ = false; }
}
void DemoFlowController::stop(uint32_t now) {
    const bool latched = stage_ == DisplayStage::Error;
    running_ = launchPending_ = false;
    resetPending_ = false;
    stopping_ = true;
    stopAt_ = now;
    if (!latched) { stage_ = DisplayStage::NotReady; error_ = DisplayError::None; }
    reason_ = "stop_requested";
    if (!executor_.stop()) {
        stage_ = DisplayStage::Error; error_ = DisplayError::CanFault;
        reference_ = false; reason_ = "stop_unconfirmed";
    }
}
void DemoFlowController::tick(uint32_t now) {
    if (resetPending_) {
        if (settled(false)) {
            resetPending_ = false;
            if (reference_) {
                stage_ = settled(true) ? DisplayStage::Ready : DisplayStage::NotReady;
                error_ = DisplayError::None; reason_ = "reset_checked";
            } else { full_ = false; begin(-1, now); }
        } else if (uint32_t(now - stopAt_) >= 3000) {
            resetPending_ = false; reference_ = false;
            stage_ = DisplayStage::Error; error_ = DisplayError::CanFault; reason_ = "reset_feedback_missing";
        }
        return;
    }
    if (stopping_) {
        // Adapter must return post-stop evidence, not the pre-stop sample.
        if (settled(false)) { stopping_ = false; reason_ = "stopped"; }
        else if (uint32_t(now - stopAt_) >= 3000) {
            stopping_ = false; reference_ = false; stage_ = DisplayStage::Error;
            error_ = executor_.healthy() ? DisplayError::Unknown : DisplayError::CanFault;
            reason_ = "stop_unconfirmed";
        }
        return;
    }
    if (running_) {
        if (!executor_.healthy()) {
            reference_ = false; fail(DisplayError::CanFault, "can_fault", now); return;
        }
        const DemoScript& script = index_ < 0 ? config_.initialization : config_.stages[index_];
        if (uint32_t(now - began_) >= script.timeoutMs) {
            fail(index_ < 0 ? DisplayError::Unknown : timeouts[index_], "stage_timeout", now); return;
        }
        if (launchPending_) {
            // A preceding stage may have sent an immediate enable/disable whose
            // driver confirmation is still pending. Keep the stage timeout
            // supervising this handoff instead of turning a transient busy
            // state into script_rejected.
            if (!executor_.available()) return;
            launchPending_ = false;
            if (!executor_.start(script, index_ < 0, zeros_, now)) fail(DisplayError::Unknown, "script_rejected", now);
            return;
        }
        const auto execution = executor_.execution();
        if (execution == DemoExecution::Failed) { fail(executor_.failureError(), "execution_failed", now); return; }
        if (execution != DemoExecution::Done) return;
        if (index_ == -1) {
            for (const auto& axis : config_.axes) if (axis.zero)
                zeros_[axis.id] = executor_.evidence(axis.id).position;
            reference_ = true; running_ = false; stage_ = DisplayStage::Ready; reason_ = "ready";
        } else if (full_ && index_ < 4) {
            begin(index_ + 1, now);
        } else if (full_) {
            running_ = false; stage_ = DisplayStage::Complete; completeAt_ = now; reason_ = "complete";
        } else {
            running_ = false; stage_ = DisplayStage::NotReady; reason_ = "single_stage_done";
        }
        return;
    }
    if (stage_ == DisplayStage::Ready || stage_ == DisplayStage::Complete) {
        if (!executor_.healthy()) { reference_ = false; fail(DisplayError::CanFault, "can_fault", now); return; }
        if (!reference_) {
            stage_ = DisplayStage::NotReady; reason_ = "zero_or_feedback_required"; return;
        }
        if (stage_ == DisplayStage::Complete && uint32_t(now - completeAt_) >= 3000) {
            stage_ = DisplayStage::Ready; reason_ = "ready";
        }
    }
}
babytech::display::DisplaySnapshot DemoFlowController::snapshot() const {
    babytech::display::DisplaySnapshot s;
    s.stage = stage_; s.error = error(); s.startEnabled = startEnabled();
    s.thermalSimulated = true; s.waterMl = config_.waterMl; s.temperatureC = config_.temperatureC;
    std::strncpy(s.babyName.data(), config_.baby.c_str(), s.babyName.size() - 1);
    std::strncpy(s.formulaBrand.data(), config_.brand.c_str(), s.formulaBrand.size() - 1);
    return s;
}
} // namespace motion
