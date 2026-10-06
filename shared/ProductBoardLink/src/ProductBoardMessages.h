#pragma once

#include "BoardSessionV4.h"
#include "ProductRequest.h"
#include "display_model.h"

namespace babytech { namespace boardlink {

// Flat JSON objects, all fields required; unknown/duplicate fields rejected.
// HELLO/HELLO_ACK: protocol (integer 4), role ("brain" or "motion"),
// capabilities (uint32, bit 0 set), device_id (1..64 ASCII, validHello rules),
// pairing_epoch (32 lower hex), physical_id (12 lower hex). Hex IDs nonzero.
// reply_to (required uint32): HELLO must use 0; HELLO_ACK must use 1..UINT32_MAX.
// Session, not this codec, checks reply_to against the current local HELLO ID.
// STATUS: schema_version (= display::kDisplaySchemaVersion), stage,
// primary_condition, footer_condition, error (integer display enum values),
// cloud_connected, start_enabled, thermal_simulated (bool), water_ml (uint16),
// temperature_c (int16), baby_name, formula_brand (UTF-8, <=31 bytes each),
// sample_uptime_ms (uint32), context_version (0..INT32_MAX),
// cloud_watermark, local_watermark (canonical decimal strings 0..INT64_MAX),
// motion_busy, stationary, event_pending (bool), active_execution_id
// (empty or nonzero 32 lower hex), pending_event_id (UTF-8, 0..128 bytes).
// STATUS adds 14 required product fields (35 total), still UART v4:
// product_progress (ready/noready/error/cleaning/unscrewing_cap/dispensing_water/
// dispensing_powder/screwing_cap/mixing/complete), product_error (1..39 bytes,
// [A-Z0-9_]+, including NONE and extensible E_* codes), is_preparing (bool),
// low_water_valid, low_water, powder_valid (bool), powder_grams (0..INT32_MAX),
// actuator_operational, actuator_config_valid, actuator_bus_healthy,
// actuator_position_referenced, execution_authorized (bool),
// feeding_context_configured (bool), baby_id (UTF-8, 0..96 bytes, full ID).
// All strings reject embedded NUL and invalid UTF-8; there is no truncation.
// Individually valid fields may exceed the 2047-byte escaped payload budget
// in combination; encoding rejects that whole status atomically.
// Display names are display-only, NOT configuration or transport identities.
// STATUS is observational: no command, action ACK, initialization success,
// NVS or cloud behavior is implemented; start_enabled is not authorization.
struct Status {
    display::DisplaySnapshot snapshot{};
    uint32_t sampleUptimeMs = 0;
    uint32_t contextVersion = 0;
    char cloudWatermark[20] = "0";
    char localWatermark[20] = "0";
    bool motionBusy = false;
    bool stationary = false;
    bool eventPending = false;
    char activeExecutionId[33]{};
    char pendingEventId[129]{};
    char productProgress[24] = "noready";
    char productError[40] = "NONE";
    bool isPreparing = false;
    bool lowWaterValid = false;
    bool lowWater = false;
    bool powderValid = false;
    int32_t powderGrams = 0;
    bool actuatorOperational = false;
    bool actuatorConfigValid = false;
    bool actuatorBusHealthy = false;
    bool actuatorPositionReferenced = false;
    bool executionAuthorized = false;
    bool feedingContextConfigured = false;
    char babyId[97]{};

    Status() { snapshot.stage = display::DisplayStage::NotReady; }
};

// Fixed-capacity ArduinoJson 6 documents; no dynamic JSON document allocation.
// False leaves the entire output unchanged. Successful encoding produces only
// kind/length/payload with zero boot IDs/message ID. The caller MUST set those
// IDs before v4::fragment(); JSON is decoded only after complete reassembly.
// Payload is length-delimited (<=v4::kMaxMessage), NOT necessarily NUL-terminated.
// Inputs must remain stable for the call. Encoders preflight the fully escaped
// length, then write directly; an impossible internal short write fails fast.
// Decoders validate payload/kind, NOT session authorization, freshness or IDs.
bool encodeHello(const v4::Hello& hello, v4::Message& output,
                 v4::Kind kind = v4::Kind::Hello);
bool decodeHello(const v4::Message& message, v4::Hello& output);
bool encodeStatus(const Status& status, v4::Message& output);
bool decodeStatus(const v4::Message& message, Status& output);

struct CommandMessage {
    ProductRequest request;
    uint16_t remainingTtlMs = 0;
};

struct CloudCommand {
    ProductRequest request;
    char session[33]{};
    uint32_t sampledAtMs = 0;
    uint16_t ttlMs = 0;
};

// Ordinary commands only; Stop remains the independent v4 binary control path.
// COMMAND uses source/seq and ttl_ms (remaining 1..5000ms), plus frozen prepare
// fields. Cloud uses command_seq/session/device_uptime_ms and fixed ttl=5000.
// Failed codecs leave output unchanged. They do not authorize or dispatch:
// owners still check MQTT generation/session, UART boots, elapsed transmission
// time, ownership and durable decisions before any mechanical action.
bool encodeCommand(const CommandMessage& command, v4::Message& output);
bool decodeCommand(const v4::Message& message, CommandMessage& output);
bool decodeCloudCommand(const uint8_t* bytes, size_t length, const char* expectedDeviceId,
                        CloudCommand& output);

} }  // namespace babytech::boardlink
