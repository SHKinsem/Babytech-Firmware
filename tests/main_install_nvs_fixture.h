#pragma once
#include "fakes/brain_state_store/FakeBrainNvs.h"

namespace main_install_nvs {
// Arm only after setup in explicit installation pipes. Business-state writes
// complete normally; the first pairing commit reports failure, optionally
// after applying its write to model an uncertain durable outcome.
class PairCommitFault {
public:
    void arm(bool enabled, bool applied = false) {
        if (!enabled) return;
        fake_brain::io.before = [this, applied](const fake_brain::Call& call) {
            if (!hit_ && call.op == fake_brain::Op::Commit && call.name == "productpair") {
                hit_ = true;
                fake_brain::fail(call.op, call.occurrence, ESP_FAIL, applied);
            }
        };
    }
    bool hit() const { return hit_; }
    ~PairCommitFault() { fake_brain::io.before = {}; }
private:
    bool hit_ = false;
};
}
