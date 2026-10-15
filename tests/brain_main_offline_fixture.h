// Production Brain cold boot with valid saved pairing/context and permanent
// MQTT SDK rejection. The scripted Motion peer verifies dispatch/ownership,
// not mechanical acceptance or a real broker's password/ACL enforcement.
#pragma once

namespace {
void runColdOffline(const std::string& failure) {
    seed();
    const bool connectRejected = failure == "connect-rejected";
    fake::io.connectOk = !connectRejected;
    fake::io.onConnect = [&] {
        if (connectRejected) return;
        // Apply on EVERY reconnect, not just the first attempt. An empty fake
        // reply queue would otherwise silently authorize later subscriptions.
        fake::io.subscriptionResults = failure == "command-subscribe-failed" ?
            std::deque<bool>{false} : std::deque<bool>{true, false};
    };
    const auto protectedPair = nvs::io.disk.at("productpair");
    const auto protectedWifi = nvs::io.disk.at("wifi-cfg");
    const auto protectedMqtt = fake::io.preferences;
    setup();
    check(productState.ready() && network.started() && fake::io.tasks.size() == 1,
          "cold offline startup did not load saved state/start network worker");
    ReadinessPeer peer;
    peer.status.snapshot.stage = babytech::display::DisplayStage::NotReady;
    peer.status.snapshot.startEnabled = false;
    std::strcpy(peer.status.productProgress, "noready");
    peer.sendStatus = peer.automaticContext = true;
    unsigned phase = 0, steps = 0, stalled = 0;
    uint32_t sinceLocalDone = 0;
    const auto assertLocalRequest = [&](ProductCommand command, uint64_t sequence) {
        const auto& request = peer.command.request;
        char id[129]{};
        check(makeLocalCommandId(productState.state().pairing, sequence, id), "canonical local ID failed");
        check(request.command == command && request.source == v4::Source::LocalTouch &&
              request.sequence == sequence && !std::strcmp(request.commandId, id) &&
              !std::strcmp(request.deviceId, device) && productState.state().pending &&
              productState.state().localSequence == sequence, "cold offline request identity/persistence mismatch");
    };
    fake::io.onDelay = [&](unsigned) {
        const bool worker = fake::io.inWorker; fake::io.inWorker = false;
        peer.tick(); loop();
        // Drain SDK short writes at one logical instant, not a baud-rate or
        // FreeRTOS timing claim. This is the actual Arduino UART adapter.
        for (unsigned i = 0; i < 160; ++i) controllerLink.poll(uint32_t(millis()));
        peer.observe();
        ++steps; ++stalled;
        check(!network.connected() && !simulating() && !cloudDispatcher.busy() &&
              network.checkFreshness("0123456789abcdef0123456789abcdef", 1, millis(), 5000) ==
                  babytech::cloud::Freshness::Disconnected,
              "rejected MQTT connection/subscription opened a remote command session");
        check(fake::io.published.empty() && !fake::io.loopCalls,
              "rejected MQTT session still consumed/published remote messages");
        if (phase == 0 && contextSync.canPrepare() && fake_main::shownConnected &&
            fake_main::shown.stage == babytech::display::DisplayStage::NotReady &&
            !fake_main::shown.startEnabled) {
            check(!fake_main::shown.cloudConnected, "offline screen fabricated Cloud connectivity");
            fake_main::intent = babytech::display::DisplayIntent::Initialize;
            ++phase; stalled = 0;
        } else if (phase == 1 && peer.commands == 1) {
            assertLocalRequest(ProductCommand::Initialize, 1);
            peer.replyCommand(true);
            peer.status.snapshot.stage = babytech::display::DisplayStage::Ready;
            peer.status.snapshot.startEnabled = true;
            std::strcpy(peer.status.productProgress, "ready");
            std::strcpy(peer.status.localWatermark, "1"); peer.changedStatus();
            ++phase; stalled = 0;
        } else if (phase == 2 && !productState.state().pending && !localDispatcher.busy() &&
                   contextSync.canPrepare() && fake_main::shown.startEnabled) {
            fake_main::intent = babytech::display::DisplayIntent::StartFeeding;
            ++phase; stalled = 0;
        } else if (phase == 3 && peer.commands == 2) {
            assertLocalRequest(ProductCommand::Prepare, 2);
            const auto& request = peer.command.request;
            check(!std::strcmp(request.babyId, "baby-original") && request.profileVersion == 1 &&
                  request.waterMl == 180 && request.temperatureC == 45 && request.powderGPer100Ml == 25,
                  "offline Prepare did not freeze the saved baby/recipe");
            peer.replyCommand(true);
            std::strcpy(peer.status.localWatermark, "2"); peer.changedStatus();
            ++phase; stalled = 0;
        } else if (phase == 4 && !productState.state().pending && !localDispatcher.busy()) {
            sinceLocalDone = millis(); ++phase; stalled = 0;
        } else if (phase == 5 && fake::io.connectCalls >= 3 && uint32_t(millis() - sinceLocalDone) >= 6000) {
            check(peer.commands == 2 && !peer.stops && !peer.queries && peer.contexts >= 1,
                  "persistent MQTT retry repeated a local action or requested Stop/query");
            check(fake_main::shownConnected && !fake_main::shown.cloudConnected &&
                  fake_main::shown.startEnabled && contextSync.canPrepare(),
                  "persistent auth failure disabled otherwise eligible offline UI");
            throw fake::StopWorker{};
        }
        if (stalled >= 700)
            throw std::runtime_error("cold offline stalled: phase=" + std::to_string(phase) +
                                     " failure=" + failure + " commands=" + std::to_string(peer.commands) +
                                     " local_reason=" + localDispatcher.reason() +
                                     " local_seq=" + std::to_string(productState.state().localSequence));
        fake::io.now += 5; fake::io.inWorker = worker;
    };
    fake::runWorker();
    check(phase == 5 && peer.commands == 2 && productState.state().localSequence == 2 &&
          !productState.state().pending && sameProductContext(productState.state().context, initialContext()),
          "offline execution changed saved context or left unresolved acceptance");
    BrainState durable;
    const auto& bytes = nvs::io.disk.at("brainstate").at("record").bytes;
    check(decodeBrainState(bytes.data(), bytes.size(), durable) && !durable.pending &&
          durable.localSequence == 2 && sameProductContext(durable.context, initialContext()),
          "offline acceptance was not durably cleared with sequence preserved");
    check(nvs::io.disk.at("productpair") == protectedPair && nvs::io.disk.at("wifi-cfg") == protectedWifi &&
          fake::io.preferences == protectedMqtt && !fake::io.preferenceWriteCalls &&
          !nvs::count(nvs::Op::Erase) && !nvs::count(nvs::Op::Init) && nvs::io.handles.empty(),
          "auth failure altered installation/credentials or erased NVS");
    check(fake::io.credentialsMatched, "network worker did not use the saved test credentials");
    const auto subscriptions = fake::io.subscriptions.size();
    check(subscriptions == fake::io.connectCalls *
          (connectRejected ? 0U : failure == "command-subscribe-failed" ? 1U : 2U),
          "persistent subscription failure variant was not applied to every attempt");
    std::vector<std::string> expectedTopics;
    if (!connectRejected) for (unsigned i = 0; i < fake::io.connectCalls; ++i) {
        expectedTopics.push_back(prefix + "command");
        if (failure == "config-subscribe-failed") expectedTopics.push_back(prefix + "config");
    }
    check(fake::io.subscriptions == expectedTopics && fake::io.connectedId == device,
          "MQTT retry used a different Device ID or subscription topic/order");
    for (const auto& call : WiFi.calls) check(call.worker, "offline Wi-Fi I/O escaped network worker");
    fake::io.onConnect = {};
    std::printf("PASS Brain cold-offline %s: steps=%u attempts=%u subscriptions=%zu local_commands=2 seq=2\n",
                failure.c_str(), steps, fake::io.connectCalls, subscriptions);
    fake::cleanupLifetimeResources();
}
}
