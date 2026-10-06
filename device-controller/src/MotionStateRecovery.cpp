#include "MotionStateRecovery.h"
#include <cstdio>
#include <cstring>

namespace motion {
using namespace babytech::boardlink;

MotionLoad MotionStateRecovery::begin(const babytech::v4::Pairing& pairing, uint32_t nowMs) {
    if (began_) return loaded_;
    began_ = true;
    interruptedAtMs_ = nowMs;
    loaded_ = store_.load(pairing);
    recoveryPending_ = loaded_ == MotionLoad::Ready &&
        store_.state().slot.kind != MotionSlotKind::Empty;
    // A paired controller with unavailable state cannot prove that a previous
    // product operation ended. Stop independently of storage health.
    stopPending_ = loaded_ != MotionLoad::Ready || recoveryPending_;
    if (stopPending_) hardware_.supervisedStop(nowMs);
    return loaded_;
}

void MotionStateRecovery::poll() {
    if (!began_) return;
    if (stopPending_) {
        if (!hardware_.stationary()) return;
        stopPending_ = false;
    }
    if (!recoveryPending_ || !store_.ready()) return;
    // Recheck when recovering a record; a stop confirmation in a previous poll
    // is not permission to clear an intent after new movement or stale samples.
    if (!hardware_.stationary()) return;
    const auto& slot = store_.state().slot;
    MotionWrite result = MotionWrite::Invalid;
    if (slot.kind == MotionSlotKind::Intent) {
        if (slot.request.command == ProductCommand::Prepare) {
            result = store_.finishFeeding(slot.executionId, false, "reboot_during_feed",
                                        "E_REBOOT_DURING_FEED", interruptedAtMs_);
            if (result != MotionWrite::Stored && result != MotionWrite::Unchanged) return;
        } else {
            result = store_.finishOperation(slot.executionId, MotionOutcome::Interrupted, true);
            if (result == MotionWrite::Stored || result == MotionWrite::Unchanged)
                recoveryPending_ = false;
            return;
        }
    }
    // Existing terminal snapshots retain their original outcome and identity.
    // Queued historical results are never loaded into ProductSession's legacy
    // single-slot eventPending gate, and remain until a verified Cloud receipt.
    if (store_.state().slot.kind != MotionSlotKind::Terminal || !hardware_.stationary()) return;
    result = store_.archiveFeeding(true);
    if (result == MotionWrite::Stored || result == MotionWrite::Unchanged)
        recoveryPending_ = false;
}

void MotionStateRecovery::project(Status& status) const {
    if (!began_) return;
    status.snapshot.startEnabled = false;
    status.executionAuthorized = false;
    if (!store_.ready()) {
        // Zero watermarks in this required wire layout are placeholders only:
        // this explicit fault is not a synchronized/ready product state.
        std::strcpy(status.productProgress, "error");
        if (!std::strcmp(status.productError, "NONE"))
            std::strcpy(status.productError, "E_STORAGE_FAULT");
        status.snapshot.stage = babytech::display::DisplayStage::Error;
        if (status.snapshot.error == babytech::display::DisplayError::None)
            status.snapshot.error = babytech::display::DisplayError::Unknown;
        status.feedingContextConfigured = false;
        status.contextVersion = 0;
        status.babyId[0] = 0;
        return;
    }
    const auto& state = store_.state();
    std::snprintf(status.cloudWatermark, sizeof(status.cloudWatermark), "%llu",
                  static_cast<unsigned long long>(state.cloudSequence));
    std::snprintf(status.localWatermark, sizeof(status.localWatermark), "%llu",
                  static_cast<unsigned long long>(state.localSequence));
    status.activeExecutionId[0] = status.pendingEventId[0] = 0;
    if (state.slot.kind != MotionSlotKind::Empty)
        std::strcpy(status.activeExecutionId, state.slot.executionId);
    if (state.slot.kind == MotionSlotKind::Terminal)
        std::strcpy(status.pendingEventId, state.slot.eventId);
    else if (state.pendingResultCount)
        std::strcpy(status.pendingEventId, state.pendingResults[0].eventId);
    status.eventPending = status.eventPending || state.pendingResultCount ||
        state.slot.kind == MotionSlotKind::Terminal;
    status.contextVersion = state.context.present ? state.context.profileVersion : 0;
    status.feedingContextConfigured = state.context.present && !state.context.cleared;
    std::strcpy(status.babyId, status.feedingContextConfigured ? state.context.babyId : "");
}

} // namespace motion
