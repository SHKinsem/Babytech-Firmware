#pragma once

#include "BrainStateStore.h"
#include "ProductResultQuery.h"
#include "ReadOnlyBoardLink.h"

#include <cstring>

namespace babytech { namespace brain {

enum class BrainPendingRecoveryState {
    Idle, Querying, Waiting, Paused, Resolved, StoreUnavailable, ClearFault
};

// The same loop owns Link and the already-loaded business store. Recovery only
// queries old acceptance evidence; Resolved does not mean motion completed.
template<class Link>
class BrainPendingRecovery {
public:
    BrainPendingRecovery(Link& link, boardlink::BrainStateStore& store)
        : link_(link), store_(store) {}
    BrainPendingRecovery(const BrainPendingRecovery&) = delete;
    BrainPendingRecovery& operator=(const BrainPendingRecovery&) = delete;

    BrainPendingRecoveryState state() const { return state_; }
    bool clearFault() const { return clearFault_; }
    void setAcceptanceHandler(void (*handler)(const boardlink::ProductRequest&, uint32_t)) { acceptance_ = handler; }

    void poll(uint32_t nowMs, bool maintenance = false) {
        if (clearFault_) return;
        if (maintenance) {
            if (querying_) retry(nowMs);
            state_ = BrainPendingRecoveryState::Paused;
            return;
        }
        if (!store_.ready()) {
            if (querying_) retry(nowMs);
            state_ = BrainPendingRecoveryState::StoreUnavailable;
            return;
        }
        const auto& pending = store_.state();
        if (!pending.pending) {
            cancel();
            waiting_ = false;
            if (state_ != BrainPendingRecoveryState::Resolved)
                state_ = BrainPendingRecoveryState::Idle;
            return;
        }

        boardlink::ResultQuery current;
        current.source = pending.pendingRequest.source;
        current.sequence = pending.pendingRequest.sequence;
        std::memcpy(current.deviceId, pending.pendingRequest.deviceId, sizeof(current.deviceId));
        std::memcpy(current.commandId, pending.pendingRequest.commandId, sizeof(current.commandId));
        if (querying_) {
            // Another ordinary owner may have resolved/replaced pending between
            // ticks. An old query must never clear its replacement.
            if (!boardlink::sameResultQuery(current, query_)) { retry(nowMs); return; }
            const auto lookup = link_.resultLookupState();
            if (lookup == boardlink::ResultLookupState::Pending) return;
            if (lookup == boardlink::ResultLookupState::Complete) {
                const auto& result = link_.resultQueryResponse();
                if (result.status == boardlink::ResultQueryStatus::Known &&
                    boardlink::sameResultQuery(result.query, query_) &&
                    digestMatches(pending.pendingDigest, result.requestDigestHex)) {
                    cancel();
                    if (result.accepted && acceptance_) acceptance_(pending.pendingRequest, nowMs);
                    // accepted=false is definitive rejection; either ACK resolves
                    // acceptance in flight, independently of feeding outcome.
                    const auto written = store_.clearPending(pending.pendingRequest);
                    if (written == boardlink::BrainWrite::Stored) {
                        waiting_ = false;
                        state_ = BrainPendingRecoveryState::Resolved;
                    } else {
                        clearFault_ = true;
                        state_ = BrainPendingRecoveryState::ClearFault;
                    }
                    return;
                }
            }
            retry(nowMs);
            return;
        }
        if (waiting_ && uint32_t(nowMs - retryStartedAt_) < kRetryMs) {
            state_ = BrainPendingRecoveryState::Waiting;
            return;
        }
        query_ = current;
        if (!link_.requestResult(query_, nowMs)) { retry(nowMs); return; }
        waiting_ = false;
        querying_ = true;
        state_ = BrainPendingRecoveryState::Querying;
    }

private:
    static constexpr uint32_t kRetryMs = 1000;
    static bool digestMatches(const uint8_t (&digest)[boardlink::kProductDigestSize],
                              const char (&text)[65]) {
        constexpr char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < boardlink::kProductDigestSize; ++i)
            if (text[2 * i] != hex[digest[i] >> 4] || text[2 * i + 1] != hex[digest[i] & 15])
                return false;
        return text[64] == 0;
    }
    void cancel() {
        if (querying_) link_.cancelResultQuery();
        querying_ = false;
    }
    void retry(uint32_t nowMs) {
        cancel();
        waiting_ = true;
        retryStartedAt_ = nowMs;
        state_ = BrainPendingRecoveryState::Waiting;
    }

    Link& link_;
    boardlink::BrainStateStore& store_;
    boardlink::ResultQuery query_{};
    uint32_t retryStartedAt_ = 0;
    BrainPendingRecoveryState state_ = BrainPendingRecoveryState::Idle;
    bool querying_ = false;
    bool waiting_ = false;
    bool clearFault_ = false;
    void (*acceptance_)(const boardlink::ProductRequest&, uint32_t) = nullptr;
};

} }
