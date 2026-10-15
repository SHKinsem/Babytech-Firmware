#pragma once

#include <cstdint>

namespace babytech { namespace brain {

class BrainTouchStop {
public:
    void beginPass() { requested_ = false; }
    bool requested() const { return requested_; }

    template<class Local, class Simulation>
    bool dispatch(Local& local, Simulation* simulation, uint32_t nowMs) {
        // A Stop click wins over any ordinary intent captured in the same pass,
        // including when its target is unavailable or transmission is rejected.
        requested_ = true;
        if (simulation && simulation->enabled()) return simulation->stopLocal(nowMs);
        return local.stop(nowMs);
    }

private:
    bool requested_ = false;
};

} }
