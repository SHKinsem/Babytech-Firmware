#include "CloudIdentity.h"
#include "CloudLink.h"
#include "CloudCommandPriority.h"
#include "CloudConfigKind.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

int main() {
    using motion::validCloudDeviceId;
    assert(!validCloudDeviceId(nullptr, 1));
    assert(!validCloudDeviceId("", 0));
    for (size_t length = 1; length <= 64; ++length) {
        const std::string id(length, 'a');
        assert(validCloudDeviceId(id.data(), id.size()));
    }
    const std::string tooLong(65, 'a');
    assert(!validCloudDeviceId(tooLong.data(), tooLong.size()));
    assert(validCloudDeviceId("bt-184DCE6E27AC", 15));
    assert(validCloudDeviceId("A0_z-9", 6));
    // Exhaust every byte in the first and subsequent positions, including NUL.
    for (unsigned int byte = 0; byte <= 255; ++byte) {
        const char c = static_cast<char>(byte);
        const bool alphanumeric = (byte >= 'a' && byte <= 'z') ||
            (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9');
        assert(validCloudDeviceId(&c, 1) == alphanumeric);
        const char id[] = {'A', c, '0'};
        assert(validCloudDeviceId(id, sizeof(id)) ==
               (alphanumeric || byte == '_' || byte == '-'));
    }

    const std::string id = "A" + std::string(61, 'z') + "_-";
    assert(id.size() == 64 && validCloudDeviceId(id.data(), id.size()));
    char prefix[motion::kCloudTopicPrefixCapacity];
    const int prefixSize = std::snprintf(prefix, sizeof(prefix), "devices/%s/", id.c_str());
    assert(prefixSize == 73 && prefixSize < static_cast<int>(sizeof(prefix)));
    assert(std::string(prefix) == "devices/" + id + "/");
    static_assert(sizeof(CloudLink::Inbound::topic) == motion::kCloudTopicCapacity,
                  "The production inbound topic must hold a full device ID");
    for (const char* suffix : {"command", "config", "status", "ack", "event"}) {
        CloudLink::Inbound message;
        const int size = std::snprintf(message.topic, sizeof(message.topic), "%s%s", prefix, suffix);
        assert(size > 0 && size < static_cast<int>(sizeof(message.topic)));
        assert(std::string(message.topic) == "devices/" + id + "/" + suffix);
    }

    const std::string stop = "{\"command\":\"stop\",\"command_id\":\"" +
        std::string(128, 'c') + "\",\"device_id\":\"" + id + "\"}";
    assert(motion::isPriorityStopCommand(
        reinterpret_cast<const uint8_t*>(stop.data()), stop.size(), id.c_str()));
    for (const char* type : {"feeding_context", "feeding_event_receipt"}) {
        const std::string payload = "{\"type\":\"" + std::string(type) +
            "\",\"device_id\":\"" + id + "\"}";
        const auto kind = motion::cloudConfigKind(
            reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), id.c_str());
        assert(kind == (std::strcmp(type, "feeding_context") == 0
            ? motion::CloudConfigKind::Context : motion::CloudConfigKind::Receipt));
        assert(motion::cloudConfigKind(reinterpret_cast<const uint8_t*>(payload.data()),
            payload.size(), id.substr(0, 23).c_str()) == motion::CloudConfigKind::Invalid);
    }
    const std::string probe = "{\"type\":\"command_session_probe\",\"device_id\":\"" + id + "\"}";
    const auto* bytes = reinterpret_cast<const uint8_t*>(probe.data());
    assert(motion::cloudConfigKind(bytes, probe.size(), id.c_str()) == motion::CloudConfigKind::Invalid);
    assert(motion::cloudConfigKind(bytes, probe.size(), id.c_str(), true) ==
           motion::CloudConfigKind::SessionProbe);
    assert(motion::cloudConfigKind(bytes, probe.size(), "wrong", true) == motion::CloudConfigKind::Invalid);
    std::puts("PASS cloud identity boundaries, ASCII bytes, full-length topics and opt-in probes");
}
