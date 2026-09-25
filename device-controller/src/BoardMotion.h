#pragma once
#include <BoardEndpoint.h>
#include "DeviceAPI.h"

// Typed bridge: no JSON parsing and no second mechanical state machine.
class BoardMotion : public babytech::v2::Backend {
public:
    BoardMotion(motion::MotorControl& motor, motion::DeviceAPI& api) : motor_(motor), api_(api) {}
    void setRadioBusy(bool busy) { radioBusy_=busy; }
    bool unverifiedMode() const override { return api_.unverifiedMode(); }
    bool busy() const override { return !unverifiedMode() && (radioBusy_ || motor_.operationBusy()); }
    bool stopping() const override { return motor_.stopping(); }
    bool fault() const override { return motor_.hasFault(); }
    bool motorsAvailable() const override { return motor_.anyMotorOnline(); }
    babytech::v2::Reason startMove(const babytech::v2::Parameters& p) override;
    babytech::v2::Reason enable(uint8_t motor,bool enabled) override;
    babytech::v2::Outcome operation(babytech::v2::Reason& reason) const override;
    void stop() override;
    bool stopped() const override;
    void watch(uint8_t id) override { motor_.watch(id); }
    size_t motorFeedback(uint8_t id,uint8_t* data,size_t capacity) const override;
private:
    motion::MotorControl& motor_;
    motion::DeviceAPI& api_;
    bool radioBusy_=false, moving_=false, desired_=false, stopSent_=false;
    uint8_t id_=0; uint32_t since_=0, operationAt_=0;
    bool stopTargets_[256]{};
};
