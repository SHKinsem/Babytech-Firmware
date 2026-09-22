#pragma once
#include <stdint.h>
using esp_err_t=int;
using gpio_num_t=int;
constexpr int ESP_OK=0,TWAI_MODE_NORMAL=0;
enum {TWAI_STATE_STOPPED,TWAI_STATE_RUNNING,TWAI_STATE_BUS_OFF,TWAI_STATE_RECOVERING};
struct twai_general_config_t { int tx_queue_len=0,rx_queue_len=0; };
struct twai_timing_config_t {};
struct twai_filter_config_t {};
inline twai_general_config_t TWAI_GENERAL_CONFIG_DEFAULT(int,int,int) {return {};}
inline twai_timing_config_t TWAI_TIMING_CONFIG_1MBITS() {return {};}
inline twai_timing_config_t TWAI_TIMING_CONFIG_500KBITS() {return {};}
inline twai_filter_config_t TWAI_FILTER_CONFIG_ACCEPT_ALL() {return {};}
inline uint32_t pdMS_TO_TICKS(uint32_t ms) {return ms;}
struct twai_message_t { uint32_t identifier=0; uint8_t extd=0,rtr=0,ss=0,data_length_code=0; uint8_t data[8]{}; };
struct twai_status_info_t {
    int state=TWAI_STATE_RUNNING;
    uint32_t tx_error_counter=0,rx_error_counter=0,tx_failed_count=0,rx_missed_count=0,rx_overrun_count=0,bus_error_count=0;
};
inline esp_err_t twai_stop() {return ESP_OK;}
inline esp_err_t twai_start() {return ESP_OK;}
inline esp_err_t twai_driver_uninstall() {return ESP_OK;}
inline esp_err_t twai_driver_install(const twai_general_config_t*,const twai_timing_config_t*,const twai_filter_config_t*) {return ESP_OK;}
inline esp_err_t twai_get_status_info(twai_status_info_t* out) {*out={};return ESP_OK;}
inline esp_err_t twai_receive(twai_message_t*,uint32_t) {return -1;}
esp_err_t twai_transmit(const twai_message_t*,uint32_t);
