#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace babytech { namespace brain {

// Local USB only. Hex preserves spaces/UTF-8 without shell quoting or echoing
// credentials. It is transport encoding, not encryption.
class BrainNetworkConsole {
public:
    template<class Network>
    static bool handle(const char* line, bool maintenance, Network& network,
                       char* output, size_t capacity) {
        if (!line || std::strncmp(line, "NET ", 4)) return false;
        const auto reply = [&](const char* result) {
            std::snprintf(output, capacity, "[network] %s\n", result);
            return true;
        };
        if (!std::strcmp(line, "NET STATUS"))
            return reply(network.started() ? "brain_ready" : "brain_unpaired");
        if (!std::strcmp(line, "NET DIAG")) {
            network.diagnostics(output, capacity);
            return true;
        }
        if (!maintenance) return reply("maintenance_required");
        Fields fields;
        if (!std::strncmp(line, "NET WIFI ", 9)) {
            const char* at = line + 9;
            if (!decode(at, fields.ssid) || !decode(at, fields.password, true) || *at)
                return reply("invalid_config");
            return reply(network.configureWifi(fields.ssid, fields.password) ? "wifi_saved" : "wifi_failed");
        }
        if (!std::strncmp(line, "NET MQTT ", 9)) {
            const char* at = line + 9;
            if (!decode(at, fields.host)) return reply("invalid_config");
            uint32_t port = 0;
            unsigned digits = 0;
            while (*at >= '0' && *at <= '9' && digits < 5) {
                port = port * 10 + static_cast<unsigned>(*at++ - '0');
                ++digits;
            }
            if (!digits || !port || port > 65535 || *at++ != ' ' ||
                !decode(at, fields.user) || !decode(at, fields.password) || *at)
                return reply("invalid_config");
            return reply(network.configureMqtt(fields.host, static_cast<uint16_t>(port),
                                                fields.user, fields.password) ? "mqtt_saved" : "mqtt_failed");
        }
        return reply("unknown_command");
    }

private:
    struct Fields {
        char ssid[33]{}, host[128]{}, user[64]{}, password[128]{};
        ~Fields() {
            volatile unsigned char* p = reinterpret_cast<volatile unsigned char*>(this);
            for (size_t i = 0; i < sizeof(*this); ++i) p[i] = 0;
        }
    };
    static int hex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    }
    template<size_t N>
    static bool decode(const char*& at, char (&out)[N], bool allowEmpty = false) {
        if (allowEmpty && *at == '-' && (!at[1] || at[1] == ' ')) {
            ++at;
            if (*at == ' ') ++at;
            return true;
        }
        size_t length = 0;
        while (*at && *at != ' ') {
            const int hi = hex(*at++);
            if (hi < 0 || !*at) return false;
            const int lo = hex(*at++);
            if (lo < 0 || length == N - 1 || !(hi || lo)) return false;
            out[length++] = static_cast<char>((hi << 4) | lo);
        }
        if (*at == ' ') ++at;
        return length != 0;
    }
};

} }
