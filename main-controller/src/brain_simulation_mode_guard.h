#pragma once

#include "ProductBoardMessages.h"
#include "ProductCommandResult.h"
#include <cstdio>
#include <cstring>

namespace babytech { namespace brain {

// Protects only a developer mode change, not normal product admission. An
// acceptance ACK is not proof that the physical action has already stopped.
class BrainSimulationModeGuard {
public:
    void observeAccepted(const boardlink::CommandResult& result, uint32_t nowMs,
                         boardlink::ProductCommand command = boardlink::ProductCommand::Prepare) {
        if (command != boardlink::ProductCommand::Prepare &&
            command != boardlink::ProductCommand::Initialize &&
            command != boardlink::ProductCommand::Clean) return;
        if (!result.accepted || !result.sequence || result.sequence > v4::kMaxSequence ||
            (result.source != v4::Source::CloudCommand && result.source != v4::Source::LocalTouch)) return;
        auto& seen = result.source == v4::Source::CloudCommand ? seenCloud_ : seenLocal_;
        if (result.sequence <= seen) return;
        seen = result.sequence;
        source_ = result.source;
        sequence_ = result.sequence;
        acceptedAtMs_ = nowMs;
        pending_ = true;
    }

    void observeAccepted(const boardlink::ProductRequest& request, uint32_t nowMs) {
        boardlink::CommandResult result{};
        result.source = request.source;
        result.sequence = request.sequence;
        result.accepted = true;
        observeAccepted(result, nowMs, request.command);
    }

    bool unresolved(const boardlink::Status* status, bool connected,
                    uint32_t receivedAtMs, uint32_t nowMs) {
        if (!pending_) return false; // A pure Brain test does not need Motion.
        const uint32_t elapsed = uint32_t(nowMs - acceptedAtMs_);
        const uint32_t receivedElapsed = uint32_t(receivedAtMs - acceptedAtMs_);
        if (!connected || !status || uint32_t(nowMs - receivedAtMs) >= 1500 ||
            !receivedElapsed || receivedElapsed > elapsed || status->motionBusy ||
            status->isPreparing || !status->stationary || status->activeExecutionId[0]) return true;
        const auto* watermark = source_ == v4::Source::CloudCommand ? status->cloudWatermark : status->localWatermark;
        // The watermark excludes an idle STATUS queued before acceptance but
        // received afterward. Decode already bounds/canonicalizes these fields.
        if (!std::memchr(watermark, 0, sizeof(status->cloudWatermark))) return true;
        char expected[20]{};
        std::snprintf(expected, sizeof(expected), "%llu", static_cast<unsigned long long>(sequence_));
        const size_t actualLength = std::strlen(watermark), expectedLength = std::strlen(expected);
        if (actualLength < expectedLength ||
            (actualLength == expectedLength && std::strcmp(watermark, expected) < 0)) return true;
        pending_ = false;
        return false;
    }

private:
    uint64_t seenCloud_ = 0, seenLocal_ = 0, sequence_ = 0;
    uint32_t acceptedAtMs_ = 0;
    v4::Source source_ = v4::Source::CloudCommand;
    bool pending_ = false;
};

} }
