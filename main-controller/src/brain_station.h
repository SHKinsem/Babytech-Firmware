#pragma once

#include <atomic>
#include <cstdint>

namespace babytech { namespace brain {

// Only poll/service touch the radio and active credentials. configure may run
// on the UI/USB owner; concurrent configuration attempts return false, not wait.
class BrainStation {
public:
    static void service(void* context);
    void poll();
    // SSID: 1..32 bytes; password: empty, 8..63 bytes, or 64 hex digits.
    // True means set + commit + readback succeeded, not that Wi-Fi connected.
    // Legacy ssid/pass keys are NOT a multi-key transaction: errors/power loss
    // may leave a partial pair on flash (including after a false result).
    // No rollback/durability guarantee beyond NVS. Reconfigure to repair;
    // failure leaves this instance's verified configuration/reload unchanged.
    bool configure(const char* ssid, const char* password);
    bool copySsid(char (&output)[33]); // UI owner, no password exposure
    bool hasCredentials() const { return hasCredentials_.load(); }
    bool credentialsKnown() const { return credentialsKnown_.load(); }
    bool connected() const { return !reloadRequested_.load() && connected_.load(); }
    void provisioningAp(bool enabled); // network owner only
private:
    void loadSnapshot(); // configBusy_ held; never touches worker radio buffers.
    std::atomic_flag configBusy_ = ATOMIC_FLAG_INIT;
    std::atomic<bool> reloadRequested_{false};
    // Single verified mailbox, separate from worker-owned ssid_/password_.
    char savedSsid_[33]{};
    char savedPassword_[65]{};
    bool snapshotLoaded_ = false;
    bool snapshotValid_ = false;
    char ssid_[33]{};
    char password_[65]{};
    bool loaded_ = false;
    bool configured_ = false;
    bool apActive_ = false;
    std::atomic<bool> hasCredentials_{false}, credentialsKnown_{false}, connected_{false};
    bool joining_ = false;
    bool attempted_ = false;
    uint32_t attemptAtMs_ = 0;
};

} }
