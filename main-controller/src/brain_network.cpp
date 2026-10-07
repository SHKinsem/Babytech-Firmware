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
bool boundedText(const char* value, size_t maximum) {
    if (!value || !value[0]) return false;
    size_t length = 0;
    while (length <= maximum && value[length]) ++length;
    return length <= maximum && v4::validUtf8(reinterpret_cast<const uint8_t*>(value), length);
}
bool validSession(const char* session) {
    if (!boundedText(session, 32) || std::strlen(session) != 32) return false;
    bool nonzero = false;
    for (size_t i = 0; i < 32; ++i) {
        if (!((session[i] >= '0' && session[i] <= '9') ||
              (session[i] >= 'a' && session[i] <= 'f'))) return false;
        nonzero |= session[i] != '0';
    }
    return nonzero;
}
bool cloudCommandName(const char* command) {
    if (!boundedText(command, 21)) return false;
    return !std::strcmp(command, "prepare") || !std::strcmp(command, "clean") ||
        !std::strcmp(command, "set_target_temp") || !std::strcmp(command, "reset_error") ||
        !std::strcmp(command, "check_firmware_update") || !std::strcmp(command, "stop");
}
bool validReason(const char* reason) {
    if (!boundedText(reason, 64)) return false;
    for (const char* at = reason; *at; ++at)
        if (!((*at >= 'a' && *at <= 'z') || (*at >= 'A' && *at <= 'Z') ||
              (*at >= '0' && *at <= '9') || *at == '_')) return false;
    return true;
}
}

bool BrainNetwork::begin(const char* pairedDeviceId) {
    if (started_ || !pairedDeviceId || !pairedDeviceId[0]) return false;
    started_ = cloud_.beginV4(pairedDeviceId, BrainStation::service, &station_);
    return started_;
}

void BrainNetwork::setProductHandlers(CommandHandler command, StopHandler stop, void* context) {
    commandHandler_ = command;
    stopHandler_ = stop;
    productContext_ = context;
}

void BrainNetwork::setContextHandler(ContextHandler handler, void* context) {
    contextHandler_ = handler;
    contextOwner_ = context;
}

cloud::Freshness BrainNetwork::checkFreshness(const char* session, uint32_t generation,
                                             uint32_t sampledAtMs, uint16_t ttlMs) {
    return cloud_.checkFreshness(session, generation, sampledAtMs, ttlMs);
}

bool BrainNetwork::publishStatus(const boardlink::Status* lastMotion, bool motionConnected,
                                const cloud::SessionSnapshot& session, uint32_t motionReceivedAtMs,
                                const char* challenge, bool commandsEnabled, bool canStart) {
    // CloudLink also checks this original receipt again at actual publish entry.
    const bool freshMotion = motionConnected && lastMotion &&
        uint32_t(session.uptimeMs - motionReceivedAtMs) < 1500;
    status_.clear();
    writeStatus(status_.to<JsonObject>(), cloud_.deviceId(), FIRMWARE_VERSION,
                lastMotion, freshMotion, session, challenge, commandsEnabled, canStart);
    if (!encodeStatusJson(status_, payload_, sizeof(payload_))) return false;
    CloudLink::StatusPublishOptions options;
    options.probeReply = challenge != nullptr;
    options.hasMotionSample = motionConnected && lastMotion;
    options.motionReceivedAtMs = motionReceivedAtMs;
    return cloud_.publishStatusForSession(String(payload_), session.generation, options);
}

bool BrainNetwork::publishAckForGeneration(const char* commandId, const char* command, uint64_t sequence,
                                           const char* session, bool accepted, const char* reason,
                                           uint32_t generation) {
    if (!started_ || !motion::validCloudDeviceId(cloud_.deviceId(), std::strlen(cloud_.deviceId())) ||
        !boundedText(commandId, 128) || !cloudCommandName(command) ||
        !sequence || sequence > v4::kMaxSequence || !validSession(session) || !validReason(reason)) return false;
    char sequenceText[20]{};
    const int length = std::snprintf(sequenceText, sizeof(sequenceText), "%llu",
                                    static_cast<unsigned long long>(sequence));
    if (length <= 0 || size_t(length) >= sizeof(sequenceText)) return false;
    status_.clear();
    status_["device_id"] = cloud_.deviceId();
    status_["command_id"] = commandId;
    status_["command"] = command;
    status_["command_seq"] = static_cast<const char*>(sequenceText);
    status_["command_session"] = session;
    status_["accepted"] = accepted;
    status_["reason"] = reason;
    if (!encodeStatusJson(status_, payload_, sizeof(payload_))) return false;
    return cloud_.publishForSession("ack", String(payload_), generation);
}

bool BrainNetwork::publishAck(const char* commandId, const char* command, uint64_t sequence,
                              const char* originalSession, bool accepted, const char* reason) {
    cloud::SessionSnapshot current;
    if (!cloud_.sessionSnapshot(current)) return false;
    return publishAckForGeneration(commandId, command, sequence, originalSession, accepted, reason,
                                   current.generation);
}

void BrainNetwork::rejectCommand(const char* commandId, const char* command, uint64_t sequence,
                                 const char* session, cloud::Freshness freshness) {
    if (freshness != cloud::Freshness::Current && freshness != cloud::Freshness::Expired) return;
    // This is an explicit local rejection, never a fabricated Motion acceptance.
    // A reconnect drops this reply instead of publishing it in another generation.
    publishAckForGeneration(commandId, command, sequence, session, false,
        freshness == cloud::Freshness::Expired ? "request_expired" : "integration_not_ready",
        inbound_.generation);
}

void BrainNetwork::receiveCommand(uint32_t nowMs) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(inbound_.payload);
    const size_t length = std::strlen(inbound_.payload);
    boardlink::CloudCommand command;
    if (boardlink::decodeCloudCommand(bytes, length, cloud_.deviceId(), command)) {
        const auto freshness = checkFreshness(command.session, inbound_.generation, command.sampledAtMs, command.ttlMs);
        if (commandHandler_ && (freshness == cloud::Freshness::Current || freshness == cloud::Freshness::Expired)) {
            commandHandler_(productContext_, command, inbound_.generation, nowMs);
            return;
        }
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
                      command.session, freshness);
        return;
    }
    boardlink::CloudStop stop;
    if (boardlink::decodeCloudStop(bytes, length, cloud_.deviceId(), stop)) {
        const auto freshness = checkFreshness(stop.session, inbound_.generation, stop.sampledAtMs, stop.ttlMs);
        if (stopHandler_ && (freshness == cloud::Freshness::Current || freshness == cloud::Freshness::Expired)) {
            stopHandler_(productContext_, stop, inbound_.generation, nowMs);
            return;
        }
        rejectCommand(stop.commandId, "stop", stop.sequence, stop.session, freshness);
    }
}

void BrainNetwork::poll(const boardlink::Status* lastMotion, bool motionConnected, uint32_t nowMs,
                        uint32_t motionReceivedAtMs, bool commandsEnabled, bool canStart) {
    if (!started_) return;
    cloud::SessionSnapshot current;
    const bool hasSession = cloud_.sessionSnapshot(current);
    if (!hasSession) {
        published_ = false;
        attempted_ = false;
    } else if (generation_ != current.generation) {
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
            receiveCommand(nowMs);
            continue;
        }
        if (std::strcmp(suffix, "/config")) continue;
        if (contextHandler_ && boardlink::decodeProductContext(
                reinterpret_cast<const uint8_t*>(inbound_.payload), std::strlen(inbound_.payload),
                device, contextScratch_)) {
            // Decode may span a reconnect. Do not deliver an old generation;
            // config delivery does not authorize commands or require their TTL.
            if (inbound_.generation == cloud_.sessionGeneration())
                contextHandler_(contextOwner_, contextScratch_, inbound_.generation, nowMs);
            continue;
        }
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
            publishStatus(lastMotion, motionConnected, reply.session, motionReceivedAtMs, reply.challenge,
                          commandsEnabled, canStart);
        // Other config messages cannot dispatch actions or mutate NVS here.
    }
    if (!hasSession) return;
    if ((!published_ || uint32_t(nowMs - lastPublishedAtMs_) >= 2000) &&
        (!attempted_ || uint32_t(nowMs - lastAttemptAtMs_) >= 250)) {
        attempted_ = true;
        lastAttemptAtMs_ = nowMs;
        if (cloud_.sessionSnapshot(current) &&
            publishStatus(lastMotion, motionConnected, current, motionReceivedAtMs, nullptr,
                          commandsEnabled, canStart)) {
            lastPublishedAtMs_ = nowMs;
            published_ = true;
        }
    }
}

} }
#endif
