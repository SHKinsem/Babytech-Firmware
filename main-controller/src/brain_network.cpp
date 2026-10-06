#include "brain_network.h"

#if BABYTECH_BOARD_LINK_V4
#include "brain_status.h"
#include <cstdio>
#include <cstring>

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "unknown"
#endif

namespace babytech { namespace brain {
namespace {
const char* text(JsonVariantConst value, size_t maximum) {
    if (!value.is<JsonString>()) return nullptr;
    const JsonString str = value.as<JsonString>();
    if (!str.size() || str.size() > maximum || std::strlen(str.c_str()) != str.size()) return nullptr;
    return str.c_str();
}
}

bool BrainNetwork::begin(const char* pairedDeviceId) {
    if (started_ || !pairedDeviceId || !pairedDeviceId[0]) return false;
    started_ = cloud_.beginV4(pairedDeviceId, BrainStation::service, &station_);
    return started_;
}

bool BrainNetwork::publishStatus(const boardlink::Status* lastMotion, bool motionConnected,
                                const cloud::SessionSnapshot& session, uint32_t motionReceivedAtMs,
                                const char* challenge) {
    status_.clear();
    writeStatus(status_.to<JsonObject>(), cloud_.deviceId(), FIRMWARE_VERSION,
                lastMotion, motionConnected, session, challenge);
    if (!encodeStatusJson(status_, payload_, sizeof(payload_))) return false;
    CloudLink::StatusPublishOptions options;
    options.probeReply = challenge != nullptr;
    options.hasMotionSample = motionConnected && lastMotion;
    options.motionReceivedAtMs = motionReceivedAtMs;
    return cloud_.publishStatusForSession(String(payload_), session.generation, options);
}

void BrainNetwork::rejectCommand(const char* commandId, const char* command, uint64_t sequence,
                                 const char* session, uint32_t sampledAtMs, uint16_t ttlMs) {
    const auto freshness = cloud_.checkFreshness(session, inbound_.generation, sampledAtMs, ttlMs);
    if (freshness != cloud::Freshness::Current && freshness != cloud::Freshness::Expired) return;
    char sequenceText[20]{};
    const int length = std::snprintf(sequenceText, sizeof(sequenceText), "%llu",
                                    static_cast<unsigned long long>(sequence));
    if (length <= 0 || size_t(length) >= sizeof(sequenceText)) return;
    status_.clear();
    status_["device_id"] = cloud_.deviceId();
    status_["command_id"] = commandId;
    status_["command"] = command;
    status_["command_seq"] = static_cast<const char*>(sequenceText);
    status_["command_session"] = session;
    status_["accepted"] = false;
    status_["reason"] = freshness == cloud::Freshness::Expired
        ? "request_expired" : "integration_not_ready";
    if (!encodeStatusJson(status_, payload_, sizeof(payload_))) return;
    // This is an explicit local rejection, never a fabricated Motion acceptance.
    // A reconnect drops this reply instead of publishing it in another generation.
    cloud_.publishForSession("ack", String(payload_), inbound_.generation);
}

void BrainNetwork::receiveCommand() {
    const auto* bytes = reinterpret_cast<const uint8_t*>(inbound_.payload);
    const size_t length = std::strlen(inbound_.payload);
    boardlink::CloudCommand command;
    if (boardlink::decodeCloudCommand(bytes, length, cloud_.deviceId(), command)) {
        const char* name = nullptr;
        switch (command.request.command) {
            case boardlink::ProductCommand::Prepare: name = "prepare"; break;
            case boardlink::ProductCommand::Clean: name = "clean"; break;
            case boardlink::ProductCommand::SetTargetTemp: name = "set_target_temp"; break;
            case boardlink::ProductCommand::ResetError: name = "reset_error"; break;
            case boardlink::ProductCommand::CheckFirmwareUpdate: name = "check_firmware_update"; break;
            default: return;
        }
        rejectCommand(command.request.commandId, name, command.request.sequence,
                      command.session, command.sampledAtMs, command.ttlMs);
        return;
    }
    boardlink::CloudStop stop;
    if (boardlink::decodeCloudStop(bytes, length, cloud_.deviceId(), stop))
        rejectCommand(stop.commandId, "stop", stop.sequence, stop.session, stop.sampledAtMs, stop.ttlMs);
}

void BrainNetwork::poll(const boardlink::Status* lastMotion, bool motionConnected, uint32_t nowMs,
                        uint32_t motionReceivedAtMs) {
    if (!started_) return;
    cloud::SessionSnapshot current;
    if (!cloud_.sessionSnapshot(current)) {
        published_ = false;
        attempted_ = false;
        return;
    }
    if (generation_ != current.generation) {
        generation_ = current.generation;
        published_ = false;
        attempted_ = false;
    }
    for (unsigned count = 0; count < 3 && cloud_.take(inbound_); ++count) {
        const char* device = cloud_.deviceId();
        const size_t deviceLength = std::strlen(device);
        if (std::strncmp(inbound_.topic, "devices/", 8) ||
            std::strncmp(inbound_.topic + 8, device, deviceLength)) continue;
        const char* suffix = inbound_.topic + 8 + deviceLength;
        if (!std::strcmp(suffix, "/command")) {
            receiveCommand();
            continue;
        }
        if (std::strcmp(suffix, "/config")) continue;
        incomingJson_.clear();
        if (deserializeJson(incomingJson_, inbound_.payload)) continue;
        if (!incomingJson_.is<JsonObject>() || incomingJson_.size() != 4) continue;
        const char* type = text(incomingJson_["type"], 32);
        const char* targetDevice = text(incomingJson_["device_id"], 64);
        const char* target = text(incomingJson_["command_session"], 32);
        const char* challenge = text(incomingJson_["challenge"], 32);
        if (!type || std::strcmp(type, "command_session_probe") || !targetDevice ||
            std::strcmp(targetDevice, cloud_.deviceId()) || !target || !challenge) continue;
        cloud::ProbeReply reply;
        if (cloud_.probeReply(target, challenge, inbound_.generation, reply))
            publishStatus(lastMotion, motionConnected, reply.session, motionReceivedAtMs, reply.challenge);
        // Other messages deliberately cannot dispatch actions or mutate NVS
        // during the read-only integration stage.
    }
    if ((!published_ || uint32_t(nowMs - lastPublishedAtMs_) >= 2000) &&
        (!attempted_ || uint32_t(nowMs - lastAttemptAtMs_) >= 250)) {
        attempted_ = true;
        lastAttemptAtMs_ = nowMs;
        if (cloud_.sessionSnapshot(current) &&
            publishStatus(lastMotion, motionConnected, current, motionReceivedAtMs)) {
            lastPublishedAtMs_ = nowMs;
            published_ = true;
        }
    }
}

} }
#endif
