#pragma once

#include <ProductEventMessages.h>

#include <cstring>
#include <memory>
#include <new>

namespace babytech { namespace brain {

enum class SimulationModeResult { Changed, Unchanged, Busy };
enum class SimulationStart { Accepted, Duplicate, Disabled, Busy, Full, Invalid, Conflict, Expired, Unavailable };

// UI-loop-owned developer tool. Only the overall action is simulated; no UART,
// motor, network or persistence access. The caller owns session/TTL admission,
// verified identity and Cloud delivery. RAM results do not survive a reset.
class BrainSimulation {
public:
    static constexpr uint32_t kDefaultDurationMs = 15000;
    static constexpr size_t kResultCapacity = 4;

    explicit BrainSimulation(const v4::Pairing& pairing, uint32_t durationMs = kDefaultDurationMs)
        : pairing_(pairing), durationMs_(durationMs) {}
    BrainSimulation(const BrainSimulation&) = delete;
    BrainSimulation& operator=(const BrainSimulation&) = delete;

    SimulationModeResult setEnabled(bool enabled, bool realRequestUnresolved) {
        if (enabled == enabled_) return SimulationModeResult::Unchanged;
        if (active_ || realRequestUnresolved) return SimulationModeResult::Busy;
        enabled_ = enabled;
        return SimulationModeResult::Changed;
    }

    bool enabled() const { return enabled_; }
    bool running() const { return active_; }
    uint32_t durationMs() const { return durationMs_; }
    size_t resultCount() const { return resultCount_; }
    bool canStart() const {
        return enabled_ && !active_ && resultCount_ < kResultCapacity && validSetup();
    }
    const boardlink::ProductRequest* activeRequest() const {
        return active_ ? &current_.request : nullptr;
    }
    const boardlink::TerminalEvent* result(size_t index) const {
        return index < resultCount_ ? &results_[index] : nullptr;
    }

    SimulationStart start(const boardlink::ProductRequest& request, uint32_t nowMs) {
        if (!enabled_) return SimulationStart::Disabled;
        if (!validSetup() || !boardlink::validProductRequest(request) ||
            std::strcmp(request.deviceId, pairing_.deviceId)) return SimulationStart::Invalid;
        auto& last = request.source == v4::Source::CloudCommand ? lastCloud_ : lastLocal_;
        auto& lastDecision = request.source == v4::Source::CloudCommand ? lastCloudDecision_ : lastLocalDecision_;
        const auto remember = [&](SimulationStart decision) {
            if (request.sequence > last.sequence) { last = request; lastDecision = decision; }
            return decision;
        };
        if (active_ && overlaps(current_.request, request)) return remember(classify(current_.request, request));
        for (size_t i = 0; i < resultCount_; ++i)
            if (overlaps(results_[i].request, request)) return remember(classify(results_[i].request, request));
        if (request.sequence < last.sequence) return SimulationStart::Expired;
        if (last.sequence && overlaps(last, request)) {
            if (!boardlink::sameProductRequest(last, request)) return remember(SimulationStart::Conflict);
            return lastDecision == SimulationStart::Accepted ? SimulationStart::Duplicate : lastDecision;
        }
        if (request.sequence <= last.sequence) return SimulationStart::Expired;
        const auto& otherLast = request.source == v4::Source::CloudCommand ? lastLocal_ : lastCloud_;
        if (otherLast.sequence && !std::strcmp(otherLast.commandId, request.commandId))
            return remember(SimulationStart::Conflict);
        if (request.command != boardlink::ProductCommand::Prepare) return remember(SimulationStart::Invalid);
        // Busy/full/failed admission is a final decision for these bytes, not
        // an implicit queue that may acquire execution on a later delivery.
        if (active_) return remember(SimulationStart::Busy);
        if (resultCount_ == kResultCapacity) return remember(SimulationStart::Full);

        boardlink::TerminalEvent next;
        next.request = request;
        next.targetPowderG = boardlink::productTargetPowderG(request);
        if (!boardlink::makeProductEventId(pairing_, request.source, request.sequence, next.eventId))
            return remember(SimulationStart::Invalid);
        // Reserve deliverable space for either completion or Stop before
        // accepting. Never trim an already accepted result to fit its JSON.
        std::unique_ptr<v4::Message> scratch(new (std::nothrow) v4::Message);
        if (!scratch) return remember(SimulationStart::Unavailable);
        next.completed = true;
        if (!boardlink::encodeTerminalEvent(pairing_, next, *scratch)) return remember(SimulationStart::Unavailable);
        next.completed = false;
        std::strcpy(next.reason, "stopped");
        std::strcpy(next.errorCode, "E_STOPPED");
        if (!boardlink::encodeTerminalEvent(pairing_, next, *scratch)) return remember(SimulationStart::Unavailable);
        next.reason[0] = next.errorCode[0] = 0;
        current_ = next;
        startedAtMs_ = nowMs;
        active_ = true;
        return remember(SimulationStart::Accepted);
    }

    void poll(uint32_t nowMs) {
        if (active_ && uint32_t(nowMs - startedAtMs_) >= durationMs_) finish(true, nowMs);
    }

    bool stop(uint32_t nowMs) {
        if (!active_) return false;
        finish(false, nowMs);
        return true;
    }

    // Call only after decoding an actual Cloud stored receipt. MQTT queueing
    // and UART ACKs never enter this API. It affects only this simulation owner.
    bool acknowledge(const boardlink::CloudReceipt& receipt) {
        if (std::strncmp(receipt.deviceId, pairing_.deviceId, sizeof(receipt.deviceId))) return false;
        for (size_t i = 0; i < resultCount_; ++i) {
            if (std::strncmp(receipt.eventId, results_[i].eventId, sizeof(receipt.eventId))) continue;
            for (size_t j = i + 1; j < resultCount_; ++j) results_[j - 1] = results_[j];
            results_[--resultCount_] = {};
            return true;
        }
        return false;
    }

private:
    bool validSetup() const {
        return v4::validPairing(pairing_) && pairing_.role == v4::Role::Brain &&
            durationMs_ && durationMs_ <= INT32_MAX;
    }
    static bool overlaps(const boardlink::ProductRequest& left, const boardlink::ProductRequest& right) {
        return (left.source == right.source && left.sequence == right.sequence) ||
            !std::strcmp(left.commandId, right.commandId);
    }
    static SimulationStart classify(const boardlink::ProductRequest& left,
                                    const boardlink::ProductRequest& right) {
        return boardlink::sameProductRequest(left, right) ? SimulationStart::Duplicate : SimulationStart::Conflict;
    }
    void finish(bool completed, uint32_t nowMs) {
        current_.completed = completed;
        current_.uptimeMs = nowMs;
        if (!completed) {
            std::strcpy(current_.reason, "stopped");
            std::strcpy(current_.errorCode, "E_STOPPED");
        }
        results_[resultCount_++] = current_;
        active_ = false;
        current_ = {};
    }

    const v4::Pairing pairing_;
    const uint32_t durationMs_;
    bool enabled_ = false;
    bool active_ = false;
    uint32_t startedAtMs_ = 0;
    size_t resultCount_ = 0;
    boardlink::TerminalEvent current_;
    boardlink::TerminalEvent results_[kResultCapacity];
    boardlink::ProductRequest lastCloud_, lastLocal_;
    SimulationStart lastCloudDecision_ = SimulationStart::Invalid;
    SimulationStart lastLocalDecision_ = SimulationStart::Invalid;
};

} }
