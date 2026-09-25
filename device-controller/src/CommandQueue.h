#pragma once
// Board queue: ordered command sender with opt-in completion observation.
// Plain commands advance after transmission. Only move/home with a trailing
// await enters the runner's Motion phase; wait and legacy timed torque/velocity
// use runner timers.
// Await observes driver evidence without invoking manual controller policy:
// no implicit enable, retry, timeout stop, or automatic fault latching.
// Missing feedback leaves await pending; explicit driver rejection fails it.
// Cancel remains an explicit broadcast abort/stop operation.
// Opt-in sync/helix groups instead own fresh preflight, a shared trajectory,
// cache confirmation, one trigger and independent completion/fault stops.
//
// Memory: the plan is a fixed array of 64 QueueStep values held by
// the queue object in .bss, plus one function-local static scratch buffer of the
// same size used for validation. No whole-program value is ever placed on a
// task stack, and nothing is allocated per step: the ESP32 loop task keeps its
// default 8 KB stack.

#include <stddef.h>
#include <stdint.h>

#include "Arduino.h"
#include "DebugLimits.h"
#include "MotorControl.h"
#include "ProgramRunner.h"
#include "QueueProgram.h"
#include "SyncRuntime.h"

namespace motion {

class CommandQueue : private SyncPort {
public:
    explicit CommandQueue(MotorControl& motor) : motor_(motor), sync_(*this,motor.syncQueryScheduler()) {}
    bool setSyncSettings(const SyncSettings& settings) {
        if(active() || sync_.active() || !settings.valid()) return false;
        syncSettings_=settings;return true;
    }
    const SyncSettings& syncSettings() const {return syncSettings_;}
    String syncSettingsJson() const;

    // Call every loop after MotorControl::poll().
    void poll(uint32_t now);

    // Parses the whole program (no CAN frame is sent, and no state is cleared,
    // when the text is invalid) and starts sending it. 202 on success, 400 with
    // a stable reason + lastErrorLine() for an invalid program, 409 while a run
    // is still going, 503 when the bus cannot transmit at all.
    Result start(const char* text, size_t length, long repeat,
                 const QueueRotationSource& rotation, uint32_t now);
    // Prevalidated demo scripts preserve feedback and require positive homing
    // proof; the ordinary direct queue retains its existing behavior.
    Result startDemo(const QueueProgram& program, uint32_t now);

    // Cancels the run (if any) and stops everything: broadcast 9C then the FE
    // broadcast stop. Returns 202 when a run was cancelled, 200 when idle.
    Result cancel(const char* reason);

    // Explicit operator reset: cancel pending steps and attempt stop exactly
    // once, then clear queue/controller ownership even if stop cannot be sent.
    // The result reports stop transmission only, never physical completion.
    Result clearControlState();

    bool active() const { return runner_.active() || sync_.active(); }
    bool containsRaw() const { return program_.hasRaw; }
    QueueState state() const { return runner_.state(); }
    uint32_t runId() const { return runner_.runId(); }
    uint16_t lastErrorLine() const { return runner_.errorLine(); }
    const char* message() const { return runner_.message(); }

    // Complete JSON object for GET /api/queue.
    String statusJson() const;

private:
    void beginStep(uint32_t now);
    void dispatchStep(uint32_t now, const QueueStep& step);
    bool encodeAndSend(const QueueStep& step, bool synchronized=false);
    bool sendStopFrame(uint8_t id);
    bool stopEverything();
    void observeMotion(uint32_t now);
    uint32_t observedAckAt_ = 0, donePosAt_ = 0, doneVelAt_ = 0;
    uint8_t doneSamples_ = 0;
    int64_t expectedMoveTargetTenths_ = 0;
    bool expectedMoveTargetValid_ = false;
    uint32_t homeProofAt_ = 0;
    bool accepted_ = false, homeSeenRunning_ = false, homeComplete_ = false;

    void advance(uint32_t now);
    void fail(uint32_t now, const char* reason, uint16_t line);
    void setMessage(const char* text);
    const QueueStep* currentStep() const;
    SyncFeedback syncFeedback(uint8_t id) const override;
    bool syncSendMove(const QueueStep& step) override;
    bool syncTrigger() override;
    bool syncStop(uint8_t id) override;
    void syncObserve(uint8_t id,bool value) override;
    void pollSync(uint32_t now);

    MotorControl& motor_;
    SyncSettings syncSettings_;
    SyncRuntime sync_;
    double helixTravelMm_=0;
    double helixGeometryErrorMm_=0;
    QueueProgram program_{};
    ProgramRunner runner_;
};

}  // namespace motion
