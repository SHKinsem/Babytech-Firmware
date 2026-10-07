#pragma once

#ifdef ARDUINO
#include <Arduino.h>
#include <driver/uart.h>
#include "ReadOnlyBoardLink.h"
#include "BoardPairingStore.h"
#include "BoardDiscovery.h"
#include "BoardExportTransfer.h"
#include "BoardMaintenance.h"
#include "BoardInstall.h"

namespace babytech { namespace boardlink {

// One instance owns UART1; use static storage and call only from the loop task.
class ArduinoBoardLink {
public:
    bool begin(v4::Role role, int rxPin, int txPin, uint32_t baud = 115200,
               bool enableDiscovery = false);
    void poll(uint32_t nowMs, const Status* localStatus = nullptr);
    const ReadOnlyLink& link() const { return link_; }
    PairingLoad pairingState() const { return pairingState_; }
    const char* deviceId() const { return deviceId_; }
    // Local recovery must still run if the separately initialized UART fails.
    const v4::Pairing* verifiedPairing() const { return pairingVerified_ ? &pairing_ : nullptr; }
    bool requestDiscovery(const char* deviceId, uint32_t nowMs) {
        return started_ && discoveryEnabled_ && discovery_.request(deviceId, nowMs);
    }
    bool setResultQueryHandler(ReadOnlyLink::ResultQueryHandler handler) {
        return started_ && link_.setResultQueryHandler(handler);
    }
    bool setCommandHandler(ReadOnlyLink::CommandHandler handler) {
        return started_ && link_.setCommandHandler(handler);
    }
    bool setStopHandler(ReadOnlyLink::StopHandler handler) {
        return started_ && link_.setStopHandler(handler);
    }
    bool setCommandReadyHandler(ReadOnlyLink::CommandReadyHandler handler) {
        return started_ && link_.setCommandReadyHandler(handler);
    }
    bool setContextHandler(ReadOnlyLink::ContextHandler handler) {
        return started_ && link_.setContextHandler(handler);
    }
    bool requestContext(const ProductContext& context, uint32_t nowMs) {
        return started_ && link_.requestContext(context, nowMs);
    }
    ContextSendState contextSendState() const { return link_.contextSendState(); }
    const ContextResult& contextResponse() const { return link_.contextResponse(); }
    void cancelContext() { link_.cancelContext(); }
    v4::LinkFailure takeFailure() { return link_.takeFailure(); }
    bool requestCommand(const CommandMessage& command, uint32_t nowMs) {
        return started_ && link_.requestCommand(command, nowMs);
    }
    bool commandAvailable(uint32_t nowMs) const { return started_ && link_.commandAvailable(nowMs); }
    CommandSendState commandSendState() const { return link_.commandSendState(); }
    const CommandResult& commandResponse() const { return link_.commandResponse(); }
    void cancelCommand() { link_.cancelCommand(); }
    bool requestStop(const v4::StopRequest& request, uint32_t nowMs) {
        return started_ && link_.requestStop(request, nowMs);
    }
    StopSendState stopSendState() const { return link_.stopSendState(); }
    bool requestResult(const ResultQuery& query, uint32_t nowMs) {
        return started_ && link_.requestResult(query, nowMs);
    }
    ResultLookupState resultLookupState() const { return link_.resultLookupState(); }
    const QueriedResult& resultQueryResponse() const { return link_.resultQueryResponse(); }
    void cancelResultQuery() { link_.cancelResultQuery(); }
    const DiscoveryResult& discoveryResult() const { return discovery_.result(); }
    bool requestRecords(const char* deviceId, uint32_t nowMs);
    bool requestInstalledRecords(const char* deviceId, const v4::Pairing& expected, uint32_t nowMs);
    bool requestRecoveryRecords(const char* deviceId, const v4::Pairing& expected, uint32_t nowMs);
    bool setExportSource(BoardExportSource* source) { return records_.setSource(source); }
    ExportTransferState recordsState() const { return records_.state(); }
    const MotionExportSnapshot* recordsSnapshot() const { return records_.snapshot(); }
    bool requestMaintenance(const char* device, uint32_t nowMs);
    bool releaseMaintenance(uint32_t nowMs) { return started_ && maintenance_.release(nowMs); }
    bool setMaintenanceTarget(BoardMaintenanceTarget* target) { return maintenance_.setTarget(target); }
    BoardMaintenanceState maintenanceState() const { return maintenance_.state(); }
    bool maintenanceActive() const { return maintenance_.active(); }
    BoardMaintenance& maintenance() { return maintenance_; }
    const BoardMaintenance& maintenance() const { return maintenance_; }
    BoardInstall& install() { return install_; }
    const BoardInstall& install() const { return install_; }
private:
    class Sink : public v4::ByteSink {
    public:
        explicit Sink(HardwareSerial& serial) : serial_(serial) {}
        bool idle() const override;
        size_t available() const override;
        size_t write(const uint8_t* bytes, size_t length) override;
    private:
        HardwareSerial& serial_;
    };
    HardwareSerial serial_{1};
    Sink sink_{serial_};
    ReadOnlyLink link_{};
    v4::Parser parser_{};
    BoardDiscovery discovery_{};
    BoardExportTransfer records_{};
    BoardMaintenance maintenance_{};
    BoardInstall install_{};
    PairingLoad pairingState_ = PairingLoad::Missing;
    v4::Pairing pairing_{};
    char deviceId_[65]{};
    bool pairingVerified_ = false;
    bool started_ = false;
    bool discoveryEnabled_ = false;
};

} }
#endif
