#pragma once

#include "brain_simulation.h"
#include "brain_status.h"
#include "CloudSession.h"
#include "ProductBoardMessages.h"

namespace babytech { namespace brain {

// Separate UI-loop owner for the approved App/Cloud/Brain developer path.
// Intentionally has no Link, Motion Store or local-sequence interface.
template<class Network>
class BrainSimulationDispatcher {
public:
    using Clock = uint32_t (*)();
    BrainSimulationDispatcher(const v4::Pairing& pairing, Network& network, Clock clock,
                              uint32_t durationMs = BrainSimulation::kDefaultDurationMs)
        : pairing_(pairing), simulation_(pairing, durationMs), network_(network), clock_(clock) {}

    bool enabled() const { return simulation_.enabled(); }
    bool running() const { return simulation_.running(); }
    size_t resultCount() const { return simulation_.resultCount(); }
    uint32_t durationMs() const { return simulation_.durationMs(); }
    SimulationModeResult setEnabled(bool enabled, bool realRequestUnresolved) {
        const auto result = simulation_.setEnabled(enabled, realRequestUnresolved);
        if (result == SimulationModeResult::Changed) {
            // Immediate generation invalidation also drops queued old-mode
            // commands/status. A late simulation request cannot reach Motion.
            network_.resetCommandSession();
            completed_ = false;
        }
        refresh();
        return result;
    }

    void setContext(const boardlink::ProductContext* context, bool usable, bool admitted) {
        context_ = context && boardlink::validProductContext(*context) &&
            !std::strcmp(context->deviceId, pairing_.deviceId) ? context : nullptr;
        usable_ = usable && context_ && !context_->cleared;
        admitted_ = admitted;
        refresh();
    }
    const SimulationStatus& status() const { return status_; }

    void command(const boardlink::CloudCommand& incoming, uint32_t generation, uint32_t) {
        if (!enabled() || incoming.request.source != v4::Source::CloudCommand ||
            !boardlink::validProductRequest(incoming.request) ||
            std::strcmp(incoming.request.deviceId, pairing_.deviceId)) return;
        const auto freshness = network_.checkFreshness(incoming.session, generation,
            incoming.sampledAtMs, incoming.ttlMs);
        if (freshness != cloud::Freshness::Current && freshness != cloud::Freshness::Expired) return;
        const auto& request = incoming.request;
        if (request.sequence <= seenSequence_) {
            if (last_.request.sequence == request.sequence &&
                boardlink::sameProductRequest(last_.request, request)) {
                reply(last_, accepted_, reason_);
            } else if (const char* session = retainedSession(request)) {
                auto original = incoming;
                std::strcpy(original.session, session);
                reply(original, true, "accepted");
            }
            return;
        }
        seenSequence_ = request.sequence;
        const bool reusedId = last_.request.sequence && !std::strcmp(last_.request.commandId, request.commandId);
        last_ = incoming;
        accepted_ = false;
        reason_ = "request_expired";
        if (freshness == cloud::Freshness::Current) {
            if (reusedId) reason_ = "request_conflict";
            else if (replyPending_) reason_ = "busy";
            else if (!admitted_) reason_ = "integration_not_ready";
            else if (request.command != boardlink::ProductCommand::Prepare) reason_ = "unsupported_command";
            else if (!usable_ || !context_ || context_->cleared ||
                request.profileVersion != context_->profileVersion ||
                std::strcmp(request.babyId, context_->babyId) ||
                request.powderGPer100Ml != context_->powderGPer100Ml) reason_ = "context_required";
            else {
                Guard guard{*this, incoming, generation};
                const auto result = simulation_.start(request, clock_(), stillCurrent, &guard);
                accepted_ = result == SimulationStart::Accepted || result == SimulationStart::Duplicate;
                reason_ = accepted_ ? "accepted" : result == SimulationStart::Busy ? "busy" :
                    result == SimulationStart::Full ? "result_queue_full" :
                    result == SimulationStart::Expired ? "request_expired" :
                    result == SimulationStart::Conflict ? "request_conflict" : "simulation_unavailable";
                if (result == SimulationStart::Accepted) {
                    activeContext_ = *context_;
                    std::strcpy(activeSession_, incoming.session);
                    completed_ = false;
                }
            }
        }
        reply(last_, accepted_, reason_);
        refresh();
    }

    void stop(const boardlink::CloudStop& incoming, uint32_t generation, uint32_t) {
        if (!enabled() || std::strcmp(incoming.deviceId, pairing_.deviceId)) return;
        const auto fresh = network_.checkFreshness(incoming.session, generation,
            incoming.sampledAtMs, incoming.ttlMs);
        if (fresh != cloud::Freshness::Current && fresh != cloud::Freshness::Expired) return;
        if (incoming.sequence <= seenStopSequence_) {
            if (incoming.sequence == stop_.sequence && !std::strcmp(incoming.commandId, stop_.commandId))
                stopReplyPending_ = !network_.publishAck(stop_.commandId, "stop", stop_.sequence,
                    stop_.session, stopAccepted_, stopReason_);
            return;
        }
        seenStopSequence_ = incoming.sequence;
        if (incoming.sequence > seenSequence_) seenSequence_ = incoming.sequence;
        stop_ = incoming;
        const auto stopAtMs = clock_();
        stopAccepted_ = fresh == cloud::Freshness::Current &&
            network_.checkFreshness(incoming.session, generation, incoming.sampledAtMs, incoming.ttlMs)
                == cloud::Freshness::Current;
        stopReason_ = "request_expired";
        if (stopAccepted_) {
            const bool stopped = simulation_.stop(stopAtMs);
            if (stopped) std::strcpy(resultSessions_[resultCount() - 1], activeSession_);
            stopReason_ = stopped ? "accepted" : "already_idle";
            completed_ = false;
        }
        stopReplyPending_ = !network_.publishAck(stop_.commandId, "stop", stop_.sequence,
            stop_.session, stopAccepted_, stopReason_);
        refresh();
    }

    bool receipt(const boardlink::CloudReceipt& receipt) {
        size_t index = 0;
        while (index < resultCount() && std::strncmp(simulation_.result(index)->eventId,
               receipt.eventId, sizeof(receipt.eventId))) ++index;
        const bool acknowledged = simulation_.acknowledge(receipt);
        if (acknowledged) {
            for (size_t i = index; i < resultCount(); ++i)
                std::memcpy(resultSessions_[i], resultSessions_[i + 1], sizeof(resultSessions_[i]));
            resultSessions_[resultCount()][0] = 0;
        }
        refresh();
        return acknowledged;
    }

    void poll(uint32_t nowMs) {
        const bool wasRunning = running();
        simulation_.poll(nowMs);
        if (wasRunning && !running()) {
            std::strcpy(resultSessions_[resultCount() - 1], activeSession_);
            completed_ = true;
            completedAtMs_ = nowMs;
        }
        if (completed_ && uint32_t(nowMs - completedAtMs_) >= 2000) completed_ = false;
        if (replyPending_)
            replyPending_ = !network_.publishAck(reply_.request.commandId, name(reply_.request.command),
                reply_.request.sequence, reply_.session, replyAccepted_, replyReason_);
        if (stopReplyPending_)
            stopReplyPending_ = !network_.publishAck(stop_.commandId, "stop", stop_.sequence,
                stop_.session, stopAccepted_, stopReason_);
        // Queueing is not storage proof. Retain frozen results until a real
        // matching Cloud receipt, including after switching simulation off.
        if (resultCount() && (!attempted_ || uint32_t(nowMs - attemptedAtMs_) >= 250)) {
            attempted_ = true;
            attemptedAtMs_ = nowMs;
            nextResult_ %= resultCount();
            const auto* event = simulation_.result(nextResult_);
            network_.publishSimulationEvent(pairing_, *event);
            nextResult_ = (nextResult_ + 1) % resultCount();
        }
        refresh();
    }

private:
    struct Guard { BrainSimulationDispatcher& owner; const boardlink::CloudCommand& command; uint32_t generation; };
    static bool stillCurrent(void* value) {
        const auto& guard = *static_cast<Guard*>(value);
        return guard.owner.network_.checkFreshness(guard.command.session, guard.generation,
            guard.command.sampledAtMs, guard.command.ttlMs) == cloud::Freshness::Current;
    }
    static const char* name(boardlink::ProductCommand command) {
        switch (command) {
            case boardlink::ProductCommand::Prepare: return "prepare";
            case boardlink::ProductCommand::Clean: return "clean";
            case boardlink::ProductCommand::SetTargetTemp: return "set_target_temp";
            case boardlink::ProductCommand::ResetError: return "reset_error";
            case boardlink::ProductCommand::CheckFirmwareUpdate: return "check_firmware_update";
            default: return "prepare";
        }
    }
    const char* retainedSession(const boardlink::ProductRequest& request) const {
        if (const auto* active = simulation_.activeRequest())
            if (boardlink::sameProductRequest(*active, request)) return activeSession_;
        for (size_t i = 0; i < resultCount(); ++i)
            if (boardlink::sameProductRequest(simulation_.result(i)->request, request)) return resultSessions_[i];
        return nullptr;
    }
    void reply(const boardlink::CloudCommand& command, bool accepted, const char* reason) {
        if (network_.publishAck(command.request.commandId, name(command.request.command),
                command.request.sequence, command.session, accepted, reason)) return;
        // One retained acceptance/rejection, not a queue of executable actions.
        if (replyPending_) return;
        reply_ = command;
        replyAccepted_ = accepted;
        replyReason_ = reason;
        replyPending_ = true;
    }
    void refresh() {
        status_.commandsEnabled = enabled() && admitted_;
        status_.canStart = status_.commandsEnabled && usable_ && simulation_.canStart() && !replyPending_;
        status_.running = running();
        status_.complete = completed_;
        status_.request = simulation_.activeRequest();
        status_.context = running() ? &activeContext_ : context_;
    }
    const v4::Pairing pairing_;
    BrainSimulation simulation_;
    Network& network_;
    Clock clock_;
    const boardlink::ProductContext* context_ = nullptr;
    boardlink::ProductContext activeContext_;
    SimulationStatus status_;
    boardlink::CloudCommand last_, reply_;
    boardlink::CloudStop stop_;
    char activeSession_[33]{}, resultSessions_[BrainSimulation::kResultCapacity][33]{};
    uint64_t seenSequence_ = 0, seenStopSequence_ = 0;
    uint32_t attemptedAtMs_ = 0, completedAtMs_ = 0;
    size_t nextResult_ = 0;
    const char* reason_ = "integration_not_ready";
    const char* replyReason_ = "integration_not_ready";
    const char* stopReason_ = "already_idle";
    bool accepted_ = false, replyAccepted_ = false, stopAccepted_ = false;
    bool replyPending_ = false, stopReplyPending_ = false;
    bool attempted_ = false, completed_ = false;
    bool usable_ = false, admitted_ = false;
};

} }
