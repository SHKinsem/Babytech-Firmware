#pragma once

#include <MotionStateStore.h>
#include <ProductBoardMessages.h>

namespace motion {

class MotionRecoveryHardware {
public:
    virtual ~MotionRecoveryHardware() = default;
    virtual void supervisedStop(uint32_t nowMs) = 0;
    // Must be fresh post-stop evidence for every configured axis, not an ACK,
    // elapsed timeout, empty queue or an idle software state.
    virtual bool stationary() const = 0;
};

// Single loop owner. The referenced store belongs in static storage. Ordinary
// boot only loads; missing/bad records are never installed or erased here.
// Recovery cannot start a motor, emit a Cloud receipt or reissue an intent.
class MotionStateRecovery {
public:
    MotionStateRecovery(babytech::boardlink::MotionStateStore& store,
                        MotionRecoveryHardware& hardware) : store_(store), hardware_(hardware) {}
    babytech::boardlink::MotionLoad begin(const babytech::v4::Pairing& verifiedPairing,
                                        uint32_t nowMs);
    void poll();
    void project(babytech::boardlink::Status& status) const;
    bool motionPending() const { return stopPending_; }
    // Stop confirmation may precede archival; receipts must not clear the
    // execution slot while this recovery owner still needs to finish it.
    bool executionPending() const { return recoveryPending_; }
    bool began() const { return began_; }
    babytech::boardlink::MotionLoad loadResult() const { return loaded_; }
private:
    babytech::boardlink::MotionStateStore& store_;
    MotionRecoveryHardware& hardware_;
    babytech::boardlink::MotionLoad loaded_ = babytech::boardlink::MotionLoad::Missing;
    uint32_t interruptedAtMs_ = 0;
    bool began_ = false;
    bool stopPending_ = false;
    bool recoveryPending_ = false;
};

} // namespace motion
