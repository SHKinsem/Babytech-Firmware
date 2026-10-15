#pragma once

#include <cstdint>
#include "esp_err.h"

enum uart_port_t { UART_NUM_1 = 1 };
using TickType_t = uint32_t;
esp_err_t uart_wait_tx_done(uart_port_t port, TickType_t ticks);
