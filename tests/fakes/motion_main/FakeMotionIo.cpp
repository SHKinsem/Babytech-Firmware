#include "FakeMotionIo.h"
#include <esp_mac.h>
#include <stdexcept>
unsigned long millis() { return motion_io::now; }
void delay(unsigned long ms) { if (!motion_io::freezeClock) motion_io::now += uint32_t(ms); }
void delayMicroseconds(unsigned) {}
void pinMode(int, int) {}
void digitalWrite(int, int) { ++motion_io::gpioWrites; }
int digitalRead(int pin) {
    if (pin == 21) return motion_io::lowWaterLevel; // Explicit host low-water fixture only.
    return HIGH; // HX711 absent, never fabricate a weight.
}
void noInterrupts() { motion_io::critical = true; }
void interrupts() { motion_io::critical = false; }
esp_err_t esp_read_mac(uint8_t* out, esp_mac_type_t type) {
    if (!out || type != ESP_MAC_WIFI_STA) return ESP_FAIL;
    std::memcpy(out, motion_io::mac.data(), motion_io::mac.size()); return ESP_OK;
}
namespace motion_io {
void reply(uint8_t id, std::initializer_list<uint8_t> data) {
    twai_message_t f; f.extd = true; f.identifier = uint32_t(id) << 8;
    for (auto byte : data) f.data[f.data_length_code++] = byte;
    canRx.push_back(f);
}
}
esp_err_t twai_stop() { return ESP_OK; }
esp_err_t twai_start() { ++motion_io::canStarts; return ESP_OK; }
esp_err_t twai_driver_uninstall() { return ESP_OK; }
esp_err_t twai_driver_install(const twai_general_config_t*, const twai_timing_config_t*, const twai_filter_config_t*) { return ESP_OK; }
esp_err_t twai_get_status_info(twai_status_info_t* out) { *out = twai_status_info_t{}; return ESP_OK; }
esp_err_t twai_transmit(const twai_message_t* f, unsigned long) {
    using namespace motion_io;
    if (f->data_length_code && f->data[0] == rejectedOpcode &&
        (rejectedPacket < 0 || int(f->identifier & 255) == rejectedPacket)) return ESP_FAIL;
    canTx.push_back(*f);
    const auto id = uint8_t(f->identifier >> 8);
    if (automaticFeedback && id && id != missingId && !(f->identifier & 255) && f->data_length_code == 2) {
        if (f->data[0] == 0x36) reply(id, {0x36, 0, 0, 0, 0, 0, 0x6b});
        if (f->data[0] == 0x35) reply(id, {0x35, 0, 0, uint8_t(id == movingId ? 30 : 0), 0x6b});
        if (f->data[0] == 0x3a) reply(id, {0x3a, 1, 0x6b});
        if (f->data[0] == 0x3b) reply(id, {0x3b, 0, 0x6b});
    }
    return ESP_OK;
}
esp_err_t twai_receive(twai_message_t* out, unsigned long) {
    if (motion_io::canRx.empty()) return ESP_FAIL;
    *out = motion_io::canRx.front(); motion_io::canRx.pop_front(); return ESP_OK;
}
