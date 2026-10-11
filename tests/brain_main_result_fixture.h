// Host-only combination: production Brain main and real Motion link/Store/
// delivery owner. Archived terminal fixtures stand in for earlier execution;
// neither Motion Arduino main nor motors nor Cloud transactions run here.
#pragma once
#include <set>

namespace {
struct ObservedResultLink : ReadOnlyLink {
    std::map<std::string, unsigned> admitted;
    bool publishTerminal(const TerminalEvent& event, uint32_t now) {
        const bool accepted = ReadOnlyLink::publishTerminal(event, now);
        if (accepted) ++admitted[event.eventId];
        return accepted;
    }
};
class ResultMotionPeer : public v4::ByteSink {
public:
    ResultMotionPeer() {
        pair.role = v4::Role::Motion;
        std::strcpy(pair.deviceId, device);
        std::strcpy(pair.epoch, "0123456789abcdef0123456789abcdef");
        std::strcpy(pair.localPhysicalId, "aabbccddeeff");
        std::strcpy(pair.peerPhysicalId, "112233445566");
        store = std::make_unique<MotionStateStore>();
        const auto cached = initialContext();
        check(store->installInitial(pair, &cached) == MotionWrite::Stored, "cannot seed Motion Store");
        auto brainPair = pair;
        brainPair.role = v4::Role::Brain;
        std::swap(brainPair.localPhysicalId, brainPair.peerPhysicalId);
        for (unsigned i = 1; i <= kMotionResultQueueCapacity; ++i) {
            ProductRequest request;
            request.source = i % 2 ? v4::Source::CloudCommand : v4::Source::LocalTouch;
            request.command = ProductCommand::Prepare; request.sequence = i;
            std::strcpy(request.deviceId, device); std::strcpy(request.babyId, cached.babyId);
            request.profileVersion = 1; request.waterMl = 180;
            request.temperatureC = 45; request.powderGPer100Ml = 25;
            if (request.source == v4::Source::LocalTouch)
                check(makeLocalCommandId(brainPair, i, request.commandId), "local fixture ID failed");
            else std::snprintf(request.commandId, sizeof(request.commandId), "history-%u", i);
            char execution[33];
            std::snprintf(execution, sizeof(execution), "%032x", i);
            check(store->recordDecision(request, true, "accepted", execution) == MotionWrite::Stored,
                  "cannot reserve historical result");
            const bool completed = i % 2;
            check(store->finishFeeding(execution, completed, completed ? "" : "motor_fault",
                      completed ? "" : "E_MOTOR", 100 + i) == MotionWrite::Stored &&
                  store->archiveFeeding(true) == MotionWrite::Stored, "cannot archive historical result");
            TerminalEvent event;
            v4::Message message;
            check(terminalEventFromSlot(pair, store->state().pendingResults[i - 1], event) &&
                  encodeTerminalEvent(pair, event, message), "cannot encode archived result");
            expected.emplace(event.eventId, std::string(reinterpret_cast<const char*>(message.payload), message.length));
        }
        check(store->state().pendingResultCount == 4, "fixture is not a full durable queue");
        resetOwner();
    }
    bool idle() const override { return true; }
    size_t available() const override { return 23; }
    size_t write(const uint8_t* data, size_t count) override {
        count = std::min(count, size_t(7));
        fake_main::uartRx.insert(fake_main::uartRx.end(), data, data + count);
        return count;
    }
    void tick() {
        while (cursor < fake_main::uartTx.size()) {
            const auto byte = fake_main::uartTx[cursor++];
            v4::Frame frame;
            if (auditParser.push(byte, millis(), frame)) {
                check(frame.kind != v4::Kind::Command && frame.kind != v4::Kind::Stop,
                      "historical delivery caused an unsolicited motion/control frame");
                if (dropReceipts && frame.kind == v4::Kind::CloudReceipt) { ++droppedReceiptFrames; continue; }
                core.receiveFrame(frame, millis());
            }
        }
        // Only historical queued results are involved. They were already
        // archived with stationary evidence; false must not block receipts.
        delivery->poll(millis(), false);
        for (unsigned i = 0; i < 160; ++i) core.poll(millis(), *this);
    }
    void resetOwner() {
        check(nvs::io.handles.empty(), "open NVS handle at Motion owner reset");
        delivery.reset();
        store = std::make_unique<MotionStateStore>();
        check(store->load(pair) == MotionLoad::Ready, "Motion cannot reload durable result queue");
        check(core.begin(pair, ++boot), "Motion core restart failed");
        delivery = std::make_unique<motion::MotionResultDelivery<ObservedResultLink>>(core, *store);
        check(core.setCloudReceiptHandler([](const CloudReceipt& receipt, uint32_t) {
            check(current != nullptr, "missing Motion result callback owner");
            ++current->receipts;
            return current->delivery->receipt(receipt);
        }), "Motion receipt handler failed");
        // No action handler is installed: receipt recovery cannot create an
        // execution. Context is independent, saved by the production Store.
        check(core.setContextHandler([](const ProductContext& context, uint32_t, ContextResult& result) {
            check(current != nullptr, "missing Motion context callback owner");
            const auto written = current->store->saveContext(context);
            std::strcpy(result.deviceId, context.deviceId);
            result.profileVersion = context.profileVersion; result.cleared = context.cleared;
            check(contextDigest(context, result.digest), "context digest failed");
            result.status = written == MotionWrite::Stored ? ContextStatus::Stored :
                            written == MotionWrite::Unchanged ? ContextStatus::Unchanged : ContextStatus::StorageFault;
            return true;
        }), "Motion context handler failed");
        current = this;
        auditParser.reset();
        // Bytes already written toward the old boot are discarded, just as a
        // disconnected peer cannot deliver them to the reconstructed owner.
        cursor = fake_main::uartTx.size();
        fake_main::uartRx.clear();
    }
    bool has(const std::string& id) const {
        const auto& state = store->state();
        for (size_t i = 0; i < state.pendingResultCount; ++i)
            if (id == state.pendingResults[i].eventId) return true;
        return false;
    }
    v4::Pairing pair;
    std::unique_ptr<MotionStateStore> store;
    ObservedResultLink core;
    std::unique_ptr<motion::MotionResultDelivery<ObservedResultLink>> delivery;
    std::map<std::string, std::string> expected;
    bool dropReceipts = false;
    unsigned receipts = 0, droppedReceiptFrames = 0;
    inline static ResultMotionPeer* current = nullptr;
private:
    v4::Parser auditParser;
    size_t cursor = 0;
    uint64_t boot = 1000;
};

void runRealResults(bool writeFailure) {
    seed();
    ResultMotionPeer peer;
    setup();
    check(productState.ready() && !simulating(), "result fixture enabled simulation or failed startup");
    // The Brain's cached profile is changed independently from these frozen
    // results. Historical delivery must not require current baby/Ready/STATUS.
    auto updated = initialContext(); updated.profileVersion = 2;
    std::strcpy(updated.babyId, "baby-new"); std::strcpy(updated.babyName, "New baby");
    check(peer.store->saveContext(updated) == MotionWrite::Stored, "Motion profile update failed");
    std::array<uint8_t, v4::kMaxMessage> contextBytes{};
    const size_t contextSize = encodeProductContext(updated, contextBytes.data(), contextBytes.size());
    check(contextSize != 0, "cannot encode current profile update");
    incoming("config", std::string(reinterpret_cast<const char*>(contextBytes.data()), contextSize));
    const auto motionBefore = nvs::io.disk.at("productstate").at("record").bytes;
    auto maskOutbox = [](MotionState value) {
        value.pendingResultCount = 0;
        for (auto& slot : value.pendingResults) slot = MotionExecutionSlot{};
        std::array<uint8_t, kMotionStateMaxSize> bytes{};
        const auto size = encodeMotionState(value, bytes.data(), bytes.size());
        check(size != 0, "cannot encode protected Motion state");
        return nvs::Bytes(bytes.begin(), bytes.begin() + size);
    };
    const auto protectedMotion = maskOutbox(peer.store->state());
    nvs::Namespace brainAfterConfig;
    fake::io.rejectedPublishTopic = prefix + "event";
    unsigned publishedCursor = 0, steps = 0, stage = 0, phaseSteps = 0, failedPublishAttempts = 0;
    std::set<std::string> observed;
    std::map<std::string, unsigned> publicationCounts;
    std::string selected;
    nvs::Bytes beforeDeletion;
    unsigned receiptsBefore = 0, admittedBefore = 0, publishedBefore = 0;
    auto receipt = [&](const std::string& id, const char* status = "stored", const char* target = device) {
        StaticJsonDocument<256> value;
        value["type"] = "feeding_event_receipt";
        value["device_id"] = target; value["event_id"] = id; value["status"] = status;
        incoming("config", encode(value));
    };
    auto next = [&] { ++stage; phaseSteps = 0; };
    fake::io.onDelay = [&](unsigned) {
        const bool worker = fake::io.inWorker; fake::io.inWorker = false;
        peer.tick(); loop();
        for (unsigned i = 0; i < 160; ++i) controllerLink.poll(uint32_t(millis()));
        peer.tick();
        ++steps; ++phaseSteps;
        for (; publishedCursor < fake::io.published.size(); ++publishedCursor) {
            const auto& packet = fake::io.published[publishedCursor];
            if (packet.topic != prefix + "event") continue;
            const auto doc = json(packet.payload);
            const auto id = doc["event_id"].as<std::string>();
            check(peer.expected.count(id) && packet.payload == peer.expected.at(id),
                  "Brain changed Motion's original JSON/identity/frozen recipe");
            check(!doc.containsKey("execution_mode") && doc["baby_id"] == "baby-original" &&
                  doc["feeding_context_profile_version"] == 1 && doc["target_powder_g"] == 45,
                  "real terminal became simulation or switched to current baby");
            if (fake::io.rejectedPublishTopic.empty()) { observed.insert(id); ++publicationCounts[id]; }
            else ++failedPublishAttempts;
        }
        if (stage == 0 && phaseSteps >= 100 && failedPublishAttempts) {
            check(failedPublishAttempts && observed.empty() && peer.store->state().pendingResultCount == 4 &&
                  nvs::io.disk.at("productstate").at("record").bytes == motionBefore,
                  "failed MQTT publication removed or modified Motion evidence");
            check(productState.state().context.profileVersion == 2 &&
                  !std::strcmp(productState.state().context.babyId, "baby-new"), "Brain did not save current baby update");
            brainAfterConfig = nvs::io.disk.at("brainstate");
            fake::io.rejectedPublishTopic.clear(); next();
        } else if (stage == 1 && observed.size() == 4) {
            check(peer.store->state().pendingResultCount == 4, "MQTT publish acted as stored receipt");
            selected = *observed.begin(); beforeDeletion = nvs::io.disk.at("productstate").at("record").bytes;
            receipt(selected, "received"); receipt(selected, "stored", "different-device");
            receipt("event-not-in-this-queue"); next();
        } else if (stage == 2 && phaseSteps >= 40) {
            check(nvs::io.disk.at("productstate").at("record").bytes == beforeDeletion && peer.has(selected),
                  "invalid receipt removed durable evidence");
            peer.dropReceipts = true; receipt(selected); next();
        } else if (stage == 3 && phaseSteps >= 40) {
            check(peer.droppedReceiptFrames && peer.has(selected) &&
                  nvs::io.disk.at("productstate").at("record").bytes == beforeDeletion,
                  "lost UART receipt cleared result");
            peer.dropReceipts = false;
            peer.resetOwner(); // Actual Store/link/owner reload, not Motion setup or mechanical restart.
            admittedBefore = peer.core.admitted[selected]; publishedBefore = publicationCounts[selected];
            receiptsBefore = peer.receipts; next();
        } else if (stage == 4 && peer.core.connected(millis()) && phaseSteps >= 40 &&
                   peer.core.admitted[selected] > admittedBefore && publicationCounts[selected] > publishedBefore) {
            check(peer.has(selected), "owner restart erased unacknowledged historical result");
            if (writeFailure) nvs::fail(nvs::Op::Commit, nvs::count(nvs::Op::Commit) + 1);
            receipt(selected); next();
        } else if (stage == 5 && peer.receipts > receiptsBefore &&
                   (writeFailure ? peer.store->faulted() : !peer.has(selected))) {
            if (writeFailure) {
                check(nvs::io.disk.at("productstate").at("record").bytes == beforeDeletion,
                      "failed deletion changed durable result queue");
                nvs::verifyFaults(); nvs::io.faults.clear();
                peer.resetOwner();
                check(peer.has(selected), "commit failure/reload lost result");
            }
            admittedBefore = peer.core.admitted[selected]; publishedBefore = publicationCounts[selected];
            next();
        } else if (stage == 6 && peer.core.connected(millis()) && phaseSteps >= 40 &&
                   (!writeFailure || (peer.core.admitted[selected] > admittedBefore &&
                                      publicationCounts[selected] > publishedBefore))) {
            receipt(selected); next();
        } else if (stage == 7 && !peer.has(selected)) {
            check(peer.store->state().pendingResultCount == 3, "matching receipt deleted more than one result");
            receiptsBefore = peer.receipts;
            beforeDeletion = nvs::io.disk.at("productstate").at("record").bytes;
            receipt(selected); next();
        } else if (stage == 8 && phaseSteps >= 40 && peer.receipts > receiptsBefore) {
            check(peer.store->state().pendingResultCount == 3 &&
                  nvs::io.disk.at("productstate").at("record").bytes == beforeDeletion,
                  "delivered duplicate stored changed unrelated durable results");
            for (const auto& item : peer.expected) if (peer.has(item.first)) { selected = item.first; break; }
            receipt(selected); next();
        } else if (stage >= 9 && stage <= 11 && !peer.has(selected)) {
            const unsigned left = 11 - stage;
            check(peer.store->state().pendingResultCount == left, "receipt did not delete exactly its queue item");
            if (!left) next();
            else {
                for (const auto& item : peer.expected) if (peer.has(item.first)) { selected = item.first; break; }
                receipt(selected); next();
            }
        }
        if (phaseSteps >= 400)
            throw std::runtime_error("real result recovery stalled at stage " + std::to_string(stage) +
                                     " events=" + std::to_string(observed.size()));
        if (stage == 12) throw fake::StopWorker{};
        fake::io.now += 5; fake::io.inWorker = worker;
    };
    fake::runWorker();
    check(stage == 12 && observed.size() == 4 && peer.store->ready() &&
          peer.store->state().slot.kind == MotionSlotKind::Empty &&
          peer.store->state().pendingResultCount == 0 &&
          peer.store->state().context.profileVersion == 2 &&
          peer.store->state().cloudSequence == 3 && peer.store->state().localSequence == 4,
          "recovery changed context, consumption watermarks or active slot");
    check(maskOutbox(peer.store->state()) == protectedMotion, "receipt deletion changed protected Motion evidence");
    check(nvs::io.disk.at("brainstate") == brainAfterConfig && !productState.state().pending &&
          !productState.state().localSequence,
          "historical result delivery changed Brain durable request identity");
    check(!controllerLink.connected(millis()) && !cloudCanStart() && !contextSync.canPrepare() &&
          !unsafeTxPrefix(fake_main::uartTx), "historical delivery fabricated readiness or requested motion");
    check(nvs::io.handles.empty() && !nvs::count(nvs::Op::Erase) && !nvs::count(nvs::Op::Init) &&
          !fake::io.preferenceWriteCalls, "unsafe result recovery NVS operation");
    ResultMotionPeer::current = nullptr;
    std::printf("REAL RESULTS %s: steps=%u unique_events=%zu receipts=%u lost_receipt_frames=%u failed_publish=%u remaining=0\n",
                writeFailure ? "commit-failure" : "receipt-loss", steps, observed.size(), peer.receipts,
                peer.droppedReceiptFrames, failedPublishAttempts);
    fake::cleanupLifetimeResources();
}
}
