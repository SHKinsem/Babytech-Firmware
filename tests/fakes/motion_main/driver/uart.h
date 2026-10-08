#pragma once
#include <esp_err.h>
constexpr int UART_NUM_1 = 1;
inline esp_err_t uart_wait_tx_done(int port, unsigned) { return port == UART_NUM_1 ? ESP_OK : ESP_FAIL; }
