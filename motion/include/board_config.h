#pragma once
#include <stddef.h>
#include <stdint.h>

// Board-level constants for the ESP32-S3 motion board.
// Values are physical GPIO numbers, not connector silkscreen labels unless noted.

// Brain link (legacy board protocol). Kept exactly as validated in the first slice.
constexpr int kLinkTxPin = 43;
constexpr int kLinkRxPin = 44;
constexpr uint32_t kLinkBaud = 115200;

// Motor CAN transceiver pins and bitrate.
// Carried over from the previously validated single-motor bench profile.
constexpr int kCanTxPin = 4;
constexpr int kCanRxPin = 5;
constexpr long kCanBitrate = 500000;

// Soft access point used by the bench debug page.
// The password must stay at 8 characters or more or the AP will not start.
constexpr char kApSsid[] = "Babytech-Motion";
constexpr char kApPassword[] = "babytech-demo";
constexpr uint8_t kApChannel = 1;
constexpr uint8_t kApMaxClients = 4;

// HTTP debug server. Default soft-AP address is 192.168.4.1.
constexpr uint16_t kHttpPort = 80;

// Upper bound of brain-link bytes consumed per loop pass, so CAN polling and
// HTTP handling can never be starved by a chatty or noisy link.
constexpr size_t kLinkBytesPerPass = 128;
