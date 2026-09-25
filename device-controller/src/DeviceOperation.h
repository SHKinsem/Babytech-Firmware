#pragma once

#include <stdint.h>

namespace motion {

// In-process manual operation identity. These IDs are not included in the
// existing HTTP or UART protocols and do not promise uniqueness across reboot.
enum class DeviceOperationKind : uint8_t { None, Move, DirectPosition, Home };
enum class DeviceOperationState : uint8_t {
    Unknown, Running, Reached, NoMotion, Cancelled, Failed,
    Superseded, Invalidated, Evicted, Expired
};
enum class DeviceOperationFault : uint8_t {
    None, DriverDisabled, FeedbackStale, MoveAckTimeout, MoveTimeout,
    HomeAckTimeout, HomeStatusMissing, HomeFailed, HomeProtection, HomeTimeout,
    AckRejected, BusOff
};

struct DeviceOperationResult {
    uint64_t operationId = 0;
    uint8_t motorId = 0;
    DeviceOperationKind kind = DeviceOperationKind::None;
    DeviceOperationState state = DeviceOperationState::Unknown;
    DeviceOperationFault fault = DeviceOperationFault::None;
    // True for a matching Received/Completed (0x02/0x9F) reply. The documented
    // 0x12/0x22 no-motion replies have their own NoMotion state instead.
    bool protocolAck = false;
    // True only after the existing fresh-feedback completion rule succeeds.
    bool reached = false;
};

} // namespace motion
