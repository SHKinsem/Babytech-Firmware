#pragma once

#include "brain_stop_target.h"

#include "CloudSession.h"
#include "ProductBoardMessages.h"
#include "ProductCommandResult.h"
#include "ProductResultQuery.h"
#include "ReadOnlyBoardLink.h"

#include <cstring>

namespace babytech { namespace brain {

// One UI-loop owner; no action queue, network worker, or second persistent Store.
// Cloud owns issuance, Motion owns durable acceptance. Unknown requests are only
// queried, not replayed. Expired unknowns may release execution tracking on new
// idle/stationary evidence; Cloud/Motion records and local NVS remain untouched.
// Local pending recovery must not share the query slot while busy(). Its durable
// pending/maintenance ownership is passed to poll; Stop itself never waits on it.
template<class Link, class Network>
class BrainCloudDispatcher {
public:
    using Clock = uint32_t (*)();
    using Admission = const char* (*)();

    BrainCloudDispatcher(Link& link, Network& network, Clock clock, Admission admission)
        : link_(link), network_(network), clock_(clock), admission_(admission) {}
    BrainCloudDispatcher(const BrainCloudDispatcher&) = delete;
    BrainCloudDispatcher& operator=(const BrainCloudDispatcher&) = delete;

    bool busy() const { return active_ || stopQuerying_; }
    bool resultPending() const { return replyPending_; }
    bool ordinaryBusy() const { return active_; }
    bool stopInFlight() const { return stopPending_; }
    void setPrepareReadyHandler(bool (*ready)()) { prepareReady_ = ready; }
    void setConfigurationYieldHandler(void (*yield)(uint32_t)) { configurationYield_ = yield; }
    void setAcceptanceHandler(void (*handler)(const boardlink::ProductRequest&, uint32_t)) { acceptance_ = handler; }
    // A new explicit local operation has the same priority as a Cloud command;
    // informational Stop lookup never owns the mechanics or blocks new work.
    bool yieldToLocal(uint32_t nowMs) {
        if (active_ || stopPending_) return false;
        if (stopQuerying_) retryStopQuery(nowMs);
        return true;
    }

    void command(const boardlink::CloudCommand& incoming, uint32_t generation, uint32_t) {
        if (!boardlink::validProductRequest(incoming.request) ||
            incoming.request.source != v4::Source::CloudCommand) return;
        const auto freshness = network_.checkFreshness(incoming.session, generation, incoming.sampledAtMs, incoming.ttlMs);
        if (freshness != cloud::Freshness::Current && freshness != cloud::Freshness::Expired) return;
        // Replayed MQTT bytes must never acquire another execution opportunity
        // during their original session/TTL, even after a local busy rejection.
        if (incoming.request.sequence <= seenSequence_) return;
        seenSequence_ = incoming.request.sequence;
        if (freshness == cloud::Freshness::Expired) { reject(incoming, "request_expired"); return; }
        if (retiredCommandId_[0] && !std::strcmp(incoming.request.commandId, retiredCommandId_)) {
            reject(incoming, "request_conflict");
            return;
        }
        if (active_ || replyPending_) { reject(incoming, "busy"); return; }
        if (const char* reason = admission_()) { reject(incoming, reason); return; }
        if (incoming.request.command == boardlink::ProductCommand::Prepare &&
            prepareReady_ && !prepareReady_()) { reject(incoming, "context_required"); return; }
        if (!link_.connected(clock_())) { reject(incoming, "link_lost"); return; }
        // Informational Stop recovery yields to a new independent ordinary request.
        if (stopQuerying_) retryStopQuery(clock_());

        boardlink::CommandMessage outgoing;
        outgoing.request = incoming.request;
        const uint32_t nowMs = clock_();
        const uint32_t elapsed = uint32_t(nowMs - incoming.sampledAtMs);
        if (!fresh(incoming.session, generation, incoming.sampledAtMs, incoming.ttlMs) ||
            elapsed >= incoming.ttlMs || incoming.ttlMs - elapsed <= v4::kCommandFirstFrameBudgetMs) {
            reject(incoming, "request_expired");
            return;
        }
        outgoing.remainingTtlMs = uint16_t(incoming.ttlMs - elapsed);
        if (!boardlink::requestDigest(incoming.request, digest_)) {
            reject(incoming, "storage_fault");
            return;
        }
        // Digest computation also consumes the original deadline.
        const uint32_t sendAt = clock_();
        const uint32_t sendElapsed = uint32_t(sendAt - incoming.sampledAtMs);
        if (!fresh(incoming.session, generation, incoming.sampledAtMs, incoming.ttlMs) ||
            sendElapsed >= incoming.ttlMs || incoming.ttlMs - sendElapsed <= v4::kCommandFirstFrameBudgetMs) {
            reject(incoming, "request_expired");
            return;
        }
        outgoing.remainingTtlMs = uint16_t(incoming.ttlMs - sendElapsed);
        if (outgoing.request.command != boardlink::ProductCommand::Prepare && configurationYield_)
            configurationYield_(sendAt);
        if (!link_.requestCommand(outgoing, sendAt)) { reject(incoming, "busy"); return; }
        current_ = incoming;
        generation_ = generation;
        active_ = true;
        querying_ = false;
        waiting_ = false;
    }

    void stop(const boardlink::CloudStop& incoming, uint32_t generation, uint32_t) {
        if (!incoming.sequence || incoming.sequence > v4::kMaxSequence) return;
        const auto freshness = network_.checkFreshness(incoming.session, generation, incoming.sampledAtMs, incoming.ttlMs);
        if (freshness != cloud::Freshness::Current && freshness != cloud::Freshness::Expired) return;
        // Ordinary issuance/watermarks must not hide a valid safety Stop.
        if (incoming.sequence <= seenStopSequence_) return;
        seenStopSequence_ = incoming.sequence;
        if (incoming.sequence > seenSequence_) seenSequence_ = incoming.sequence;
        if (freshness == cloud::Freshness::Expired) {
            rejectStop(incoming, "request_expired");
            return;
        }
        if (stopPending_) {
            network_.publishAck(incoming.commandId, "stop", incoming.sequence, incoming.session, false, "busy");
            return;
        }
        v4::StopRequest target;
        if (!stopTarget(target, clock_())) {
            rejectStop(incoming, "motion_state_unavailable");
            return;
        }
        target.source = v4::Source::CloudCommand;
        target.sequence = incoming.sequence;
        const size_t length = std::strlen(incoming.commandId);
        if (!length || length > 128) return;
        std::memcpy(target.commandId, incoming.commandId, length + 1);
        target.commandIdLength = uint8_t(length);
        const uint32_t sendAt = clock_();
        if (!fresh(incoming.session, generation, incoming.sampledAtMs, incoming.ttlMs) ||
            uint32_t(sendAt - incoming.sampledAtMs) > incoming.ttlMs) {
            rejectStop(incoming, "request_expired");
            return;
        }
        // No admission/Store check, ordinary wait, or Flash precedes this call.
        if (!link_.requestStop(target, sendAt)) {
            rejectStop(incoming, "busy");
            return;
        }
        replaceStopTracking();
        stop_ = incoming;
        // A retained informational ACK must not gate a new explicit safety Stop.
        // Motion still retains its durable Cloud Stop evidence independently.
        stopReplyPending_ = false;
        idleStop_ = target.scope == v4::StopScope::Idle;
        stopPending_ = true;
    }

    void poll(uint32_t nowMs, bool localQueryOwner = false) {
        if (stopPending_) {
            const auto state = link_.stopSendState();
            if (state == boardlink::StopSendState::Received || state == boardlink::StopSendState::Rejected) {
                stopAccepted_ = state == boardlink::StopSendState::Received;
                stopReason_ = stopAccepted_ ? (idleStop_ ? "already_idle" : "accepted") : "stop_rejected";
                stopReplyPending_ = true;
                stopPending_ = false;
            } else if (state != boardlink::StopSendState::Pending) {
                // No false rejection/acceptance on an uncertain transport result.
                stopPending_ = false;
                stopUnknown_ = true;
                stopQueryWaiting_ = true;
                stopRetryAt_ = nowMs;
            }
        }
        pollStopQuery(nowMs, localQueryOwner);
        if (stopReplyPending_ && network_.publishAck(stop_.commandId, "stop", stop_.sequence,
                stop_.session, stopAccepted_, stopReason_))
            stopReplyPending_ = false;
        if (replyPending_ && network_.publishAck(current_.request.commandId, name(current_.request.command),
                current_.request.sequence, current_.session, result_.accepted, result_.reason))
            replyPending_ = false;
        if (!active_) return;

        if (!querying_ && !waiting_) {
            const auto state = link_.commandSendState();
            if (state == boardlink::CommandSendState::Complete) {
                const auto& response = link_.commandResponse();
                if (matches(response)) { complete(response.accepted, response.reason); return; }
                retry(nowMs);
            } else if (state == boardlink::CommandSendState::Pending) {
                if (network_.checkFreshness(current_.session, generation_, current_.sampledAtMs,
                                             current_.ttlMs) == cloud::Freshness::Current) return;
                link_.cancelCommand(); // D2 does not stop an already accepted Motion task.
                retry(nowMs);
            } else retry(nowMs);
        }
        if (querying_) {
            const auto state = link_.resultLookupState();
            if (state == boardlink::ResultLookupState::Pending) {
                releaseExpiredUnknown(clock_());
                return;
            }
            if (state == boardlink::ResultLookupState::Complete) {
                const auto& response = link_.resultQueryResponse();
                if (response.status == boardlink::ResultQueryStatus::Known &&
                    boardlink::sameResultQuery(query_, response.query) && digestMatches(response.requestDigestHex)) {
                    link_.cancelResultQuery();
                    querying_ = false;
                    complete(response.accepted, response.reason);
                    return;
                }
            }
            retry(nowMs);
            releaseExpiredUnknown(clock_());
            return;
        }
        if (releaseExpiredUnknown(clock_())) return;
        if (waiting_ && uint32_t(nowMs - retryAt_) < kRetryMs) return;
        query_.source = current_.request.source;
        query_.sequence = current_.request.sequence;
        std::strcpy(query_.deviceId, current_.request.deviceId);
        std::strcpy(query_.commandId, current_.request.commandId);
        if (!link_.requestResult(query_, nowMs)) { retry(nowMs); return; }
        waiting_ = false;
        querying_ = true;
    }

private:
    static constexpr uint32_t kRetryMs = 1000;
    void replaceStopTracking() {
        if (stopQuerying_) link_.cancelResultQuery();
        stopQuerying_ = stopUnknown_ = stopQueryWaiting_ = false;
    }
    void retryStopQuery(uint32_t nowMs) {
        if (stopQuerying_) link_.cancelResultQuery();
        stopQuerying_ = false;
        stopQueryWaiting_ = true;
        stopRetryAt_ = nowMs;
    }
    void pollStopQuery(uint32_t nowMs, bool localQueryOwner) {
        if (!stopUnknown_) return;
        // Only one read-only query slot exists. Ordinary acceptance recovery and
        // durable local pending take precedence over this informational reply.
        if (active_ || stopPending_ || localQueryOwner) {
            if (stopQuerying_) retryStopQuery(nowMs);
            return;
        }
        if (stopQuerying_) {
            const auto state = link_.resultLookupState();
            if (state == boardlink::ResultLookupState::Pending) return;
            if (state == boardlink::ResultLookupState::Complete) {
                const auto& result = link_.resultQueryResponse();
                if (result.status == boardlink::ResultQueryStatus::Known &&
                    boardlink::sameResultQuery(stopQuery_, result.query) &&
                    !result.requestDigestHex[0] && result.outcome == boardlink::MotionOutcome::None) {
                    // Empty digest is the existing codec's Cloud Stop result tag;
                    // a same-ID ordinary result is not evidence for this Stop.
                    stopAccepted_ = result.accepted;
                    std::strcpy(stopRecoveredReason_, result.reason);
                    stopReason_ = stopRecoveredReason_;
                    replaceStopTracking();
                    stopReplyPending_ = true;
                    return;
                }
            }
            retryStopQuery(nowMs);
            return;
        }
        if (stopQueryWaiting_ && uint32_t(nowMs - stopRetryAt_) < kRetryMs) return;
        stopQuery_.source = v4::Source::CloudCommand;
        stopQuery_.sequence = stop_.sequence;
        std::strcpy(stopQuery_.deviceId, stop_.deviceId);
        std::strcpy(stopQuery_.commandId, stop_.commandId);
        // Queries use the current UART session, but retain the original Cloud
        // identity/session for the ACK. No Cloud freshness or STATUS is required.
        if (!link_.requestResult(stopQuery_, nowMs)) { retryStopQuery(nowMs); return; }
        stopQuerying_ = true;
        stopQueryWaiting_ = false;
    }
    static const char* name(boardlink::ProductCommand command) {
        switch (command) {
            case boardlink::ProductCommand::Prepare: return "prepare";
            case boardlink::ProductCommand::Clean: return "clean";
            case boardlink::ProductCommand::SetTargetTemp: return "set_target_temp";
            case boardlink::ProductCommand::ResetError: return "reset_error";
            case boardlink::ProductCommand::CheckFirmwareUpdate: return "check_firmware_update";
            default: return "";
        }
    }
    bool fresh(const char* session, uint32_t generation, uint32_t sampled, uint16_t ttl) {
        return network_.checkFreshness(session, generation, sampled, ttl) == cloud::Freshness::Current;
    }
    bool releaseExpiredUnknown(uint32_t nowMs) {
        if (!active_ || (!waiting_ && !querying_)) return false;
        const uint32_t elapsed = uint32_t(nowMs - current_.sampledAtMs);
        if (elapsed <= current_.ttlMs || !link_.connected(nowMs)) return false;
        const auto* status = link_.lastTelemetry();
        if (!status || status->motionBusy || !status->stationary || status->activeExecutionId[0]) return false;
        const uint32_t receivedAt = link_.lastTelemetryReceivedAtMs();
        const uint32_t receivedElapsed = uint32_t(receivedAt - current_.sampledAtMs);
        if (uint32_t(nowMs - receivedAt) >= 1500 || receivedElapsed <= current_.ttlMs ||
            receivedElapsed > elapsed) return false;
        // Only retire volatile ownership. Never invent an outcome or clear NVS.
        std::strcpy(retiredCommandId_, current_.request.commandId);
        link_.cancelCommand();
        if (querying_) link_.cancelResultQuery();
        active_ = waiting_ = querying_ = false;
        return true;
    }
    void reject(const boardlink::CloudCommand& command, const char* reason) {
        if (active_ || replyPending_) {
            // No unbounded reply queue; never overwrite an outstanding original.
            network_.publishAck(command.request.commandId, name(command.request.command), command.request.sequence,
                                command.session, false, reason);
            return;
        }
        current_ = command;
        complete(false, reason);
        if (network_.publishAck(command.request.commandId, name(command.request.command), command.request.sequence,
                                command.session, false, reason)) replyPending_ = false;
    }
    void rejectStop(const boardlink::CloudStop& stop, const char* reason) {
        if (stopPending_) {
            network_.publishAck(stop.commandId, "stop", stop.sequence, stop.session, false, reason);
            return;
        }
        replaceStopTracking();
        stop_ = stop;
        stopAccepted_ = false;
        stopReason_ = reason;
        stopReplyPending_ = !network_.publishAck(stop.commandId, "stop", stop.sequence, stop.session, false, reason);
    }
    bool matches(const boardlink::CommandResult& result) const {
        return result.source == current_.request.source && result.sequence == current_.request.sequence &&
            !std::strcmp(result.commandId, current_.request.commandId);
    }
    bool digestMatches(const char (&text)[65]) const {
        constexpr char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < sizeof(digest_); ++i)
            if (text[2 * i] != hex[digest_[i] >> 4] || text[2 * i + 1] != hex[digest_[i] & 15]) return false;
        return text[64] == 0;
    }
    bool stopTarget(v4::StopRequest& target, uint32_t nowMs) const {
        return bindStopTarget(link_, nowMs, target);
    }
    void complete(bool accepted, const char* reason) {
        if (accepted && acceptance_) acceptance_(current_.request, clock_());
        result_.source = current_.request.source;
        result_.sequence = current_.request.sequence;
        std::strcpy(result_.commandId, current_.request.commandId);
        result_.accepted = accepted;
        std::strcpy(result_.reason, reason);
        active_ = false;
        waiting_ = false;
        replyPending_ = true;
    }
    void retry(uint32_t nowMs) {
        if (querying_) link_.cancelResultQuery();
        querying_ = false;
        waiting_ = true;
        retryAt_ = nowMs;
    }

    Link& link_;
    Network& network_;
    Clock clock_;
    Admission admission_;
    bool (*prepareReady_)() = nullptr;
    void (*configurationYield_)(uint32_t) = nullptr;
    void (*acceptance_)(const boardlink::ProductRequest&, uint32_t) = nullptr;
    boardlink::CloudCommand current_{};
    boardlink::CommandResult result_{};
    boardlink::ResultQuery query_{};
    boardlink::ResultQuery stopQuery_{};
    boardlink::CloudStop stop_{};
    char stopRecoveredReason_[65]{};
    // A bounded RAM guard for the last retired ID; Cloud's permanent issuance
    // ledger prevents reuse of every historical ID, including after Brain boot.
    char retiredCommandId_[129]{};
    uint8_t digest_[boardlink::kProductDigestSize]{};
    uint64_t seenSequence_ = 0;
    uint64_t seenStopSequence_ = 0;
    uint32_t generation_ = 0;
    uint32_t retryAt_ = 0;
    uint32_t stopRetryAt_ = 0;
    bool active_ = false;
    bool waiting_ = false;
    bool querying_ = false;
    bool replyPending_ = false;
    bool stopPending_ = false;
    bool stopReplyPending_ = false;
    bool stopAccepted_ = false;
    bool stopUnknown_ = false;
    bool stopQuerying_ = false;
    bool stopQueryWaiting_ = false;
    bool idleStop_ = false;
    const char* stopReason_ = "stop_rejected";
};

} }
