#include "ProductSession.h"

#include <cmath>
#include <cstring>

namespace motion {

namespace {

void copyUtf8Prefix(char* destination, size_t capacity, const std::string& source) {
    if (capacity == 0) return;
    size_t length = source.size() < capacity ? source.size() : capacity - 1;
    while (length < source.size() && length > 0 &&
           (static_cast<unsigned char>(source[length]) & 0xc0) == 0x80) {
        --length;
    }
    std::memcpy(destination, source.data(), length);
    destination[length] = '\0';
}

}  // namespace

void ProductSession::resources(bool waterValid, bool lowWater, bool powderValid, float powderGrams,
                               uint32_t now) {
    waterValid_ = waterValid;
    lowWater_ = lowWater;
    powderValid_ = powderValid && std::isfinite(powderGrams);
    powderGrams_ = powderValid_ ? powderGrams : 0.0f;
    if (active_ && !stopPending_ && (!waterValid_ || lowWater_ || !powderValid_)) {
        flow_.stop(now);
        if (flow_.error() == DisplayError::CanFault)
            finish(false, "stop_unconfirmed", "E_CAN_FAULT");
        else {
            stopPending_ = true;
            stopReason_ = !waterValid_ ? "water_sensor_invalid" :
                (lowWater_ ? "low_water" : "powder_sensor_invalid");
            stopErrorCode_ = !waterValid_ ? "E_WATER_SENSOR_INVALID" :
                (lowWater_ ? "E_LOW_WATER" : "E_POWDER_SENSOR_INVALID");
        }
    }
}

bool ProductSession::acceptsContext(const FeedingContext& context) const {
    if (context.profileVersion == 0 || context.babyId.empty() ||
        context.recipe.waterMl < 30 || context.recipe.waterMl > 500 ||
        context.recipe.temperatureC < 35 || context.recipe.temperatureC > 60 ||
        !std::isfinite(context.recipe.powderGPer100Ml) ||
        context.recipe.powderGPer100Ml < 1.0f || context.recipe.powderGPer100Ml > 50.0f ||
        context.profileVersion <= context_.profileVersion) return false;
    return true;
}

bool ProductSession::applyContext(const FeedingContext& context) {
    if (!acceptsContext(context)) return false;
    context_ = context;
    return true;
}

bool ProductSession::acceptsClear(uint32_t profileVersion) const {
    return profileVersion > context_.profileVersion;
}

bool ProductSession::clearContext(uint32_t profileVersion) {
    if (!acceptsClear(profileVersion)) return false;
    context_ = FeedingContext{};
    context_.profileVersion = profileVersion;
    return true;
}

bool ProductSession::canStart() const {
    return executionAuthorized_ && contextStorageReady_ && (!startGuard_ || startGuard_->ready()) &&
        !active_ && !cleanPending_ && !cleaning_ &&
        !eventPending_ &&
        waterValid_ && !lowWater_ && powderValid_ && powderGrams_ > 50.0f &&
        flow_.startEnabled() && !flow_.busy();
}

const char* ProductSession::prepareRejection(const ProductRun& run) const {
    if (!executionAuthorized_) return "non_consumable_demo_disabled";
    if (!contextStorageReady_) return "context_storage_fault";
    if (active_ || cleanPending_ || cleaning_ || eventPending_) return "busy";
    if (!waterValid_) return "water_sensor_invalid";
    if (lowWater_) return "low_water";
    if (!powderValid_) return "powder_sensor_invalid";
    if (powderGrams_ <= 50.0f) return "low_powder";
    if (run.recipe.waterMl < 30 || run.recipe.waterMl > 500) {
        return "invalid_water_ml";
    }
    if (run.recipe.temperatureC < 35 || run.recipe.temperatureC > 60) {
        return "invalid_temp";
    }
    if (!std::isfinite(run.recipe.powderGPer100Ml) ||
        run.recipe.powderGPer100Ml < 1.0f || run.recipe.powderGPer100Ml > 50.0f) {
        return "invalid_powder_g_per_100ml";
    }
    if (run.babyId.empty()) return "baby_context_missing";
    const float targetPowderG = std::round(run.recipe.waterMl * run.recipe.powderGPer100Ml / 10.0f) / 10.0f;
    if (powderGrams_ < targetPowderG + 50.0f) return "low_powder";
    if (!flow_.startEnabled() || flow_.busy()) return "not_ready";
    if (startGuard_ && !startGuard_->ready()) return "event_storage_fault";
    return nullptr;
}

bool ProductSession::start(ProductRun run, uint32_t now, const char*& rejection) {
    rejection = prepareRejection(run);
    if (rejection) return false;
    run.targetPowderG = std::round(run.recipe.waterMl * run.recipe.powderGPer100Ml / 10.0f) / 10.0f;
    if (startGuard_ && !startGuard_->prepare(run, now)) {
        rejection = "event_storage_fault"; return false;
    }
    activeRun_ = std::move(run);
    active_ = true;
    if (!flow_.start(now)) {
        finish(false, "not_ready", "E_MOTION_FAULT");
        rejection = "not_ready";
        return false;
    }
    rejection = nullptr;
    return true;
}

bool ProductSession::startCloud(ProductRun run, uint32_t now, const char*& rejection) {
    if (run.commandId.empty()) { rejection = "invalid_command_id"; return false; }
    run.source = "cloud_command";
    if (run.babyId.empty() && hasContext()) {
        run.babyId = context_.babyId;
        run.profileVersion = context_.profileVersion;
    }
    return start(std::move(run), now, rejection);
}

bool ProductSession::startLocal(const std::string& commandId, uint32_t now, const char*& rejection) {
    if (commandId.empty()) { rejection = "invalid_command_id"; return false; }
    if (!hasContext()) { rejection = "baby_context_missing"; return false; }
    ProductRun run;
    run.commandId = commandId;
    run.source = "local_touch";
    run.babyId = context_.babyId;
    run.profileVersion = context_.profileVersion;
    run.recipe = context_.recipe;
    return start(std::move(run), now, rejection);
}

bool ProductSession::startPaired(ProductRun run, uint32_t now, const char*& rejection) {
    if (run.commandId.empty()) { rejection = "invalid_command_id"; return false; }
    if (run.source != "cloud_command" && run.source != "local_touch") {
        rejection = "invalid_source"; return false;
    }
    return start(std::move(run), now, rejection);
}

void ProductSession::networkState(bool wifiConnected, uint32_t now) {
    // Local operation uses the saved recipe; Cloud commands retain their
    // existing Wi-Fi-loss stop policy. Neither path needs MQTT mid-run.
    if (active_ && !wifiConnected && activeRun_.source == "cloud_command")
        abort(now, "network_lost", "E_NETWORK_LOST");
}

void ProductSession::recoverAfterRestart(uint32_t now) {
    // The MCU can reset while separately powered drivers retain their command.
    // Use the normal broadcast stop and post-stop feedback supervision, never resume.
    flow_.invalidate();
    flow_.stop(now);
}

void ProductSession::finish(bool completed, const char* reason, const char* errorCode) {
    if (!active_) return;
    terminal_ = {activeRun_, completed, reason ? reason : "", errorCode ? errorCode : ""};
    terminalAvailable_ = true;
    eventPending_ = true;
    active_ = false;
    stopPending_ = false;
}

bool ProductSession::stop(uint32_t now, bool& wasActive) {
    wasActive = active_;
    if (active_) {
        if (stopPending_) return true;
        flow_.stop(now);
        if (flow_.error() == DisplayError::CanFault)
            finish(false, "stop_unconfirmed", "E_CAN_FAULT");
        else {
            stopPending_ = true;
            stopReason_ = "stopped";
            stopErrorCode_ = "E_STOPPED";
        }
        return flow_.error() != DisplayError::CanFault;
    }
    cleanPending_ = false;
    cleaning_ = false;
    if (flow_.busy() || flow_.referenceValid()) flow_.stop(now);
    return flow_.error() != DisplayError::CanFault;
}

bool ProductSession::abort(uint32_t now, const char* reason, const char* errorCode) {
    if (!active_) return false;
    if (stopPending_) return true;
    flow_.stop(now);
    if (flow_.error() == DisplayError::CanFault)
        finish(false, "stop_unconfirmed", "E_CAN_FAULT");
    else {
        stopPending_ = true;
        stopReason_ = reason ? reason : "aborted";
        stopErrorCode_ = errorCode ? errorCode : "E_STOPPED";
    }
    return true;
}

const char* ProductSession::cleanRejection() const {
    if (active_ || cleanPending_ || cleaning_ || eventPending_ || flow_.busy()) {
        return "busy";
    }
    if (flow_.stage() == DisplayStage::Error) return "error_state";
    return nullptr;
}

bool ProductSession::clean(uint32_t now, const char*& rejection) {
    rejection = cleanRejection();
    if (rejection) return false;
    flow_.stop(now);
    if (flow_.error() == DisplayError::CanFault) { rejection = "stop_unconfirmed"; return false; }
    cleanPending_ = true;
    rejection = nullptr;
    return true;
}

const char* ProductSession::initializeRejection() const {
    if (active_ || eventPending_ || cleanPending_ || cleaning_ || flow_.busy()) return "busy";
    return flow_.canInitialize() ? nullptr : "not_ready";
}

bool ProductSession::initialize(uint32_t now) {
    if (initializeRejection()) return false;
    return flow_.initialize(now);
}

void ProductSession::tick(uint32_t now) {
    (void)now;
    if (cleanPending_) {
        if (flow_.stage() == DisplayStage::Error) cleanPending_ = false;
        else if (!flow_.busy()) {
            cleanPending_ = false;
            cleaning_ = true;
        }
    }
    if (cleaning_ && flow_.stage() == DisplayStage::Error) cleaning_ = false;
    if (!active_) return;
    if (stopPending_) {
        if (flow_.stage() == DisplayStage::Error)
            finish(false, flow_.reason(), errorCode());
        else if (!flow_.busy())
            finish(false, stopReason_.c_str(), stopErrorCode_.c_str());
        return;
    }
    if (flow_.stage() == DisplayStage::Complete) finish(true, "", "");
    else if (flow_.stage() == DisplayStage::Error)
        finish(false, flow_.reason(), errorCode());
}

bool ProductSession::takeTerminal(ProductTerminal& terminal) {
    if (!terminalAvailable_) return false;
    terminal = terminal_;
    terminalAvailable_ = false;
    return true;
}

const char* ProductSession::progress() const {
    if (flow_.stage() == DisplayStage::Error) return "error";
    if (cleaning_) return "cleaning";
    if (cleanPending_) return "noready";
    if (active_) {
        switch (flow_.stage()) {
            case DisplayStage::UnscrewingCap: return "unscrewing_cap";
            case DisplayStage::DispensingWater: return "dispensing_water";
            case DisplayStage::DispensingPowder: return "dispensing_powder";
            case DisplayStage::ScrewingCap: return "screwing_cap";
            case DisplayStage::Mixing: return "mixing";
            default: return "noready";
        }
    }
    if (flow_.stage() == DisplayStage::Complete) return "complete";
    return canStart() ? "ready" : "noready";
}

const char* ProductSession::errorCode() const {
    if (flow_.stage() != DisplayStage::Error) return "NONE";
    switch (flow_.error()) {
        case DisplayError::CapUnscrewTimeout: return "E_CAP_UNSCREW_TIMEOUT";
        case DisplayError::WaterDispenseTimeout: return "E_WATER_DISPENSE_TIMEOUT";
        case DisplayError::PowderDispenseTimeout: return "E_POWDER_DISPENSE_TIMEOUT";
        case DisplayError::CapScrewTimeout: return "E_CAP_SCREW_TIMEOUT";
        case DisplayError::MixingTimeout: return "E_MIXING_TIMEOUT";
        case DisplayError::CanFault: return "E_CAN_FAULT";
        default: return "E_MOTION_FAULT";
    }
}

babytech::display::DisplaySnapshot ProductSession::displaySnapshot() const {
    auto snapshot = flow_.snapshot();
    snapshot.startEnabled = canStart() && hasContext();
    snapshot.waterMl = hasContext() ? context_.recipe.waterMl : 0;
    snapshot.temperatureC = hasContext() ? context_.recipe.temperatureC : targetTemp_;
    const std::string babyName = hasContext() ? context_.babyName : "";
    const std::string brand = hasContext() ? context_.formulaBrand : "";
    snapshot.babyName.fill(0);
    snapshot.formulaBrand.fill(0);
    copyUtf8Prefix(snapshot.babyName.data(), snapshot.babyName.size(), babyName);
    copyUtf8Prefix(snapshot.formulaBrand.data(), snapshot.formulaBrand.size(), brand);
    if ((cleanPending_ || cleaning_) && flow_.stage() != DisplayStage::Error)
        snapshot.stage = DisplayStage::NotReady;
    if (!canStart() && snapshot.stage == DisplayStage::Ready) snapshot.stage = DisplayStage::NotReady;
    return snapshot;
}

}  // namespace motion
