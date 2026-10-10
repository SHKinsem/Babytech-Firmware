#pragma once

namespace babytech::display {

// The V1 display controller is a fixed board design, so its local UART pins
// are intentionally not build-time options.
constexpr int kControllerUartTxPin = 43;
constexpr int kControllerUartRxPin = 44;
constexpr uint32_t kControllerUartBaud = 115200;
constexpr uint32_t kControllerOfflineTimeoutMs = 2500;
constexpr uint32_t kIntentRetryIntervalMs = 500;
constexpr uint8_t kIntentRetryLimit = 2;

}  // namespace babytech::display
