#pragma once
#include <stdint.h>
using esp_err_t = int;
using gpio_num_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, TWAI_MODE_NORMAL = 0;
enum { TWAI_STATE_STOPPED, TWAI_STATE_RUNNING, TWAI_STATE_BUS_OFF, TWAI_STATE_RECOVERING };
struct twai_message_t {
    uint32_t identifier = 0;
    uint8_t data[8] = {}, data_length_code = 0;
    bool extd = false, rtr = false, ss = false;
};
struct twai_general_config_t { int tx_queue_len = 0, rx_queue_len = 0; };
struct twai_timing_config_t {};
struct twai_filter_config_t {};
#define TWAI_GENERAL_CONFIG_DEFAULT(tx, rx, mode) twai_general_config_t{}
#define TWAI_TIMING_CONFIG_1MBITS() twai_timing_config_t{}
#define TWAI_TIMING_CONFIG_500KBITS() twai_timing_config_t{}
#define TWAI_FILTER_CONFIG_ACCEPT_ALL() twai_filter_config_t{}
#define pdMS_TO_TICKS(ms) (ms)
struct twai_status_info_t {
    int state = TWAI_STATE_RUNNING;
    uint32_t tx_error_counter = 0, rx_error_counter = 0, tx_failed_count = 0;
    uint32_t rx_missed_count = 0, rx_overrun_count = 0, bus_error_count = 0;
};
esp_err_t twai_stop();
esp_err_t twai_start();
esp_err_t twai_driver_uninstall();
esp_err_t twai_driver_install(const twai_general_config_t*, const twai_timing_config_t*, const twai_filter_config_t*);
esp_err_t twai_get_status_info(twai_status_info_t*);
esp_err_t twai_transmit(const twai_message_t*, unsigned long);
esp_err_t twai_receive(twai_message_t*, unsigned long);
