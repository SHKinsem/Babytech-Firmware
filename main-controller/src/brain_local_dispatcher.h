#pragma once

#include "BrainStateStore.h"
#include "ReadOnlyBoardLink.h"

#include <cstring>

namespace babytech { namespace brain {

// Shares the UI-loop UART and Store with Cloud dispatch and read-only recovery.
// Only a request reserved in this boot is sent; recovered pending is never replayed.
template<class Link>
class BrainLocalDispatcher {
public:
    using Clock = uint32_t (*)();
    BrainLocalDispatcher(Link& link, boardlink::BrainStateStore& store, Clock clock)
        : link_(link), store_(store), clock_(clock) {}
    BrainLocalDispatcher(const BrainLocalDispatcher&) = delete;
    BrainLocalDispatcher& operator=(const BrainLocalDispatcher&) = delete;

    bool busy() const { return active_; }
    const char* reason() const { return reason_; }
    void setPrepareReadyHandler(bool (*ready)()) { prepareReady_ = ready; }
    void setAcceptanceHandler(void (*handler)(const boardlink::ProductRequest&, uint32_t)) { acceptance_ = handler; }

    bool canStart(uint32_t nowMs, bool blocked = false) const {
        if (!available(nowMs, blocked) || (prepareReady_ && !prepareReady_())) return false;
        const auto& state = store_.state();
        const auto& status = *link_.lastTelemetry();
        return status.stationary && state.hasContext && !state.context.cleared &&
            status.snapshot.startEnabled && status.feedingContextConfigured &&
            status.contextVersion == state.context.profileVersion &&
            !std::strcmp(status.babyId, state.context.babyId);
    }

    bool canInitialize(uint32_t nowMs, bool blocked = false) const {
        return available(nowMs, blocked) && display::displayInitializeEnabled(
            link_.lastTelemetry()->snapshot, true, false);
    }

    bool dispatch(display::DisplayIntent intent, uint32_t nowMs, bool blocked = false) {
        if (intent != display::DisplayIntent::Initialize &&
            intent != display::DisplayIntent::StartFeeding) {
            reason_ = "invalid_parameter";
            return false;
        }
        if (!(intent == display::DisplayIntent::Initialize
              ? canInitialize(nowMs, blocked) : canStart(nowMs, blocked))) {
            reason_ = "not_ready";
            return false;
        }
        // Avoid reserving a durable request for predictable transport backpressure.
        // This is not motion authorization and cannot guarantee a later Flash success.
        if (!link_.commandAvailable(nowMs)) { reason_ = "busy"; return false; }
        boardlink::CommandMessage outgoing;
        const auto& state = store_.state();
        auto& request = outgoing.request;
        request.command = intent == display::DisplayIntent::Initialize
            ? boardlink::ProductCommand::Initialize : boardlink::ProductCommand::Prepare;
        request.sequence = state.localSequence + 1;
        std::strcpy(request.deviceId, state.pairing.deviceId);
        if (!boardlink::makeLocalCommandId(state.pairing, request.sequence, request.commandId)) {
            reason_ = "invalid_parameter";
            return false;
        }
        if (request.command == boardlink::ProductCommand::Prepare) {
            std::strcpy(request.babyId, state.context.babyId);
            request.profileVersion = state.context.profileVersion;
            request.waterMl = state.context.waterMl;
            request.temperatureC = state.context.temperatureC;
            request.powderGPer100Ml = state.context.powderGPer100Ml;
        }
        const uint32_t startedAt = nowMs;
        if (store_.reserveLocal(request) != boardlink::BrainWrite::Stored) {
            reason_ = "storage_fault";
            return false;
        }
        // Persistence/SHA consumes the original TTL. Even an unsent reserved
        // request remains evidence; only a definitive Motion result may clear it.
        const uint32_t sendAt = clock_();
        const uint32_t elapsed = uint32_t(sendAt - startedAt);
        if (elapsed >= kTtlMs || kTtlMs - elapsed <= v4::kCommandFirstFrameBudgetMs) {
            reason_ = "result_unknown";
            return false;
        }
        outgoing.remainingTtlMs = uint16_t(kTtlMs - elapsed);
        if (!link_.requestCommand(outgoing, sendAt)) {
            reason_ = "result_unknown";
            return false;
        }
        sentSequence_ = request.sequence;
        std::strcpy(sentCommandId_, request.commandId);
        active_ = true;
        reason_ = "sending";
        return true;
    }

    void poll(uint32_t nowMs) {
        if (!active_) return;
        const auto sent = link_.commandSendState();
        if (sent == boardlink::CommandSendState::Pending) return;
        active_ = false;
        if (sent == boardlink::CommandSendState::Complete && store_.ready() &&
            store_.state().pending) {
            const auto& request = store_.state().pendingRequest;
            const auto& result = link_.commandResponse();
            if (request.source == v4::Source::LocalTouch && request.sequence == sentSequence_ &&
                !std::strcmp(request.commandId, sentCommandId_) &&
                result.source == request.source && result.sequence == request.sequence &&
                !std::strcmp(result.commandId, request.commandId)) {
                std::strcpy(resultReason_, result.reason);
                if (result.accepted && acceptance_) acceptance_(request, nowMs);
                reason_ = resultReason_;
                // Both accepted and rejected are definitive acceptance results,
                // not feeding completion or permission to clear Motion events.
                if (store_.clearPending(request) != boardlink::BrainWrite::Stored)
                    reason_ = "storage_fault";
                link_.cancelCommand();
                return;
            }
        }
        link_.cancelCommand();
        reason_ = "result_unknown";
    }

private:
    bool available(uint32_t nowMs, bool blocked) const {
        if (blocked || active_ || !store_.ready() || store_.state().pending ||
            store_.state().localSequence >= v4::kMaxSequence || !link_.connected(nowMs)) return false;
        const auto* status = link_.lastTelemetry();
        // Initialize may recover an idle fault without a stationary certificate;
        // Motion's existing supervised recovery owns the mechanical decision.
        return status && uint32_t(nowMs - link_.lastTelemetryReceivedAtMs()) < 1500 &&
            !status->motionBusy && !status->activeExecutionId[0];
    }
    static constexpr uint32_t kTtlMs = 5000;
    Link& link_;
    boardlink::BrainStateStore& store_;
    Clock clock_;
    bool (*prepareReady_)() = nullptr;
    void (*acceptance_)(const boardlink::ProductRequest&, uint32_t) = nullptr;
    uint64_t sentSequence_ = 0;
    char sentCommandId_[129]{};
    char resultReason_[65]{};
    const char* reason_ = "idle";
    bool active_ = false;
};

} }
