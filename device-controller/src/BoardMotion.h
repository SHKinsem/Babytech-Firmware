#pragma once
#include <BoardEndpoint.h>
#include "MotorControl.h"

// Typed bridge: no JSON parsing and no second mechanical state machine.
class BoardMotion : public babytech::v2::Backend {
public:
    explicit BoardMotion(motion::MotorControl& motor) : motor_(motor) {}
    void setRadioBusy(bool busy) { radioBusy_=busy; }
    bool busy() const override { return radioBusy_ || motor_.operationBusy(); }
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
    bool radioBusy_=false, moving_=false, desired_=false, stopSent_=false;
    uint8_t id_=0; uint32_t since_=0, operationAt_=0;
    bool stopTargets_[256]{};
};
