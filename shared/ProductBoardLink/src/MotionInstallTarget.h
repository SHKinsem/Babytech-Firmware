#pragma once

#ifdef ARDUINO
#include "BoardInstall.h"
#include "BoardMaintenance.h"
#include <cstring>

namespace babytech { namespace boardlink {

// The Motion loop owns this target and its existing runtime store. A completed
// import does not change the running UART identity; activation follows the
// Brain's durable verification and controlled restart, not a received packet.
class MotionInstallTarget : public BoardInstallTarget, private CommissioningGuard {
public:
    MotionInstallTarget(MotionStateStore& store, BoardMaintenance& maintenance,
                        bool (*safeNow)(), uint32_t (*nowMs)())
        : store_(store), maintenance_(maintenance), safeNow_(safeNow), nowMs_(nowMs) {}
    CommissioningResult install(const CommissioningImport& request, const char* nonce,
                                uint64_t requesterBoot) override {
        if (!nonce || std::strlen(nonce) != 32) return CommissioningResult::Invalid;
        std::memcpy(nonce_, nonce, sizeof(nonce_));
        requesterBoot_ = requesterBoot;
        const auto result = importer_.importMotion(request, store_, *this);
        std::memset(nonce_, 0, sizeof(nonce_));
        requesterBoot_ = 0;
        return result;
    }
    bool faulted() const { return importer_.faulted() || store_.faulted(); }
private:
    bool allowImport(const CommissioningImport& request) override {
        if (!nowMs_) return false;
        maintenance_.poll(nowMs_());
        return safeNow_ && safeNow_() &&
            maintenance_.owns(request.pairing.deviceId, nonce_, requesterBoot_,
                              request.pairing.peerPhysicalId);
    }
    MotionStateStore& store_;
    BoardMaintenance& maintenance_;
    bool (*safeNow_)();
    uint32_t (*nowMs_)();
    BoardCommissioning importer_{};
    char nonce_[33]{};
    uint64_t requesterBoot_ = 0;
};

} }
#endif
