#pragma once

namespace motion {

enum class CloudInboundLane { None, Stop, Config, Command };

constexpr CloudInboundLane selectCloudInbound(bool stopWaiting, bool configWaiting,
                                              bool commandWaiting) {
    if (stopWaiting) return CloudInboundLane::Stop;
    if (configWaiting) return CloudInboundLane::Config;
    return commandWaiting ? CloudInboundLane::Command : CloudInboundLane::None;
}

}  // namespace motion
