#include "brain_network.h"

#if BABYTECH_BOARD_LINK_V4
#include "brain_status.h"
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
        const size_t topicLength = std::strlen(inbound_.topic);
        if (topicLength < 7 || std::strcmp(inbound_.topic + topicLength - 7, "/config")) continue;
        incomingJson_.clear();
        if (deserializeJson(incomingJson_, inbound_.payload)) continue;
        if (!incomingJson_.is<JsonObject>() || incomingJson_.size() != 4) continue;
        const char* type = text(incomingJson_["type"], 32);
        const char* device = text(incomingJson_["device_id"], 64);
        const char* target = text(incomingJson_["command_session"], 32);
        const char* challenge = text(incomingJson_["challenge"], 32);
        if (!type || std::strcmp(type, "command_session_probe") || !device ||
            std::strcmp(device, cloud_.deviceId()) || !target || !challenge) continue;
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
