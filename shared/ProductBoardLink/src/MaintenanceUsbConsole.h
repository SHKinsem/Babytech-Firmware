#pragma once

#include "MaintenanceConsole.h"
#include "MaintenanceExport.h"
#include "BoardProtocol.h"
#include <cstdio>

namespace babytech { namespace boardlink {

// Single loop-task USB owner. Firmware must bound its SDK's CDC TX wait;
// Arduino 3.3 HWCDC needs a positive timeout to avoid retry-counter underflow.
// All replies are bounded/queued; one complete input command per poll. JSON is
// transported in independently checked chunks so ordinary logs may interleave.
class MaintenanceUsbConsole {
public:
    using ExtraCommand = bool (*)(const char*, char*, size_t);
    void begin(v4::Role role, uint64_t boot) { role_ = role; boot_ = boot; }
    bool active() const { return session_.active(); }
    bool exporting() const { return export_.active(); }

    template<class Port>
    void poll(Port& port, uint32_t nowMs, bool localSafe, ExtraCommand extraCommand = nullptr) {
        if (export_.active() && (!session_.active() || !localSafe || export_.expired(nowMs))) {
            export_.cancel();
            clearResponse();
            reply("export_aborted", nowMs);
        }
        if (responseSize_) {
            if (uint32_t(nowMs - responseAt_) >= MaintenanceExport::kLifetimeMs) {
                clearResponse();
                return;
            }
            flush(port);
            return;
        }
        if (export_.active()) {
            frame(nowMs);
            flush(port);
            return;
        }
        for (size_t n = 0; n < MaintenanceLineReader::kPollBytes && port.available() > 0; ++n) {
            const int byte = port.read();
            if (byte < 0) break;
            const auto result = lines_.feed(static_cast<uint8_t>(byte), nowMs);
            if (result == MaintenanceLineReader::Result::Rejected) {
                reply("invalid_line", nowMs);
                return;
            }
            if (result != MaintenanceLineReader::Result::Line) continue;
            struct ClearLine {
                MaintenanceLineReader& reader;
                ~ClearLine() { reader.clear(); }
            } clearLine{lines_};
            const char* line = lines_.line();
            response_[0] = '\n';
            if (extraCommand && extraCommand(line, response_ + 1, sizeof(response_) - 1)) {
                const void* end = std::memchr(response_ + 1, 0, sizeof(response_) - 1);
                if (!end) reply("invalid_response", nowMs);
                else {
                    responseSize_ = size_t(static_cast<const char*>(end) - response_);
                    responseAt_ = nowMs;
                }
                return;
            }
            clearResponse();
            char device[65], challenge[33];
            const auto command = parseMaintenanceExport(line, device, challenge);
            if (command != ExportCommand::NotExport) {
                if (command == ExportCommand::Invalid) reply("invalid_export", nowMs);
                else if (!session_.active() || !localSafe) reply("unsafe", nowMs);
                else if (!export_.begin(role_, device, challenge, boot_, nowMs)) reply("export_failed", nowMs);
                else exportOffset_ = 0;
                return;
            }
            const char* response = session_.handle(line, localSafe);
            reply(response ? response : "unknown_command", nowMs);
            return;
        }
    }

private:
    void clearResponse() {
        std::memset(response_, 0, sizeof(response_));
        responseSize_ = responsePosition_ = framePayloadSize_ = 0;
    }
    void reply(const char* message, uint32_t nowMs) {
        clearResponse();
        const int length = std::snprintf(response_, sizeof(response_), "\n[maint] %s\n", message);
        if (length > 0 && size_t(length) < sizeof(response_)) responseSize_ = size_t(length);
        responseAt_ = nowMs;
    }
    void frame(uint32_t nowMs) {
        uint8_t bytes[19];
        const size_t length = export_.peek(bytes + 3, 16);
        bytes[0] = uint8_t(exportOffset_);
        bytes[1] = uint8_t(exportOffset_ >> 8);
        bytes[2] = uint8_t(length);
        const uint16_t crc = babytech::crc16(bytes, length + 3);
        const int header = std::snprintf(response_, sizeof(response_), "\n[mx] %04x:%04x:",
                                         unsigned(exportOffset_), unsigned(crc));
        responseSize_ = size_t(header);
        constexpr char hex[] = "0123456789abcdef";
        for (size_t n = 0; n < length; ++n) {
            response_[responseSize_++] = hex[bytes[n + 3] >> 4];
            response_[responseSize_++] = hex[bytes[n + 3] & 15];
        }
        response_[responseSize_++] = '\n';
        std::memset(bytes, 0, sizeof(bytes));
        framePayloadSize_ = length;
        responseAt_ = nowMs;
    }
    template<class Port>
    void flush(Port& port) {
        const int available = port.availableForWrite();
        if (available <= 0) return;
        size_t budget = responseSize_ - responsePosition_;
        if (budget > MaintenanceLineReader::kPollBytes) budget = MaintenanceLineReader::kPollBytes;
        if (budget > size_t(available)) budget = size_t(available);
        const size_t written = port.write(reinterpret_cast<const uint8_t*>(response_) + responsePosition_, budget);
        if (written > budget) { export_.cancel(); clearResponse(); return; }
        responsePosition_ += written;
        if (responsePosition_ != responseSize_) return;
        if (framePayloadSize_) {
            export_.consume(framePayloadSize_);
            exportOffset_ += framePayloadSize_;
        }
        clearResponse();
    }

    v4::Role role_ = v4::Role::Brain;
    uint64_t boot_ = 0;
    MaintenanceSession session_;
    MaintenanceLineReader lines_;
    MaintenanceExport export_;
    char response_[96]{};
    size_t responseSize_ = 0, responsePosition_ = 0, framePayloadSize_ = 0, exportOffset_ = 0;
    uint32_t responseAt_ = 0;
};

} }
