#include "ProductEventState.h"
#include "ProductEventCodec.h"

namespace motion {
namespace {
constexpr size_t kDocumentCapacity = 3072;
constexpr size_t kMaxPayload = 2047;

bool encode(JsonDocument& document, std::string& result) {
    if (document.overflowed() || measureJson(document) > kMaxPayload) return false;
    result.clear();
    serializeJson(document, result);
    return !result.empty();
}
}

bool ProductEventState::begin(const std::string& deviceId, const std::string& bootToken) {
    deviceId_ = deviceId;
    bootToken_ = bootToken;
    commandId_.clear();
    eventId_.clear();
    payload_.clear();
    sequence_ = 0;
    journal_ = false;
    persisted_ = false;
    healthy_ = false;
    if (!storage_.read(payload_)) return false;
    if (payload_.empty()) { healthy_ = true; return true; }
    DynamicJsonDocument document(kDocumentCapacity);
    if (payload_.size() > kMaxPayload || deserializeJson(document, payload_) ||
        document["device_id"].as<std::string>() != deviceId_ ||
        !document["event_id"].is<const char*>() ||
        !document["command_id"].is<const char*>() ||
        (document["event"] != "feeding_completed" && document["event"] != "feeding_failed"))
        return false;
    eventId_ = document["event_id"].as<std::string>();
    commandId_ = document["command_id"].as<std::string>();
    if (eventId_.empty() || commandId_.empty()) return false;
    if (document.containsKey("_run_pending")) {
        if (!document["_run_pending"].is<bool>() || !document["_run_pending"].as<bool>() ||
            document["event"] != "feeding_failed" || document["reason"] != "reboot_during_feed")
            return false;
        document.remove("_run_pending");
        if (!encode(document, payload_)) return false;
        // If replacing the journal fails, retry before sending, retaining the
        // same event ID and reboot failure snapshot across further power losses.
        persisted_ = storage_.write(payload_);
    } else {
        persisted_ = true;
    }
    healthy_ = true;
    return true;
}

bool ProductEventState::prepare(ProductRun& run, uint32_t now) {
    if (!ready() || run.commandId.empty()) return false;
    run.eventId = deviceId_ + "-" + bootToken_ + "-" + std::to_string(++sequence_);
    ProductTerminal interrupted{run, false, "reboot_during_feed", "E_REBOOT_DURING_FEED"};
    DynamicJsonDocument document(kDocumentCapacity);
    writeProductTerminalEvent(document.to<JsonObject>(), interrupted,
                              deviceId_.c_str(), run.eventId.c_str(), now);
    document["_run_pending"] = true;
    std::string serialized;
    if (!encode(document, serialized)) return false;
    if (!storage_.write(serialized)) {
        // A failed write may have reached flash. Require a reboot/readback,
        // never overwrite an uncertain accepted-run journal with another run.
        healthy_ = false;
        return false;
    }
    commandId_ = run.commandId;
    eventId_ = run.eventId;
    payload_ = std::move(serialized);
    journal_ = true;
    persisted_ = true;
    return true;
}

bool ProductEventState::queue(const ProductTerminal& terminal, uint32_t now) {
    if (!healthy_ || !journal_ || terminal.run.eventId != eventId_ ||
        terminal.run.commandId != commandId_) return false;
    DynamicJsonDocument document(kDocumentCapacity);
    writeProductTerminalEvent(document.to<JsonObject>(), terminal,
                              deviceId_.c_str(), eventId_.c_str(), now);
    std::string serialized;
    if (!encode(document, serialized)) { healthy_ = false; return false; }
    payload_ = std::move(serialized);
    journal_ = false;
    persisted_ = storage_.write(payload_);
    return persisted_;
}

bool ProductEventState::persistPending() {
    if (!healthy_ || !pending()) return false;
    if (!persisted_) persisted_ = storage_.write(payload_);
    return persisted_;
}

bool ProductEventState::receiveReceipt(JsonVariantConst receipt) {
    if (!healthy_ || !pending() || !persisted_ ||
        receipt["type"] != "feeding_event_receipt" || receipt["status"] != "stored" ||
        receipt["device_id"].as<std::string>() != deviceId_ ||
        receipt["event_id"].as<std::string>() != eventId_) return false;
    if (!storage_.clear()) return false;
    payload_.clear();
    commandId_.clear();
    eventId_.clear();
    persisted_ = false;
    return true;
}

}  // namespace motion
