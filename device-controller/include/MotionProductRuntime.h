#pragma once

#include <MotionStateStore.h>
#include <ProductBoardMessages.h>
#include <ProductCommandResult.h>
#include <ProductResultQuery.h>
#include "ProductSession.h"

namespace motion {

class MotionProductHardware {
public:
    virtual ~MotionProductHardware() = default;
    virtual uint32_t nowMs() const = 0;
    // Local maintenance/OTA or another hardware owner; never external Cloud/Wi-Fi liveness.
    virtual const char* unavailable() const = 0;
    virtual bool newExecution(char (&id)[33]) = 0;
    virtual bool stationary() const = 0;
};

// One loop owner, using the existing session/flow and durable store. No CAN or
// MQTT is implemented here. Only a newly Stored acceptance can start hardware.
class MotionProductRuntime {
public:
    MotionProductRuntime(babytech::boardlink::MotionStateStore& store, ProductSession& product,
                         DemoFlowController& flow, MotionProductHardware& hardware)
        : store_(store), product_(product), flow_(flow), hardware_(hardware) {}
    bool command(const babytech::boardlink::CommandMessage& command, uint32_t nowMs,
                 babytech::boardlink::CommandResult& result);
    // A moving rejection is frozen but not final until poll persists it at rest.
    bool resultReady(babytech::boardlink::CommandResult& result);
    void releaseMotionOwnership() { motionOwned_ = false; }
    bool ownsMotion() const { return motionOwned_; }
    bool stopOwned(uint32_t nowMs);
    bool stop(const babytech::v4::StopRequest& request, uint32_t nowMs);
    void linkLost(uint32_t nowMs);
    void poll(uint32_t nowMs);
    void project(babytech::boardlink::Status& status, bool linkConnected) const;
    bool active() const { return execution_[0] != 0; }
private:
    ProductRun run(const babytech::boardlink::ProductRequest& request) const;
    bool matchingDigest(const babytech::boardlink::ProductRequest& request,
                        const char (&hex)[65]);
    void finishFailed(const char* reason, uint32_t nowMs);
    babytech::boardlink::MotionStateStore& store_;
    ProductSession& product_;
    DemoFlowController& flow_;
    MotionProductHardware& hardware_;
    babytech::boardlink::QueriedResult queried_{};
    ProductTerminal terminal_{};
    uint32_t terminalAt_ = 0;
    char execution_[33]{};
    babytech::boardlink::ProductCommand operation_ = babytech::boardlink::ProductCommand::None;
    bool terminalSeen_ = false;
    bool operationFailed_ = false;
    bool stopped_ = false;
    bool stopDelivered_ = false;
    bool motionOwned_ = false;
    bool deferred_ = false;
    bool deferredReply_ = false;
    char deferredReason_[65]{};
    babytech::boardlink::ProductRequest deferredRequest_{};
    babytech::boardlink::CommandResult deferredResult_{};
    bool cloudStopPending_ = false;
    babytech::v4::StopRequest cloudStop_{};
};

} // namespace motion
