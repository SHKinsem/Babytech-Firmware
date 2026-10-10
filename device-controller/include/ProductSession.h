#pragma once

#include <cstdint>
#include <string>
#include "DemoFlowController.h"

namespace motion {

struct ProductRecipe {
    int waterMl = 180;
    int temperatureC = 45;
    float powderGPer100Ml = 0.0f;
};

struct FeedingContext {
    std::string babyId;
    std::string babyName;
    std::string formulaBrand;
    ProductRecipe recipe;
    uint32_t profileVersion = 0;
};

struct ProductRun {
    std::string eventId;
    std::string commandId;
    std::string source;
    std::string babyId;
    uint32_t profileVersion = 0;
    ProductRecipe recipe;
    float targetPowderG = 0.0f;
};

struct ProductTerminal {
    ProductRun run;
    bool completed = false;
    std::string reason;
    std::string errorCode;
};

class ProductStartGuard {
public:
    virtual ~ProductStartGuard() = default;
    virtual bool ready() const = 0;
    virtual bool prepare(ProductRun& run, uint32_t now) = 0;
};

// Product state and authorization around the existing supervised motion flow.
// It never issues CAN directly and does not decide MQTT delivery or persistence.
class ProductSession {
public:
    explicit ProductSession(DemoFlowController& flow) : flow_(flow) {}
    void resources(bool waterValid, bool lowWater, bool powderValid, float powderGrams, uint32_t now);
    bool acceptsContext(const FeedingContext& context) const;
    bool applyContext(const FeedingContext& context);
    bool acceptsClear(uint32_t profileVersion) const;
    bool clearContext(uint32_t profileVersion);
    const FeedingContext& context() const { return context_; }
    bool hasContext() const { return !context_.babyId.empty(); }
    void setContextStorageReady(bool ready) { contextStorageReady_ = ready; }
    bool contextStorageReady() const { return contextStorageReady_; }
    void setExecutionAuthorized(bool authorized) { executionAuthorized_ = authorized; }
    bool executionAuthorized() const { return executionAuthorized_; }
    bool startCloud(ProductRun run, uint32_t now, const char*& rejection);
    bool startLocal(const std::string& commandId, uint32_t now, const char*& rejection);
    bool startPaired(ProductRun run, uint32_t now, const char*& rejection);
    // Read-only admission checks; persistence and motion remain with the caller/actions.
    const char* prepareRejection(const ProductRun& run) const;
    const char* cleanRejection() const;
    const char* initializeRejection() const;
    void setStartGuard(ProductStartGuard* guard) { startGuard_ = guard; }
    void networkState(bool wifiConnected, uint32_t now);
    void recoverAfterRestart(uint32_t now);
    bool pendingEventSettled() const {
        return eventPending_ && !ownsMotion() && !flow_.busy() && flow_.stationary();
    }
    bool stop(uint32_t now, bool& wasActive);
    bool abort(uint32_t now, const char* reason, const char* errorCode);
    bool clean(uint32_t now, const char*& rejection);
    bool initialize(uint32_t now);
    void tick(uint32_t now);
    void setEventPending(bool pending) { eventPending_ = pending; }
    bool eventPending() const { return eventPending_; }
    bool takeTerminal(ProductTerminal& terminal);
    bool active() const { return active_; }
    bool cleaning() const { return cleaning_; }
    bool ownsMotion() const { return active_ || cleanPending_ || cleaning_; }
    bool canStart() const;
    bool flowConfigured() const { return flow_.config().configured; }
    bool referenceValid() const { return flow_.referenceValid(); }
    const char* progress() const;
    const char* errorCode() const;
    bool waterValid() const { return waterValid_; }
    bool lowWater() const { return lowWater_; }
    bool powderValid() const { return powderValid_; }
    float powderGrams() const { return powderGrams_; }
    int targetTemp() const { return targetTemp_; }
    void setTargetTemp(int temperatureC) { targetTemp_ = temperatureC; }
    const ProductRun& activeRun() const { return activeRun_; }
    babytech::display::DisplaySnapshot displaySnapshot() const;

private:
    bool start(ProductRun run, uint32_t now, const char*& rejection);
    void finish(bool completed, const char* reason, const char* errorCode);
    DemoFlowController& flow_;
    ProductStartGuard* startGuard_ = nullptr;
    FeedingContext context_;
    ProductRun activeRun_;
    ProductTerminal terminal_;
    bool waterValid_ = false;
    bool executionAuthorized_ = false;
    bool contextStorageReady_ = true;
    bool lowWater_ = false;
    bool powderValid_ = false;
    float powderGrams_ = 0.0f;
    int targetTemp_ = 45;
    bool active_ = false;
    bool stopPending_ = false;
    std::string stopReason_;
    std::string stopErrorCode_;
    bool terminalAvailable_ = false;
    bool eventPending_ = false;
    bool cleanPending_ = false;
    bool cleaning_ = false;
};

}  // namespace motion
