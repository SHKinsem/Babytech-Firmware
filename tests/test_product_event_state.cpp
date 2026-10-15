#include "ProductEventState.h"
#include "CloudConfigKind.h"
#include <cassert>
#include <iostream>

using namespace motion;

struct MemoryStorage : ProductEventStorage {
    std::string value;
    bool readOk = true, writeOk = true, clearOk = true;
    int writes = 0;
    bool read(std::string& out) override { out = value; return readOk; }
    bool write(const std::string& in) override {
        ++writes;
        if (!writeOk) return false;
        value = in;
        return true;
    }
    bool clear() override { if (!clearOk) return false; value.clear(); return true; }
};

ProductRun sampleRun() {
    ProductRun run;
    run.commandId = "command-1";
    run.babyId = "baby-1";
    run.source = "local_touch";
    run.profileVersion = 7;
    run.recipe = {180, 45, 25.0f};
    run.targetPowderG = 45.0f;
    return run;
}

DynamicJsonDocument receipt(const ProductRun& run) {
    DynamicJsonDocument result(512);
    result["type"] = "feeding_event_receipt";
    result["device_id"] = "bt-test";
    result["event_id"] = run.eventId;
    result["status"] = "stored";
    return result;
}

int main() {
    MemoryStorage storage;
    ProductEventState state(storage);
    assert(state.begin("bt-test", "boot1") && state.ready());
    auto run = sampleRun();
    assert(state.prepare(run, 100));
    assert(!run.eventId.empty() && !state.ready() && !state.pending());
    auto ack = receipt(run);
    assert(!state.receiveReceipt(ack.as<JsonVariantConst>())); // Journal is not a terminal.
    const auto journal = storage.value;
    assert(journal.find("_run_pending") != std::string::npos);
    assert(!state.prepare(run, 101));
    // Power loss before a terminal yields one failed event, never resumes motors.
    ProductEventState reboot(storage);
    assert(reboot.begin("bt-test", "boot2") && reboot.pending() && !reboot.ready());
    DynamicJsonDocument parsed(3072);
    assert(!deserializeJson(parsed, reboot.payload()));
    assert(parsed["reason"] == "reboot_during_feed");
    assert(parsed["event_id"].as<std::string>() == run.eventId);
    assert(parsed["baby_id"] == "baby-1" && parsed["feeding_context_profile_version"] == 7);
    assert(!parsed.containsKey("_run_pending"));
    const auto recovered = reboot.payload();
    ProductEventState again(storage);
    assert(again.begin("bt-test", "boot3") && again.payload() == recovered);
    ack["device_id"] = "other";
    assert(!again.receiveReceipt(ack.as<JsonVariantConst>()));
    ack["device_id"] = "bt-test";
    ack["event_id"] = "other";
    assert(!again.receiveReceipt(ack.as<JsonVariantConst>()));
    ack["event_id"] = run.eventId;
    ack["status"] = "accepted";
    assert(!again.receiveReceipt(ack.as<JsonVariantConst>()));
    ack["status"] = "stored";
    storage.clearOk = false;
    assert(!again.receiveReceipt(ack.as<JsonVariantConst>()) && again.pending());
    storage.clearOk = true;
    assert(again.receiveReceipt(ack.as<JsonVariantConst>()) && again.ready());
    assert(storage.value.empty());
    assert(!again.receiveReceipt(ack.as<JsonVariantConst>()));

    // Persisted terminal takes precedence over reboot recovery and replays byte-for-byte.
    assert(again.prepare(run, 200));
    ProductTerminal terminal{run, true, "", ""};
    assert(again.queue(terminal, 250));
    const auto completed = again.payload();
    ProductEventState afterTerminal(storage);
    assert(afterTerminal.begin("bt-test", "boot4"));
    assert(afterTerminal.payload() == completed);
    assert(!deserializeJson(parsed, completed) && parsed["event"] == "feeding_completed");
    ack = receipt(run);
    assert(afterTerminal.receiveReceipt(ack.as<JsonVariantConst>()));

    // A terminal write failure keeps RAM truth and retries flash without a network.
    assert(afterTerminal.prepare(run, 300));
    terminal.run = run;
    storage.writeOk = false;
    assert(!afterTerminal.queue(terminal, 350));
    assert(afterTerminal.pending() && !afterTerminal.persisted());
    assert(!afterTerminal.persistPending());
    ack = receipt(run);
    assert(!afterTerminal.receiveReceipt(ack.as<JsonVariantConst>()));
    storage.writeOk = true;
    assert(afterTerminal.persistPending());
    assert(afterTerminal.receiveReceipt(ack.as<JsonVariantConst>()));

    // Failed recovery write retains the same reboot event through another power cut.
    storage.value = journal;
    storage.writeOk = false;
    ProductEventState failedRecovery(storage);
    assert(failedRecovery.begin("bt-test", "boot5"));
    assert(failedRecovery.pending() && !failedRecovery.persisted());
    assert(failedRecovery.payload() == recovered);
    storage.writeOk = true;
    ProductEventState retryRecovery(storage);
    assert(retryRecovery.begin("bt-test", "boot6") && retryRecovery.payload() == recovered);

    // A bad read or payload is never silently discarded to allow another start.
    storage.readOk = false;
    ProductEventState unreadable(storage);
    assert(!unreadable.begin("bt-test", "boot7") && unreadable.blocked());
    assert(!unreadable.prepare(run, 400));
    storage.readOk = true;
    storage.value = "{broken";
    assert(!unreadable.begin("bt-test", "boot8") && unreadable.blocked());
    assert(storage.value == "{broken");
    storage.value = completed;
    assert(!unreadable.begin("wrong-device", "boot9") && unreadable.blocked());
    assert(storage.value == completed);
    storage.value.clear();
    assert(unreadable.begin("bt-test", "boot10"));
    storage.writeOk = false;
    assert(!unreadable.prepare(run, 500) && !unreadable.ready());

    // Config routing prevents receipts from overwriting the retained context lane.
    const std::string context = "{\"type\":\"feeding_context\",\"device_id\":\"bt-test\"}";
    std::string receiptJson;
    serializeJson(receipt(run), receiptJson);
    assert(cloudConfigKind(reinterpret_cast<const uint8_t*>(context.data()), context.size(),
                           "bt-test") == CloudConfigKind::Context);
    assert(cloudConfigKind(reinterpret_cast<const uint8_t*>(receiptJson.data()), receiptJson.size(),
                           "bt-test") == CloudConfigKind::Receipt);
    assert(cloudConfigKind(reinterpret_cast<const uint8_t*>(receiptJson.data()), receiptJson.size(),
                           "wrong") == CloudConfigKind::Invalid);
    std::cout << "PASS durable journal, terminal replay, receipt and storage faults\n";
}
