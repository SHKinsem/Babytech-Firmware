#pragma once

// Typed device-control boundary for the current board implementation. An
// admitted request is not a CAN acknowledgement or evidence of shaft motion.
// Source adapters retain their existing UART/HTTP/Demo ownership checks.

#include <stddef.h>
#include <stdint.h>

#include "CommandQueue.h"

namespace motion {

enum class DeviceAdmission : uint8_t {
    // Processed includes an idle cancel which can still transmit a stop frame.
    Accepted, Processed, Invalid, Busy, Unavailable, Failed
};

struct DeviceReceipt {
    DeviceAdmission admission;
    uint16_t code;       // Existing HTTP-compatible result code.
    const char* message; // Existing stable reason; consume before the next request.
    uint32_t runId;      // Nonzero only for an accepted program or Demo start.
    bool accepted() const { return admission == DeviceAdmission::Accepted || admission == DeviceAdmission::Processed; }
};

// Reached is used only for the manual supervisor's post-command evidence. A
// protocol ACK alone never changes a manual operation to Reached.
enum class DeviceMoveStage : uint8_t { None, Running, Reached, Cancelled, Failed };
enum class DeviceHomeStage : uint8_t { None, Running, Reached, NoMotion, Cancelled, Failed };

// Program Done means the queue finished its own send/optional-await contract;
// a program without await can finish while a motor is still moving.
enum class DeviceProgramStage : uint8_t { Idle, Running, Done, Failed, Cancelled };

struct DeviceMotorObservation {
    bool positionValid = false;
    int32_t positionTenths = 0;
    uint32_t positionAgeMs = UINT32_MAX;
    bool velocityValid = false;
    int32_t velocityTenthsRpm = 0;
    uint32_t velocityAgeMs = UINT32_MAX;
    bool currentValid = false;
    uint16_t currentMa = 0;
    uint32_t currentAgeMs = UINT32_MAX;
    bool enabledConfirmed = false;
    bool enablePending = false;
    bool enableProtocolAck = false;
    bool enableTimedOut = false;
    bool stopPending = false;
    bool fault = false;
};

struct DeviceSnapshot {
    uint32_t sampledAtMs = 0;
    uint8_t motorId = 0;
    bool busReady = false;
    bool manualBusy = false;
    bool hasActiveMotion = false;
    bool stopping = false;
    bool fault = false;
    // Existing manual supervisors are board-global. These fields are not
    // attributed to motorId; per-operation IDs/history belong to a later slice.
    DeviceMoveStage manualMove = DeviceMoveStage::None;
    DeviceHomeStage manualHome = DeviceHomeStage::None;
    uint8_t homeId = 0;
    DeviceProgramStage program = DeviceProgramStage::Idle;
    uint32_t programRunId = 0;
    uint16_t programErrorLine = 0;
    char programMessage[64] = {};
    char faultTag[48] = {};
    DeviceMotorObservation motor;
};

class DeviceAPI {
public:
    DeviceAPI(MotorControl& motor, CommandQueue& queue) : motor_(motor), queue_(queue) {}

    // Lifecycle helpers allow host contract tests to drive the real composition.
    bool begin(int tx, int rx, long bitrate);
    void poll(uint32_t now, bool dispatchQueries = true);

    DeviceReceipt requestEnable(uint8_t id, bool enabled);
    DeviceReceipt requestBroadcastEnable(bool enabled);
    DeviceReceipt requestMove(const MoveRequest& request);
    DeviceReceipt requestDirectPosition(const DirectPositionRequest& request);
    DeviceReceipt requestHome(uint8_t id, uint8_t mode);
    DeviceReceipt requestStop(uint8_t id);
    DeviceReceipt requestStopAll();
    DeviceReceipt requestRawCommand(const uint8_t* bytes, uint8_t length);
    // Demo's volatile 0x50 marker uses queue transport, not the manual raw
    // command gate. The caller still verifies post-write driver flags.
    bool requestDemoMarker(uint8_t id);
    DeviceReceipt startProgram(const char* text, size_t length, long repeat,
                               const QueueRotationSource& rotation, uint32_t now);
    DeviceReceipt startDemo(const QueueProgram& program, uint32_t now);
    DeviceReceipt cancelProgram(const char* reason);
    DeviceReceipt clearControlState();

    // No CAN TX, query scheduling or mutation occurs while producing a snapshot.
    // sampledAtMs is read from millis() to match MotorControl feedback ages.
    DeviceSnapshot readSnapshot(uint8_t id) const;
    // Cheap per-ID read for stop/feedback loops: no board-wide state scan.
    DeviceMotorObservation readMotorObservation(uint8_t id) const;

private:
    static DeviceReceipt receipt(Result result, uint32_t runId = 0);
    MotorControl& motor_;
    CommandQueue& queue_;
};

} // namespace motion
