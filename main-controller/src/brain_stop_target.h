#pragma once

#include "ProductBoardMessages.h"

namespace babytech { namespace brain {

template<class Link>
bool bindStopTarget(const Link& link, uint32_t nowMs, v4::StopRequest& target) {
    const auto* status = link.lastTelemetry();
    if (!link.connected(nowMs) || !status ||
        uint32_t(nowMs - link.lastTelemetryReceivedAtMs()) >= 1500) return false;
    v4::StopRequest selected;
    switch (status->executionOwner) {
        case boardlink::ExecutionOwner::None:
            if (status->activeExecutionId[0] || status->motionBusy || !status->stationary) return false;
            selected.scope = v4::StopScope::Idle;
            target = selected;
            return true;
        case boardlink::ExecutionOwner::Product: selected.scope = v4::StopScope::Product; break;
        case boardlink::ExecutionOwner::Workbench: selected.scope = v4::StopScope::Workbench; break;
        default: return false;
    }
    bool nonzero = false;
    for (size_t i = 0; i < sizeof(selected.executionId); ++i) {
        const auto nibble = [](char c) -> int {
            return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        };
        const int high = nibble(status->activeExecutionId[2 * i]);
        const int low = nibble(status->activeExecutionId[2 * i + 1]);
        if (high < 0 || low < 0) return false;
        selected.executionId[i] = uint8_t(high * 16 + low);
        nonzero |= selected.executionId[i] != 0;
    }
    if (!nonzero || status->activeExecutionId[32]) return false;
    target = selected;
    return true;
}

} }
