#pragma once
#include <nvs.h>

namespace {
void checkPortalStringFake() {
    nvs_handle_t handle;
    check(nvs_open("wifi-cfg", NVS_READWRITE, &handle) == ESP_OK, "string fake open failed");
    check(nvs_set_str(handle, "ssid", "staged") == ESP_OK, "string staging failed");
    check(nvs::io.disk.at("wifi-cfg").empty() &&
          nvs::io.handles.at(handle).pending.at("ssid").type == nvs::Type::String,
          "string set bypassed staging/type");
    char readback[33]; size_t length = sizeof(readback);
    check(nvs_get_str(handle, "ssid", readback, &length) == ESP_OK && !std::strcmp(readback, "staged"),
          "same-handle string read missed pending value");
    nvs::fail(nvs::Op::Set, nvs::count(nvs::Op::Set) + 1);
    check(nvs_set_str(handle, "pass", "not-written") == ESP_FAIL &&
          !nvs::io.handles.at(handle).pending.count("pass"), "string set bypassed fault injection");
    nvs::fail(nvs::Op::Commit, nvs::count(nvs::Op::Commit) + 1);
    check(nvs_commit(handle) == ESP_FAIL && nvs::io.disk.at("wifi-cfg").empty(), "failed commit became durable");
    nvs_close(handle);
    check(nvs_open("wifi-cfg", NVS_READWRITE, &handle) == ESP_OK, "string fake reopen failed");
    length = sizeof(readback);
    check(nvs_get_str(handle, "ssid", readback, &length) == ESP_ERR_NVS_NOT_FOUND,
          "close retained uncommitted string writes");
    nvs::fail(nvs::Op::Set, nvs::count(nvs::Op::Set) + 1, ESP_FAIL, true);
    check(nvs_set_str(handle, "ssid", "early-write") == ESP_FAIL &&
          nvs::io.disk.at("wifi-cfg").at("ssid").type == nvs::Type::String,
          "string error-after-write semantics missing");
    nvs_close(handle); nvs::verifyFaults();
    nvs::reset(); fake_commissioning::reset();
}
void runPortalMain(bool unpaired) {
    if (unpaired) {
        nvs::reset(); fake_commissioning::reset(); WiFi.state = 0;
        fake_main::allowCredentialWrites = true;
        checkPortalStringFake();
    }
    else seed();
    setup();
    check(fake::io.tasks.size() == 1, "portal introduced zero/two radio workers");
    auto& http = *WebServer::instances.back();
    ReadinessPeer peer; peer.sendStatus = peer.automaticContext = true;
    unsigned phase = 0, phasePolls = 0;
    auto request = [&](const char* path, HTTPMethod method) {
        fake::io.inWorker = true; http.request(path, method); fake::io.inWorker = false;
    };
    auto save = [&] {
        http.requestHeaders["X-Babytech-Portal"] = "1";
        // Valid HTTP input but invalid station password: no unrelated radio/NVS
        // changes after admission, while exercising the actual main save gate.
        http.arguments = {{"ssid", "fixture-wifi"}, {"password", "short"}};
        request("/api/wifi", HTTP_POST);
    };
    fake::io.onDelay = [&](unsigned) {
        const bool worker = fake::io.inWorker; fake::io.inWorker = false;
        peer.tick(); loop();
        for (unsigned n = 0; n < 160; ++n) controllerLink.poll(millis());
        peer.observe();
        if (++phasePolls > 1000) throw std::runtime_error("portal main phase stalled: " + std::to_string(phase));
        bool next = false;
        if (unpaired) {
            check(!feedingForPortal(millis()), "unpaired/offline became feeding gate");
            if (phase == 0 && http.listening) {
                request("/api/status", HTTP_GET);
                const auto doc = json(http.response);
                check(http.status == 200 && doc["host"] == "101.33.219.108" && doc["port"] == 1883 &&
                      doc["device_id"] == "" && doc["development_code"] == "", "blank/unpaired portal defaults wrong");
                save(); check(http.status == 202, "unpaired config save was blocked");
                loop(); request("/api/status", HTTP_GET);
                check(http.response.find("save_failed") != std::string::npos, "mailbox apply missing completion");
                http.arguments = {{"ssid", "unpaired-wifi"}, {"password", "portal-fixture-password"}};
                request("/api/wifi", HTTP_POST); check(http.status == 202, "valid unpaired save rejected");
                loop(); request("/api/status", HTTP_GET);
                check(http.response.find("wifi_saved") != std::string::npos, "valid unpaired save failed");
                next = true;
            } else if (phase == 1 && WiFi.attemptedSsid == "unpaired-wifi") {
                WiFi.state = WL_CONNECTED; WiFi.ssid = "unpaired-wifi"; WiFi.address = IPAddress(192, 168, 1, 40);
                next = true;
            } else if (phase == 2 && !http.listening) {
                check(!fake::io.connectCalls && fake::io.published.empty(), "unpaired WiFi published MQTT identity");
                check(WiFi.attemptedPassword == "portal-fixture-password", "unpaired join lost supplied password");
                throw fake::StopWorker{};
            }
        } else if (phase == 0 && cloudCanStart()) {
            check(!feedingForPortal(millis()), "idle readiness blocked portal"); fakeBootLevel = LOW; next = true;
        } else if (phase == 1 && http.listening) {
            fakeBootLevel = 1;
            save(); check(http.status == 202, "idle save rejected"); loop();
            ProductRequest initialize; initialize.command = ProductCommand::Initialize;
            initialize.source = v4::Source::LocalTouch; initialize.sequence = 1;
            observeRealAcceptance(initialize, millis());
            check(!feedingForPortal(millis()), "Initialize became a config gate");
            check(localDispatcher.dispatch(babytech::display::DisplayIntent::StartFeeding, millis()), "local Prepare dispatch failed");
            check(localDispatcher.preparePending() && feedingForPortal(millis()), "pending local Prepare not guarded"); next = true;
        } else if (phase == 2 && peer.commands == 1) {
            peer.replyCommand(true); next = true;
        } else if (phase == 3 && !localDispatcher.busy() && portalFeedingAccepted) {
            check(feedingForPortal(millis()), "accepted Prepare did not guard configuration");
            save(); check(http.status == 409, "accepted Prepare allowed save");
            peer.changedStatus(); next = true;
        } else if (phase == 4 && controllerLink.lastTelemetryReceivedAtMs() > portalAcceptedAt) {
            check(!std::strcmp(controllerLink.lastTelemetry()->localWatermark, "0") && feedingForPortal(millis()),
                  "idle N-1 released accepted Prepare");
            save(); check(http.status == 409, "newly received idle N-1 admitted config");
            std::strcpy(peer.status.localWatermark, "1"); peer.changedStatus(); next = true;
        } else if (phase == 5 && !std::strcmp(controllerLink.lastTelemetry()->localWatermark, "1")) {
            check(!feedingForPortal(millis()), "idle watermark N did not release config");
            loop(); save(); check(http.status == 202, "idle watermark N did not admit config"); loop();
            check(localDispatcher.dispatch(babytech::display::DisplayIntent::StartFeeding, millis()), "second Prepare failed"); next = true;
        } else if (phase == 6 && peer.commands == 2) {
            peer.replyCommand(true); next = true;
        } else if (phase == 7 && !localDispatcher.busy() && portalFeedingAccepted) {
            auto wrong = peer.command.request; wrong.sequence = 1;
            check(makeLocalCommandId(productState.state().pairing, 1, wrong.commandId), "wrong terminal ID");
            peer.replyTerminal(wrong); next = true;
        } else if (phase == 8 && portalLastTerminalSequence == 1) {
            check(feedingForPortal(millis()), "old terminal released current Prepare");
            peer.replyTerminal(peer.command.request); next = true;
        } else if (phase == 9 && portalLastTerminalSequence == 2) {
            check(!feedingForPortal(millis()), "matching terminal did not release config");
            observeRealAcceptance(peer.command.request, millis());
            check(!feedingForPortal(millis()), "late duplicate acceptance resurrected config gate");
            loop(); save(); check(http.status == 202, "matching terminal did not admit config"); loop();
            throw fake::StopWorker{};
        }
        if (next) { ++phase; phasePolls = 0; }
        fake::io.inWorker = worker;
    };
    fake::runWorker();
    check(unpaired || phase == 9, "portal guard schedule unfinished");
    check(!nvs::count(nvs::Op::Erase) && !nvs::count(nvs::Op::Init), "portal erased/initialized NVS");
    for (const auto& call : WiFi.calls) check(call.worker, "portal radio work escaped network worker");
    fake::cleanupLifetimeResources();
}
}
