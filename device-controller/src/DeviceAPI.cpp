#include "DeviceAPI.h"
#include "ProtocolGate.h"

#include <cstring>

namespace motion {
namespace {

DeviceMoveStage moveStage(MotorControl::MoveOutcome outcome) {
    switch (outcome) {
        case MotorControl::MoveOutcome::Running: return DeviceMoveStage::Running;
        case MotorControl::MoveOutcome::Done: return DeviceMoveStage::Reached;
        case MotorControl::MoveOutcome::Cancelled: return DeviceMoveStage::Cancelled;
        case MotorControl::MoveOutcome::Failed: return DeviceMoveStage::Failed;
        default: return DeviceMoveStage::None;
    }
}

DeviceHomeStage homeStage(MotorControl::HomeOutcome outcome) {
    switch (outcome) {
        case MotorControl::HomeOutcome::Running: return DeviceHomeStage::Running;
        case MotorControl::HomeOutcome::Done: return DeviceHomeStage::Reached;
        case MotorControl::HomeOutcome::NoMotion: return DeviceHomeStage::NoMotion;
        case MotorControl::HomeOutcome::Cancelled: return DeviceHomeStage::Cancelled;
        case MotorControl::HomeOutcome::Failed: return DeviceHomeStage::Failed;
        default: return DeviceHomeStage::None;
    }
}

DeviceProgramStage programStage(QueueState state) {
    switch (state) {
        case QueueState::Running: return DeviceProgramStage::Running;
        case QueueState::Done: return DeviceProgramStage::Done;
        case QueueState::Failed: return DeviceProgramStage::Failed;
        case QueueState::Cancelled: return DeviceProgramStage::Cancelled;
        default: return DeviceProgramStage::Idle;
    }
}

template <size_t N>
void copyText(char (&destination)[N], const char* source) {
    if (!source) source = "";
    std::strncpy(destination, source, N - 1);
    destination[N - 1] = '\0';
}

Result submitLiteral(MotorControl& motor, const uint8_t* bytes, uint8_t length) {
    return motor.queueSendLogical(bytes, length)
        ? Result{kCodeQueued, "sent_unverified"}
        : Result{kCodeUnavailable, "can_tx_failed"};
}

bool rawImmediateStopLike(const uint8_t* bytes, uint8_t length) {
    if (!bytes || length < 3 || length > 30 ||
        bytes[length - 1] != kProtocolChecksum) return false;
    if (bytes[1] == kFrameStop)
        return length == 5 && bytes[2] == 0x98 && bytes[3] == 0;
    if (bytes[1] == 0x9C)
        return length == 4 && bytes[2] == 0x48;
    if (bytes[1] == kFrameEnable)
        return length == 6 && bytes[2] == 0xAB && bytes[3] == 0 && bytes[4] == 0;
    return false;
}

} // namespace

DeviceReceipt DeviceAPI::receipt(Result result, uint32_t runId, uint64_t operationId) {
    DeviceAdmission admission = DeviceAdmission::Failed;
    switch (result.code) {
        case 202: admission = DeviceAdmission::Accepted; break;
        case 200: admission = DeviceAdmission::Processed; break;
        case 400: admission = DeviceAdmission::Invalid; break;
        case 409: admission = DeviceAdmission::Busy; break;
        case 503: admission = DeviceAdmission::Unavailable; break;
        default: break;
    }
    return DeviceReceipt{admission, result.code, result.message,
                         admission == DeviceAdmission::Accepted ? runId : 0,
                         admission == DeviceAdmission::Accepted ? operationId : 0};
}

bool DeviceAPI::begin(int tx, int rx, long bitrate) {
    return motor_.begin(tx, rx, bitrate);
}

void DeviceAPI::poll(uint32_t now, bool dispatchQueries) {
    motor_.poll(dispatchQueries);
    queue_.poll(now);
}

DeviceReceipt DeviceAPI::requestEnable(uint8_t id, bool enabled) {
    if (enabled && queue_.unverifiedSyncInFlight())
        return receipt(Result{kCodeBusy, "sync_batch_active"});
    if (!enabled && queue_.unverifiedSyncInFlight()) {
        queue_.cancelLocal("disabled");
        const uint8_t frame[] = {id, kFrameEnable, 0xAB, 0, 0, kProtocolChecksum};
        return receipt(submitLiteral(motor_, frame, sizeof(frame)));
    }
    if (id == 0 && !motor_.unverifiedMode())
        return receipt(Result{kCodeInvalid, "id_reserved"});
    if (!enabled && queue_.active()) {
        if (motor_.unverifiedMode()) queue_.cancelLocal("disabled");
        else (void)queue_.cancel("disabled");
    }
    return receipt(motor_.enable(id, enabled));
}

DeviceReceipt DeviceAPI::requestBroadcastEnable(bool enabled) {
    if (enabled && queue_.unverifiedSyncInFlight())
        return receipt(Result{kCodeBusy, "sync_batch_active"});
    if (!enabled && queue_.unverifiedSyncInFlight()) {
        queue_.cancelLocal("disabled_all");
        const uint8_t frame[] = {0, kFrameEnable, 0xAB, 0, 0, kProtocolChecksum};
        return receipt(submitLiteral(motor_, frame, sizeof(frame)));
    }
    if (!enabled && queue_.active()) {
        if (motor_.unverifiedMode()) queue_.cancelLocal("disabled_all");
        else (void)queue_.cancel("disabled_all");
    }
    return receipt(motor_.broadcastEnable(enabled));
}

DeviceReceipt DeviceAPI::requestMove(const MoveRequest& request) {
    if (queue_.unverifiedSyncInFlight())
        return receipt(Result{kCodeBusy, "sync_batch_active"});
    const Result result = motor_.move(request);
    return receipt(result, 0, motor_.activeOperationId());
}

DeviceReceipt DeviceAPI::requestDirectPosition(const DirectPositionRequest& request) {
    if (queue_.unverifiedSyncInFlight())
        return receipt(Result{kCodeBusy, "sync_batch_active"});
    const Result result = motor_.directPosition(request);
    return receipt(result, 0, motor_.activeOperationId());
}

DeviceReceipt DeviceAPI::requestHome(uint8_t id, uint8_t mode) {
    if (queue_.unverifiedSyncInFlight())
        return receipt(Result{kCodeBusy, "sync_batch_active"});
    const Result result = motor_.home(id, mode);
    return receipt(result, 0, motor_.activeOperationId());
}

DeviceReceipt DeviceAPI::requestStop(uint8_t id) {
    if (queue_.unverifiedSyncInFlight()) {
        queue_.cancelLocal("stopped");
        const uint8_t frame[] = {id, kFrameStop, 0x98, 0, kProtocolChecksum};
        return receipt(submitLiteral(motor_, frame, sizeof(frame)));
    }
    if (id == 0 && !motor_.unverifiedMode())
        return receipt(Result{kCodeInvalid, "id_reserved"});
    if (queue_.active()) {
        if (motor_.unverifiedMode()) queue_.cancelLocal("stopped");
        else (void)queue_.cancel("stopped");
    }
    return receipt(motor_.stop(id));
}

DeviceReceipt DeviceAPI::requestStopAll() {
    if (queue_.unverifiedSyncInFlight()) {
        queue_.cancelLocal("stopped");
        const uint8_t frame[] = {0, kFrameStop, 0x98, 0, kProtocolChecksum};
        return receipt(submitLiteral(motor_, frame, sizeof(frame)));
    }
    // The queue cancellation already issues its abort + broadcast stop. Avoid
    // sending a second FE when this API is called without a source adapter.
    if (motor_.unverifiedMode()) {
        if (queue_.active()) queue_.cancelLocal("stopped");
        return receipt(motor_.stopAll());
    }
    if (queue_.active()) return receipt(queue_.cancel("stopped"));
    return receipt(motor_.stopAll());
}

DeviceReceipt DeviceAPI::requestRawCommand(const uint8_t* bytes, uint8_t length) {
    if (motor_.unverifiedMode()) {
        const bool validShape = bytes && length >= 3 && length <= 30 &&
            bytes[length - 1] == kProtocolChecksum;
        const bool stopLike = rawImmediateStopLike(bytes, length);
        if (validShape && !stopLike && queue_.unverifiedSyncInFlight())
            return receipt(Result{kCodeBusy, "sync_batch_active"});
        if (stopLike && queue_.active())
            queue_.cancelLocal("stopped");
        return receipt(motor_.command(bytes, length));
    }
    if (queue_.unverifiedSyncInFlight()) {
        if (rawImmediateStopLike(bytes, length)) {
            queue_.cancelLocal("stopped");
            return receipt(submitLiteral(motor_, bytes, length));
        }
        const CommandKind kind = validateCommand(bytes, length, motor_.debugLimits());
        if (kind != CommandKind::Invalid)
            return receipt(Result{kCodeBusy, "sync_batch_active"});
    }
    const CommandKind kind = validateCommand(bytes, length, motor_.debugLimits());
    if (kind != CommandKind::Invalid && queue_.active()) {
        const bool stopLike = kind == CommandKind::Stop || kind == CommandKind::Interrupt ||
            (kind == CommandKind::Enable && bytes[3] == 0);
        if (stopLike) (void)queue_.cancel("stopped");
        else if (kind != CommandKind::Read) return receipt(Result{409, "queue_busy"});
    }
    const Result result = motor_.command(bytes, length);
    const bool manualMotion = kind == CommandKind::Move ||
        kind == CommandKind::DirectMove || kind == CommandKind::Home;
    return receipt(result, 0, manualMotion ? motor_.activeOperationId() : 0);
}

bool DeviceAPI::requestDemoMarker(uint8_t id) {
    if (queue_.unverifiedSyncInFlight()) return false;
    const uint8_t command[] = {id, 0x50, 1, 0x6B};
    return motor_.queueSendLogical(command, sizeof(command));
}

DeviceReceipt DeviceAPI::startProgram(const char* text, size_t length, long repeat,
                                       const QueueRotationSource& rotation, uint32_t now) {
    if (queue_.unverifiedSyncInFlight())
        return receipt(Result{kCodeBusy, "sync_batch_active"});
    const Result result = queue_.start(text, length, repeat, rotation, now);
    return receipt(result, queue_.runId());
}

DeviceReceipt DeviceAPI::startDemo(const QueueProgram& program, uint32_t now) {
    if (queue_.unverifiedSyncInFlight())
        return receipt(Result{kCodeBusy, "sync_batch_active"});
    const Result result = queue_.startDemo(program, now);
    return receipt(result, queue_.runId());
}

DeviceReceipt DeviceAPI::cancelProgram(const char* reason) {
    if (queue_.unverifiedSyncInFlight()) {
        const bool wasActive = queue_.cancelLocal(reason);
        const uint8_t frame[] = {0, kFrameStop, 0x98, 0, kProtocolChecksum};
        const Result stopped = submitLiteral(motor_, frame, sizeof(frame));
        if (stopped.code != kCodeQueued) return receipt(stopped);
        return receipt(Result{static_cast<uint16_t>(wasActive ? 202 : 200),
                              wasActive ? "queue_cancelled" : "queue_idle"});
    }
    if (motor_.unverifiedMode()) {
        const bool wasActive = queue_.cancelLocal(reason);
        const Result stopped = motor_.stopAll();
        if (stopped.code != kCodeQueued) return receipt(stopped);
        return receipt(Result{static_cast<uint16_t>(wasActive ? 202 : 200),
                              wasActive ? "queue_cancelled" : "queue_idle"});
    }
    return receipt(queue_.cancel(reason));
}

DeviceReceipt DeviceAPI::clearControlState() {
    return receipt(queue_.clearControlState());
}

DeviceSnapshot DeviceAPI::readSnapshot(uint8_t id) const {
    DeviceSnapshot result;
    result.sampledAtMs = millis();
    result.motorId = id;
    result.busReady = motor_.ready();
    result.unverifiedMode = motor_.unverifiedMode();
    result.unverifiedMotionOutstanding = motor_.unverifiedMotionOutstanding();
    result.manualBusy = motor_.operationBusy();
    result.hasActiveMotion = motor_.hasActiveMotion();
    result.stopping = motor_.stopping();
    result.fault = motor_.hasFault();
    result.manualMove = moveStage(motor_.moveOutcome());
    result.manualHome = homeStage(motor_.homeOutcome());
    result.homeId = motor_.homeId();
    result.program = programStage(queue_.state());
    result.programRunId = queue_.runId();
    result.programErrorLine = queue_.lastErrorLine();
    copyText(result.programMessage, queue_.message());
    copyText(result.faultTag, motor_.faultTag());

    result.motor = readMotorObservation(id);
    return result;
}

DeviceOperationResult DeviceAPI::readOperation(uint64_t operationId) const {
    return motor_.readOperation(operationId);
}

DeviceMotorObservation DeviceAPI::readMotorObservation(uint8_t id) const {
    const MotorControl::Snapshot source = motor_.snapshot(id);
    DeviceMotorObservation observed;
    observed.positionValid = source.positionValid;
    observed.positionTenths = source.position;
    observed.positionAgeMs = source.positionAge;
    observed.velocityValid = source.velocityValid;
    observed.velocityTenthsRpm = source.velocity;
    observed.velocityAgeMs = source.velocityAge;
    observed.currentValid = source.currentValid;
    observed.currentMa = source.current;
    observed.currentAgeMs = source.currentAge;
    observed.enabledConfirmed = source.enabled;
    observed.enablePending = source.enablePending;
    observed.enableProtocolAck = source.enableAck;
    observed.enableTimedOut = source.enableTimedOut;
    observed.stopPending = source.stopPending;
    observed.fault = source.fault;
    return observed;
}

} // namespace motion
