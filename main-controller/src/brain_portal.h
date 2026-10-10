#pragma once

#include "brain_portal_state.h"
#include <WebServer.h>
#include <WiFiUdp.h>

class CloudLink;
namespace babytech { namespace brain {
class BrainStation;

class BrainPortal {
public:
    void begin(const char* deviceId);
    void poll(BrainStation& station, CloudLink& cloud, bool feeding, bool bootDown, uint32_t nowMs);
    void service(BrainStation& station, bool mqttConnected); // sole network owner
    void configurationChanged() { refresh_ = true; } // UI/USB owner
    bool active() const { return state_.active(); }
private:
    bool localRequest();
    void routes();
    void status();
    void save(PortalSave kind);
    void dns();
    void reply(int code, const char* result);
    BrainPortalState state_;
    PortalInfo info_; // UI owner only
    WebServer server_{80};
    WiFiUDP dns_;
    std::atomic<bool> enabled_{false};
    bool refresh_ = true, routesReady_ = false, mqttConnected_ = false;
    bool wifiConnected_ = false;
    const char* finishResult_ = nullptr;
    bool finishSaved_ = false;
};
} }
