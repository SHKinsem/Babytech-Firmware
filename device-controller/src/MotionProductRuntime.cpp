#include "MotionProductRuntime.h"

#include <cstring>
#include <utility>

namespace motion {
using namespace babytech::boardlink;
namespace v4 = babytech::v4;

bool MotionProductRuntime::contextSynchronized() const {
    if (!store_.ready()) return false;
    const auto& saved = store_.state().context;
    return contextProjected_ && saved.present && observedContextVersion_ == saved.profileVersion &&
        (observedContextVersion_ != saved.profileVersion ||
         !std::memcmp(observedContextDigest_, saved.digest, sizeof(saved.digest))) &&
        product_.context().profileVersion == saved.profileVersion &&
        product_.hasContext() != saved.cleared;
}

bool MotionProductRuntime::context(const ProductContext& context, uint32_t nowMs, ContextResult& result) {
    (void)nowMs;
    if (!validProductContext(context) ||
        !v4::validPairing(store_.state().pairing) ||
        std::strcmp(context.deviceId, store_.state().pairing.deviceId)) return false;
    result = ContextResult{};
    std::strcpy(result.deviceId, context.deviceId);
    result.profileVersion = context.profileVersion;
    result.cleared = context.cleared;
    if (!contextDigest(context, result.digest)) return false;
    if (!store_.ready()) return true;

    const auto& saved = store_.state().context;
    if ((saved.present && (context.profileVersion < saved.profileVersion ||
         (context.profileVersion == saved.profileVersion &&
          std::memcmp(result.digest, saved.digest, sizeof(saved.digest))))) ||
        context.profileVersion < observedContextVersion_ ||
        (context.profileVersion == observedContextVersion_ &&
         std::memcmp(result.digest, observedContextDigest_, sizeof(observedContextDigest_)))) {
        result.status = ContextStatus::Conflict;
        return true;
    }
    if (context.profileVersion > observedContextVersion_) {
        observedContextVersion_ = context.profileVersion;
        std::memcpy(observedContextDigest_, result.digest, sizeof(observedContextDigest_));
    }
    const bool unchanged = saved.present && context.profileVersion == saved.profileVersion;
    // Already-persisted duplicates are read-only, even on an unrelated hardware
    // fault. All writes wait for the existing hardware owner's stationary gate.
    if (!unchanged && (hardware_.unavailable() || !hardware_.stationary() ||
        active() || product_.active() ||
        (flow_.busy() && flow_.stage() != babytech::display::DisplayStage::Complete))) {
        result.status = ContextStatus::Busy;
        return true;
    }
    const auto written = store_.saveContext(context);
    if (written != MotionWrite::Stored && written != MotionWrite::Unchanged) {
        result.status = written == MotionWrite::Conflict ? ContextStatus::Conflict :
            written == MotionWrite::Busy ? ContextStatus::Busy : ContextStatus::StorageFault;
        return true;
    }
    // The Store keeps only the digest/barrier. Rehydrate the complete cache
    // from the verified retry, including an Unchanged reply after reboot.
    if (context.cleared) {
        if (product_.context().profileVersion != context.profileVersion)
            product_.clearContext(context.profileVersion);
    }
    else {
        FeedingContext feeding;
        feeding.babyId = context.babyId;
        feeding.babyName = context.babyName;
        feeding.formulaBrand = context.formulaBrand;
        feeding.profileVersion = context.profileVersion;
        feeding.recipe = {context.waterMl, context.temperatureC, context.powderGPer100Ml};
        if (product_.context().profileVersion != context.profileVersion)
            product_.applyContext(feeding);
    }
    const auto& projected = product_.context();
    contextProjected_ = projected.profileVersion == context.profileVersion &&
        (context.cleared ? !product_.hasContext() :
         projected.babyId == context.babyId && projected.babyName == context.babyName &&
         projected.formulaBrand == context.formulaBrand &&
         projected.recipe.waterMl == context.waterMl &&
         projected.recipe.temperatureC == context.temperatureC &&
         projected.recipe.powderGPer100Ml == context.powderGPer100Ml);
    if (!contextProjected_) {
        result.status = ContextStatus::Conflict;
        return true;
    }
    product_.setContextStorageReady(true);
    result.status = written == MotionWrite::Stored ? ContextStatus::Stored : ContextStatus::Unchanged;
    return true;
}

ProductRun MotionProductRuntime::run(const ProductRequest& request) const {
    ProductRun value;
    value.commandId = request.commandId;
    value.source = request.source == v4::Source::CloudCommand ? "cloud_command" : "local_touch";
    value.babyId = request.babyId;
    value.profileVersion = request.profileVersion;
    value.recipe = {request.waterMl, request.temperatureC, request.powderGPer100Ml};
    return value;
}

bool MotionProductRuntime::matchingDigest(const ProductRequest& request, const char (&text)[65]) {
    uint8_t digest[kProductDigestSize];
    if (!requestDigest(request, digest)) return false;
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); ++i)
        if (text[2 * i] != hex[digest[i] >> 4] || text[2 * i + 1] != hex[digest[i] & 15]) return false;
    return text[64] == 0;
}

bool MotionProductRuntime::command(const CommandMessage& message, uint32_t nowMs, CommandResult& result) {
    const auto& request = message.request;
    if (!validProductRequest(request)) return false;
    result = CommandResult{};
    result.source = request.source;
    result.sequence = request.sequence;
    std::strcpy(result.commandId, request.commandId);
    const auto reject = [&](const char* reason) { std::strcpy(result.reason, reason); return true; };
    if (!store_.ready()) return reject("storage_fault");
    const auto& state = store_.state();
    if (std::strcmp(request.deviceId, state.pairing.deviceId)) return false;
    if (request.source == v4::Source::LocalTouch) {
        auto brain = state.pairing;
        brain.role = v4::Role::Brain;
        std::swap(brain.localPhysicalId, brain.peerPhysicalId);
        char expected[129];
        if (!makeLocalCommandId(brain, request.sequence, expected) ||
            std::strcmp(expected, request.commandId)) return reject("invalid_identity");
    }
    ResultQuery query;
    query.source = request.source; query.sequence = request.sequence;
    std::strcpy(query.deviceId, request.deviceId); std::strcpy(query.commandId, request.commandId);
    if (!queryMotionResult(store_, query, queried_)) return false;
    if (queried_.status == ResultQueryStatus::Known) {
        if (!matchingDigest(request, queried_.requestDigestHex)) return reject("request_conflict");
        result.accepted = queried_.accepted;
        std::strcpy(result.reason, queried_.reason);
        return true; // Original decision, never permission to replay.
    }
    if (queried_.status != ResultQueryStatus::Unknown) return reject(queried_.reason);
    if (deferred_) return false; // One bounded in-flight decision, never an action queue.
    if (!message.remainingTtlMs || uint32_t(hardware_.nowMs() - nowMs) >= message.remainingTtlMs)
        return reject("request_expired");

    const char* reason = cloudStopPending_ ? "busy" : hardware_.unavailable();
    ProductRun prepared;
    if (!reason && (active() || state.slot.kind != MotionSlotKind::Empty ||
                    flow_.busy() || product_.ownsMotion())) reason = "busy";
    if (!reason && workbenchExecution_[0] &&
        (hardware_.workbenchBusy() || !hardware_.workbenchStationary())) reason = "busy";
    if (!reason) switch (request.command) {
        case ProductCommand::Prepare:
            if (!contextSynchronized()) reason = "context_required";
            else if (state.pendingResultCount == kMotionResultQueueCapacity) reason = "result_queue_full";
            else {
                prepared = run(request);
                reason = product_.prepareRejection(prepared);
            }
            break;
        case ProductCommand::Initialize: reason = product_.initializeRejection(); break;
        case ProductCommand::Clean: reason = product_.cleanRejection(); break;
        case ProductCommand::SetTargetTemp: break;
        case ProductCommand::ResetError:
            if (std::strcmp(product_.progress(), "error") == 0) reason = "manual_initialization_required";
            break;
        case ProductCommand::CheckFirmwareUpdate: reason = "cloud_ota_not_supported"; break;
        default: return false;
    }
    const bool motion = request.command == ProductCommand::Prepare ||
        request.command == ProductCommand::Initialize || request.command == ProductCommand::Clean;
    char execution[33]{};
    if (!reason && motion && (!hardware_.newExecution(execution) || !validExecutionId(execution)))
        reason = "execution_id_unavailable";
    const char* acceptedReason = request.command == ProductCommand::ResetError ? "already_clear" : "accepted";
    // The final pre-write check includes decoding/preflight/ID generation time.
    if (uint32_t(hardware_.nowMs() - nowMs) >= message.remainingTtlMs) return reject("request_expired");
    if (reason && !hardware_.stationary() &&
        (active() || flow_.busy() || product_.ownsMotion() || !std::strcmp(reason, "busy"))) {
        deferredRequest_ = request;
        std::strcpy(result.reason, reason);
        deferredResult_ = result;
        std::strcpy(deferredReason_, result.reason);
        deferred_ = true;
        deferredReply_ = true;
        return true; // Link holds this reply; no final decision or Flash write yet.
    }
    auto written = store_.recordDecision(request, !reason, reason ? reason : acceptedReason,
                                         !reason && motion ? execution : nullptr);
    // These mean no acceptance was stored: consume a final rejection instead,
    // so a later resource/context change cannot execute the same request.
    if (written == MotionWrite::ContextRequired || written == MotionWrite::QueueFull || written == MotionWrite::Busy) {
        reason = written == MotionWrite::ContextRequired ? "context_required" :
                 written == MotionWrite::QueueFull ? "result_queue_full" : "busy";
        written = store_.recordDecision(request, false, reason);
    }
    if (written != MotionWrite::Stored)
        return reject(written == MotionWrite::Invalid ? "invalid_identity" : "storage_fault");
    result.accepted = !reason;
    std::strcpy(result.reason, reason ? reason : acceptedReason);
    if (reason) return true;

    if (motion) {
        std::strcpy(execution_, execution);
        operation_ = request.command;
        operationFailed_ = stopped_ = terminalSeen_ = false;
        stopDelivered_ = false;
        motionOwned_ = true;
        workbenchExecution_[0] = 0; // An older workbench Stop cannot reach this product generation.
    }
    if (uint32_t(hardware_.nowMs() - nowMs) >= message.remainingTtlMs) {
        if (motion) finishFailed("request_expired", hardware_.nowMs());
        return true; // Original durable acceptance remains; no expired side effect.
    }
    switch (request.command) {
        case ProductCommand::Prepare: {
            prepared.eventId = store_.state().slot.eventId;
            const char* failure = nullptr;
            if (!product_.startPaired(std::move(prepared), hardware_.nowMs(), failure))
                finishFailed(failure ? failure : "not_ready", hardware_.nowMs());
            else product_.setTargetTemp(request.temperatureC);
            break;
        }
        case ProductCommand::Initialize:
            if (!product_.initialize(hardware_.nowMs())) finishFailed("not_ready", hardware_.nowMs());
            break;
        case ProductCommand::Clean: {
            const char* failure = nullptr;
            if (!product_.clean(hardware_.nowMs(), failure))
                finishFailed(failure ? failure : "not_ready", hardware_.nowMs());
            break;
        }
        case ProductCommand::SetTargetTemp: product_.setTargetTemp(request.temperatureC); break;
        default: break;
    }
    return true;
}

bool MotionProductRuntime::resultReady(CommandResult& result) {
    if (!deferredReply_ || result.accepted || std::strcmp(result.reason, deferredReason_) ||
        result.source != deferredResult_.source || result.sequence != deferredResult_.sequence ||
        std::strcmp(result.commandId, deferredResult_.commandId)) return true;
    if (deferred_) return false;
    result = deferredResult_;
    deferredReply_ = false;
    return true;
}

void MotionProductRuntime::finishFailed(const char* reason, uint32_t nowMs) {
    operationFailed_ = true;
    if (operation_ == ProductCommand::Prepare && !terminalSeen_) {
        // A failed start may already have emitted a terminal in ProductSession.
        // Drain it now rather than letting the next bottle consume stale state.
        product_.takeTerminal(terminal_);
        terminal_.completed = false;
        terminal_.reason = reason;
        terminal_.errorCode = "E_MOTION_FAULT";
        terminalSeen_ = true;
        terminalAt_ = nowMs;
    }
}

bool MotionProductRuntime::workbenchAccepted() {
    char next[33]{};
    if (!hardware_.newExecution(next) || !validExecutionId(next)) return false;
    std::strcpy(workbenchExecution_, next);
    releaseMotionOwnership();
    return true;
}

bool MotionProductRuntime::stop(const v4::StopRequest& request, uint32_t nowMs) {
    uint8_t validated[155];
    if (!v4::encodeStop(request, validated, sizeof(validated)) ||
        request.commandId[request.commandIdLength] != 0) return false;
    char target[33]{};
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 16; ++i) {
        target[2 * i] = hex[request.executionId[i] >> 4];
        target[2 * i + 1] = hex[request.executionId[i] & 15];
    }
    const bool idle = request.scope == v4::StopScope::Idle;
    if (idle) {
        if (motionOwned_ || workbenchExecution_[0] || product_.ownsMotion() ||
            flow_.busy() || !hardware_.stationary()) return false;
    } else if (request.scope == v4::StopScope::Workbench) {
        if (!workbenchExecution_[0] || std::strcmp(target, workbenchExecution_)) return false;
        hardware_.stopWorkbench(nowMs); // No Flash or product event before Stop.
    } else {
        if (request.scope != v4::StopScope::Product) return false;
        if (motionOwned_ && active() && !std::strcmp(target, execution_)) stopOwned(nowMs);
        else if (hardware_.recoveringExecutionId()[0] &&
                 !std::strcmp(target, hardware_.recoveringExecutionId())) hardware_.stopRecovery(nowMs);
        else return false;
    }
    if (request.source == v4::Source::CloudCommand &&
        (!cloudStopPending_ || request.sequence > cloudStop_.sequence)) {
        cloudStop_ = request;
        cloudStopPending_ = true;
    }
    return true; // Received/matched, never stationary confirmation.
}

bool MotionProductRuntime::stopOwned(uint32_t nowMs) {
    if (!motionOwned_ || !active()) return false;
    if (!stopped_ || !stopDelivered_ || !std::strcmp(flow_.reason(), "stop_unconfirmed")) {
        const bool retry = stopped_;
        stopped_ = true;
        bool wasActive = false;
        if (retry) flow_.stop(nowMs);
        else {
            product_.stop(nowMs, wasActive); // No Flash precedes immediate Stop.
            if (std::strcmp(flow_.reason(), "stop_requested") &&
                std::strcmp(flow_.reason(), "stop_unconfirmed")) flow_.stop(nowMs);
        }
        stopDelivered_ = !std::strcmp(flow_.reason(), "stop_requested");
    }
    return stopDelivered_;
}

void MotionProductRuntime::linkLost(uint32_t nowMs) {
    if (!active() || !motionOwned_ || stopped_) return;
    stopped_ = true;
    if (operation_ == ProductCommand::Prepare && product_.active())
        product_.abort(nowMs, "link_lost", "E_LINK_LOST");
    else {
        bool wasActive = false;
        product_.stop(nowMs, wasActive);
        if (std::strcmp(flow_.reason(), "stop_requested") &&
            std::strcmp(flow_.reason(), "stop_unconfirmed")) flow_.stop(nowMs);
    }
    stopDelivered_ = !std::strcmp(flow_.reason(), "stop_requested");
}

void MotionProductRuntime::poll(uint32_t nowMs) {
    if (workbenchExecution_[0] && !hardware_.workbenchBusy() && hardware_.workbenchStationary())
        workbenchExecution_[0] = 0;
    if (active() && operation_ == ProductCommand::Prepare && !terminalSeen_) {
        if (product_.takeTerminal(terminal_)) {
            terminalSeen_ = true;
            terminalAt_ = nowMs;
        }
    }
    // V4 keeps results in the durable queue, not the legacy RAM event gate.
    if (terminalSeen_) product_.setEventPending(false);
    // Mechanical ownership ends independently of successful archival. A retained
    // journal is evidence, not permission to stop a later independent workbench.
    if (motionOwned_ && hardware_.stationary() && !product_.active() &&
        (terminalSeen_ || stopped_ || operationFailed_ || flow_.stage() == babytech::display::DisplayStage::Error ||
         (operation_ == ProductCommand::Clean ? product_.cleaning() : !flow_.busy())))
        motionOwned_ = false;
    if (!store_.ready()) {
        if (deferred_) {
            std::strcpy(deferredResult_.reason, "storage_fault");
            deferred_ = false;
        }
        return;
    }
    archiveExecution();
    if (!store_.ready()) return;
    const bool stopStationary = cloudStop_.scope == v4::StopScope::Workbench ?
        !motionOwned_ && !hardware_.workbenchBusy() && hardware_.workbenchStationary() : hardware_.stationary();
    if (cloudStopPending_ && stopStationary) {
        char target[33]{};
        if (cloudStop_.scope != v4::StopScope::Idle) {
            constexpr char hex[] = "0123456789abcdef";
            for (size_t i = 0; i < 16; ++i) {
                target[2 * i] = hex[cloudStop_.executionId[i] >> 4];
                target[2 * i + 1] = hex[cloudStop_.executionId[i] & 15];
            }
        }
        const auto written = store_.recordCloudStop(cloudStop_.sequence, cloudStop_.commandId, target,
            true, cloudStop_.scope == v4::StopScope::Idle ? "already_idle" : "accepted", true);
        if (written == MotionWrite::Stored || written == MotionWrite::Unchanged ||
            written == MotionWrite::Expired || written == MotionWrite::Conflict) cloudStopPending_ = false;
    }
    if (deferred_ && hardware_.stationary()) {
        const auto written = store_.recordDecision(deferredRequest_, false, deferredResult_.reason);
        if (written == MotionWrite::Stored || written == MotionWrite::Unchanged) deferred_ = false;
        else if (written == MotionWrite::Expired || written == MotionWrite::Conflict ||
                 written == MotionWrite::StorageFault) {
            std::strcpy(deferredResult_.reason, written == MotionWrite::Expired ? "result_expired" :
                        written == MotionWrite::Conflict ? "request_conflict" : "storage_fault");
            deferred_ = false;
        }
    }
}

void MotionProductRuntime::archiveExecution() {
    if (active()) {
        if (operation_ == ProductCommand::Prepare && terminalSeen_) {
            if (!hardware_.stationary()) return;
            auto written = store_.finishFeeding(execution_, terminal_.completed,
                terminal_.reason.c_str(), terminal_.errorCode.c_str(), terminalAt_);
            if (written != MotionWrite::Stored && written != MotionWrite::Unchanged) return;
            if (!hardware_.stationary()) return; // Flash may have aged the feedback.
            written = store_.archiveFeeding(true);
            if (written != MotionWrite::Stored && written != MotionWrite::Unchanged) return;
            execution_[0] = 0; terminalSeen_ = false;
        } else if (operation_ != ProductCommand::Prepare && hardware_.stationary() &&
                   (stopped_ || operationFailed_ || flow_.stage() == babytech::display::DisplayStage::Error ||
                    (operation_ == ProductCommand::Clean ? product_.cleaning() : !flow_.busy()))) {
            const auto outcome = operationFailed_ || flow_.stage() == babytech::display::DisplayStage::Error ?
                MotionOutcome::Failed : stopped_ ? MotionOutcome::Interrupted : MotionOutcome::Succeeded;
            const auto written = store_.finishOperation(execution_, outcome, true);
            if (written == MotionWrite::Stored || written == MotionWrite::Unchanged) {
                execution_[0] = 0;
                motionOwned_ = false;
            }
        }
    }
}

void MotionProductRuntime::project(Status& status, bool linkConnected, bool recoveringMotion) const {
    status.snapshot.temperatureC = product_.active()
        ? product_.activeRun().recipe.temperatureC : product_.targetTemp();
    status.executionAuthorized = product_.executionAuthorized() && store_.ready() && linkConnected &&
        !cloudStopPending_ && !deferred_ && !hardware_.unavailable();
    status.snapshot.startEnabled = status.executionAuthorized && !active() && product_.canStart() &&
        contextSynchronized() &&
        store_.state().context.present && !store_.state().context.cleared &&
        store_.state().pendingResultCount < kMotionResultQueueCapacity;
    status.activeExecutionId[0] = 0;
    status.executionOwner = ExecutionOwner::None;
    if (active() && motionOwned_) {
        std::strcpy(status.activeExecutionId, execution_);
        status.executionOwner = ExecutionOwner::Product;
    } else if (recoveringMotion && store_.ready() && store_.state().slot.kind != MotionSlotKind::Empty) {
        std::strcpy(status.activeExecutionId, store_.state().slot.executionId);
        status.executionOwner = ExecutionOwner::Product;
    } else if (workbenchExecution_[0]) {
        std::strcpy(status.activeExecutionId, workbenchExecution_);
        status.executionOwner = ExecutionOwner::Workbench;
        status.snapshot.startEnabled = false;
    }
}

} // namespace motion
