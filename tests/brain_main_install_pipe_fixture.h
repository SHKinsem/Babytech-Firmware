#pragma once
#include "main_install_nvs_fixture.h"

namespace {
// Offline installation exercises the real UI/main owner, not a fabricated
// network worker. Only typed SDK disk bytes survive a process restart.
void runInstallBridge(const DualOptions& options) {
    check(options.disk.has_value(), "installation pipe requires explicit disk");
    nvs::reset(); fake_commissioning::reset();
    fake_commissioning::mac = {{0x11, 0x22, 0x33, 0x44, 0x55, 0x66}};
    nvs::io.disk = *options.disk;
    fake_main::randomCounter = options.seed;
    fake_main::uartWriteLimit = 23;
    WiFi.state = 0;  // SDK Wi-Fi reports disconnected; no worker is executed.
    setup();
    main_install_nvs::PairCommitFault fault;
    fault.arm(options.pairCommitFault);
    for (;;) {
        check(!simulating() && fake_main::uartTx.size() <= dualUartLimit &&
              fake_main::usb.output.size() <= dualUartLimit, "installation output budget exceeded");
        DynamicJsonDocument report(dualOutputLimit);
        report["now_ms"] = millis(); report["simulation"] = false;
        report["store_ready"] = productState.ready();
        report["runtime_paired"] = controllerLink.verifiedPairing() != nullptr;
        report["motion_connected"] = controllerLink.connected(millis());
        report["installer_status"] = installer.status(); report["installer_reason"] = installer.reason();
        report["writes_requested"] = installer.mayHaveWritten();
        report["install_pair_commit_fault_hit"] = fault.hit();
        report["nvs_set_count"] = nvs::count(nvs::Op::Set);
        report["nvs_commit_count"] = nvs::count(nvs::Op::Commit);
        report["maintenance"] = commissioningSession.active();
        report["local_sequence"] = std::to_string(productState.state().localSequence);
        report["pending"] = productState.state().pending;
        report["network_connected"] = network.connected();
        report["network_tasks"] = fake::io.tasks.size();
        report["usb_output"] = fake_main::usb.output;
        report["uart_tx"] = dualHex(fake_main::uartTx.data(), fake_main::uartTx.size());
        v4::Pairing stored;
        const auto loaded = loadBoardPairing(v4::Role::Brain, stored);
        report["pairing_load"] = unsigned(loaded);
        if (loaded == PairingLoad::Ready) {
            auto pair = report.createNestedObject("pairing");
            pair["device_id"] = stored.deviceId; pair["epoch"] = stored.epoch;
            pair["local"] = stored.localPhysicalId; pair["peer"] = stored.peerPhysicalId;
        }
        if (productState.ready() && validProductContext(productState.state().context)) {
            uint8_t bytes[v4::kMaxMessage];
            const auto count = encodeProductContext(productState.state().context, bytes, sizeof(bytes));
            check(count, "installation context encode failed");
            report["context_json"] = std::string(reinterpret_cast<const char*>(bytes), count);
            uint8_t digest[kProductDigestSize];
            check(contextDigest(productState.state().context, digest), "installation context digest failed");
            report["context_digest"] = dualHex(digest, sizeof(digest));
        }
        dualWriteSnapshot(report.createNestedArray("snapshot"));
        check(!report.overflowed(), "installation report overflow");
        std::cout << "DUAL_BRAIN=" << encode(report) << std::endl;
        fake_main::uartTx.clear(); fake_main::usb.output.clear();
        bool present;
        const auto line = dualReadBounded(std::cin, dualInputLimit, true, present);
        if (!present) break;
        const auto step = dualParseStep(line, true);
        check(step.incoming.empty() && !step.connected.has_value() &&
              step.intent == babytech::display::DisplayIntent::None && !step.stopClick,
              "installation pipe is offline and USB/UART only");
        if (step.quit) break;
        check(fake_main::uartRx.size() + step.uart.size() <= dualUartLimit &&
              fake_main::usb.input.size() + step.usb.size() + 1 <= 256,
              "installation SDK backlog exceeded");
        fake::io.now += step.advance;
        if (!step.usb.empty()) fake_main::input(step.usb.c_str());
        fake_main::uartRx.insert(fake_main::uartRx.end(), step.uart.begin(), step.uart.end());
        loop();
    }
    check(!nvs::count(nvs::Op::Erase) && !nvs::count(nvs::Op::Init) && nvs::io.handles.empty(),
          "installation unsafe NVS operation/leak");
    nvs::verifyFaults();
    fake::cleanupLifetimeResources();
}
}  // namespace
