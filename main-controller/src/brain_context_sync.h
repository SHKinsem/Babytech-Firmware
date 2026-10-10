#pragma once

#include "BrainStateStore.h"
#include "ProductContextMessages.h"
#include "ReadOnlyBoardLink.h"

#include <cstring>

namespace babytech { namespace brain {

// UI-loop owner of the same persisted cache and UART used by installation and
// dispatch. The network callback only copies; Flash runs after urgent Stop I/O.
template<class Link>
class BrainContextSync {
public:
    BrainContextSync(Link& link, boardlink::BrainStateStore& store)
        : link_(link), store_(store) {}
    BrainContextSync(const BrainContextSync&) = delete;
    BrainContextSync& operator=(const BrainContextSync&) = delete;

    bool receive(const boardlink::ProductContext& context) {
        if (!store_.ready() || !boardlink::validProductContext(context) ||
            std::strcmp(context.deviceId, store_.state().pairing.deviceId)) return false;
        const auto& state = store_.state();
        const auto* latest = received_ ? &incoming_ : state.hasContext ? &state.context : nullptr;
        if (latest && context.profileVersion < latest->profileVersion) return false;
        if (latest && context.profileVersion == latest->profileVersion) {
            if (boardlink::sameProductContext(context, *latest)) return true;
            conflictVersion_ = context.profileVersion;
            return false;
        }
        incoming_ = context;
        received_ = true;
        return true;
    }

    bool canPrepare() const {
        return !received_ && store_.ready() && store_.state().hasContext &&
            !store_.state().context.cleared && confirmed_ &&
            store_.state().context.profileVersion > conflictVersion_ &&
            boardlink::sameProductContext(sent_, store_.state().context) && proofCurrent();
    }

    bool hasUsableCache() const {
        return !received_ && store_.ready() && store_.state().hasContext &&
            !store_.state().context.cleared &&
            store_.state().context.profileVersion > conflictVersion_;
    }

    // Explicit non-Prepare operations may preempt this idempotent transfer.
    // This never erases a cache or rolls back a possibly committed Motion write.
    void yield(uint32_t nowMs) {
        if (!sending_) return;
        link_.cancelContext();
        sending_ = false;
        confirmed_ = false;
        wait(nowMs);
    }

    void poll(uint32_t nowMs, bool maintenance = false, bool ordinaryBusy = false,
              bool forwardToMotion = true) {
        if (maintenance) { yield(nowMs); return; }
        // A bounded UART pump can still be backpressured: do not start Flash
        // while its safety-control frame/receipt is outstanding.
        if (link_.stopSendState() == boardlink::StopSendState::Pending) return;
        if (received_) {
            const auto written = store_.saveContext(incoming_);
            if (written == boardlink::BrainWrite::StorageFault) return;
            received_ = false;
            if (written == boardlink::BrainWrite::Stored) {
                yield(nowMs);
                confirmed_ = false;
                waiting_ = false;
            }
        }
        // Simulation shares the actual cache, but never forwards simulated
        // configuration or produces a fabricated Motion persistence proof.
        if (!forwardToMotion) { yield(nowMs); return; }
        if (!store_.ready() || !store_.state().hasContext) { yield(nowMs); return; }
        const auto& context = store_.state().context;
        if (context.profileVersion <= conflictVersion_) { yield(nowMs); return; }
        if ((sending_ || confirmed_) && !boardlink::sameProductContext(sent_, context)) {
            yield(nowMs);
            confirmed_ = false;
            waiting_ = false;
        }
        if (sending_) {
            const auto state = link_.contextSendState();
            if (state == boardlink::ContextSendState::Pending) return;
            sending_ = false;
            if (state == boardlink::ContextSendState::Complete &&
                boardlink::matchesContextResult(link_.contextResponse(), sent_) &&
                persisted(link_.contextResponse().status)) {
                proof_ = link_.contextResponse();
                confirmed_ = true;
                conflictVersion_ = 0;
                waiting_ = false;
                return;
            }
            confirmed_ = false;
            wait(nowMs);
        }
        if (confirmed_) {
            if (proofCurrent()) return;
            confirmed_ = false;
            waiting_ = false; // New UART boot/session: rehydrate the saved cache.
        }
        if (ordinaryBusy || !link_.connected(nowMs) ||
            (waiting_ && uint32_t(nowMs - retryAtMs_) < kRetryMs)) return;
        if (!link_.commandAvailable(nowMs)) return;
        if (!link_.requestContext(context, nowMs)) { wait(nowMs); return; }
        sent_ = context;
        sending_ = true;
        waiting_ = false;
    }

private:
    static constexpr uint32_t kRetryMs = 1000;
    static bool persisted(boardlink::ContextStatus status) {
        return status == boardlink::ContextStatus::Stored || status == boardlink::ContextStatus::Unchanged;
    }
    bool proofCurrent() const {
        const auto& result = link_.contextResponse();
        return link_.contextSendState() == boardlink::ContextSendState::Complete &&
            persisted(result.status) && result.replyTo == proof_.replyTo &&
            result.profileVersion == proof_.profileVersion && result.cleared == proof_.cleared &&
            !std::strcmp(result.deviceId, proof_.deviceId) &&
            !std::memcmp(result.digest, proof_.digest, sizeof(proof_.digest));
    }
    void wait(uint32_t nowMs) { waiting_ = true; retryAtMs_ = nowMs; }
    Link& link_;
    boardlink::BrainStateStore& store_;
    boardlink::ProductContext incoming_{};
    boardlink::ProductContext sent_{};
    boardlink::ContextResult proof_{};
    uint32_t retryAtMs_ = 0;
    uint32_t conflictVersion_ = 0;
    bool received_ = false;
    bool sending_ = false;
    bool confirmed_ = false;
    bool waiting_ = false;
};

} }
