#pragma once
#include <WiFiClient.h>
#include <stdexcept>
// V4 Motion must never construct a product MQTT client. Legacy declarations
// remain in headers, but any accidental construction fails this host harness.
class PubSubClient {
public:
    explicit PubSubClient(WiFiClient&) { throw std::logic_error("v4 Motion attempted MQTT ownership"); }
};
