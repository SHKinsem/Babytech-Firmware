#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

namespace babytech { namespace brain {

enum class PortalSave { Wifi, Mqtt };
struct PortalRequest {
    PortalSave kind = PortalSave::Wifi;
    char hostOrSsid[128]{}, user[64]{}, password[128]{};
    uint16_t port = 1883;
};
struct PortalInfo {
    char deviceId[65]{}, developmentCode[7]{}, ssid[33]{};
    char host[128] = "101.33.219.108", user[64]{};
    uint16_t port = 1883;
    const char* result = "idle";
    bool feeding = false;
};

inline void wipePortalRequest(PortalRequest& request) {
    volatile unsigned char* bytes = reinterpret_cast<volatile unsigned char*>(&request);
    for (size_t n = 0; n < sizeof(request); ++n) bytes[n] = 0;
}

inline bool developmentPairingCode(const char* id, char (&output)[7]) {
    output[0] = 0;
    if (!id || std::strlen(id) != 15 || std::strncmp(id, "bt-", 3)) return false;
    uint32_t remainder = 0;
    for (size_t n = 3; n < 15; ++n) {
        const char c = id[n];
        const int digit = c >= '0' && c <= '9' ? c - '0' :
            c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) return false;
        remainder = (remainder * 16 + unsigned(digit)) % 1000000;
    }
    output[6] = 0;
    for (int n = 5; n >= 0; --n) { output[n] = char('0' + remainder % 10); remainder /= 10; }
    return true;
}

// Only copied fields cross owners. No waiting/spinning over NVS or HTTP I/O.
class BrainPortalState {
public:
    enum class Action { None, Start, Stop };
    void button(bool down, uint32_t nowMs) { // UI loop only
        if (!down) { held_ = fired_ = false; return; }
        if (!held_) { held_ = true; pressedAt_ = nowMs; }
        if (!fired_ && uint32_t(nowMs - pressedAt_) >= 5000) {
            fired_ = true;
            manualRequested_.store(true);
        }
    }
    bool publish(const PortalInfo& info) {
        Guard guard(lock_);
        if (!guard) return false;
        const char* result = info_.result;
        info_ = info;
        info_.result = result;
        return true;
    }
    bool snapshot(PortalInfo& info) {
        Guard guard(lock_);
        if (!guard) return false;
        info = info_;
        return true;
    }
    const char* submit(const PortalRequest& request) { // HTTP owner
        Guard guard(lock_);
        if (!guard || pending_ || saving_) return "busy";
        if (info_.feeding) return "feeding_active";
        request_ = request;
        pending_ = true;
        info_.result = "pending";
        return nullptr;
    }
    bool take(PortalRequest& request) { // UI owner
        Guard guard(lock_);
        if (!guard || !pending_) return false;
        request = request_;
        wipePortalRequest(request_);
        pending_ = false;
        saving_ = true;
        return true;
    }
    bool finish(const char* result, bool saved) {
        Guard guard(lock_);
        if (!guard) return false;
        info_.result = result;
        saving_ = false;
        if (saved) ++saveGeneration_;
        return true;
    }
    Action action(bool configured, bool connected, uint32_t nowMs) { // network owner
        if (manualRequested_.exchange(false)) {
            manual_ = true;
            savedAtEntry_ = saveGeneration_.load();
            closing_ = false;
        }
        if (!active_.load()) {
            if ((!configured || manual_) && (!attempted_ || uint32_t(nowMs - attemptAt_) >= 5000)) {
                attempted_ = true;
                attemptAt_ = nowMs;
                return Action::Start;
            }
            return Action::None;
        }
        if (connected && (!manual_ || saveGeneration_.load() != savedAtEntry_)) {
            if (!closing_) { closing_ = true; closeAt_ = nowMs; }
            // Let the HTTP reply/poll reach the phone before disabling its AP.
            if (uint32_t(nowMs - closeAt_) >= 2000) return Action::Stop;
        } else closing_ = false;
        return Action::None;
    }
    void started() { active_.store(true); }
    void stopped() { active_.store(false); manual_ = closing_ = false; attempted_ = false; }
    bool active() const { return active_.load(); }
private:
    class Guard {
    public:
        explicit Guard(std::atomic_flag& flag) : flag_(flag), held_(!flag.test_and_set(std::memory_order_acquire)) {}
        ~Guard() { if (held_) flag_.clear(std::memory_order_release); }
        explicit operator bool() const { return held_; }
    private:
        std::atomic_flag& flag_;
        bool held_;
    };
    std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    std::atomic<bool> manualRequested_{false}, active_{false};
    std::atomic<uint32_t> saveGeneration_{0};
    PortalInfo info_;
    PortalRequest request_;
    bool pending_ = false, saving_ = false;
    bool held_ = false, fired_ = false;
    uint32_t pressedAt_ = 0;
    bool manual_ = false, closing_ = false, attempted_ = false;
    uint32_t savedAtEntry_ = 0, closeAt_ = 0, attemptAt_ = 0;
};

} }
