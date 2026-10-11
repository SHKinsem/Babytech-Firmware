#include "FakeMotionIo.h"
#include <esp_mac.h>
#include <stdexcept>
unsigned long millis() { return motion_io::now; }
void delay(unsigned long ms) { if (!motion_io::freezeClock) motion_io::now += uint32_t(ms); }
void delayMicroseconds(unsigned) {}
void pinMode(int, int) {}
void digitalWrite(int pin, int level) {
    ++motion_io::gpioWrites;
    if (motion_io::hxReady && motion_io::critical && pin == 2 && level == HIGH)
        ++motion_io::hxPulses;
}
int digitalRead(int pin) {
    if (pin == 21) return motion_io::lowWaterLevel; // Explicit host low-water fixture only.
    if (pin == 1 && motion_io::hxReady) {
        if (!motion_io::critical)
            return int32_t(motion_io::now - motion_io::hxNextConversionAt) >= 0 ? LOW : HIGH;
        if (motion_io::hxBit < 0) throw std::runtime_error("HX711 read beyond 24 data bits");
        return int((motion_io::hxLatched >> motion_io::hxBit--) & 1u);
    }
    return HIGH; // HX711 absent, never fabricate a weight.
}
void noInterrupts() {
    motion_io::critical = true;
    if (motion_io::hxReady) {
        motion_io::hxLatched = uint32_t(motion_io::hxRaw) & 0xffffffu;
        motion_io::hxBit = 23; motion_io::hxPulses = 0;
    }
}
void interrupts() {
    if (motion_io::hxReady) {
        if (motion_io::hxBit != -1 || motion_io::hxPulses != 25)
            throw std::runtime_error("HX711 sample must clock 24 bits plus gain pulse");
        ++motion_io::hxSamples;
        motion_io::hxNextConversionAt = motion_io::now + 13; // One conversion, approximately 80 SPS.
    }
    motion_io::critical = false;
}
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
        if (f->data[0] == 0x3a) reply(id, {0x3a, uint8_t(flowFeedback ? driverFlags[id] : 1), 0x6b});
        if (f->data[0] == 0x3b) reply(id, {0x3b, 0, 0x6b});
    }
    if (flowFeedback && automaticFeedback && id && id != missingId && !(f->identifier & 255)) {
        if (homeCompletionReply && f->data_length_code == 4 && f->data[0] == 0x9a && f->data[1] == 2 &&
            f->data[2] == 0 && f->data[3] == 0x6b)
            reply(id, {0x9a, 0x9f, 0x6b});
        if (f->data_length_code == 5 && f->data[0] == 0xf3 && f->data[1] == 0xab &&
            f->data[2] <= 1 && f->data[3] == 0 && f->data[4] == 0x6b) {
            driverFlags[id] = uint8_t((driverFlags[id] & ~1u) | (f->data[2] ? 1u : 0u));
            reply(id, {0xf3, 2, 0x6b});
        }
        if (markerReply && f->data_length_code == 3 && f->data[0] == 0x50 && f->data[1] == 1 && f->data[2] == 0x6b) {
            driverFlags[id] |= 0x80;
            reply(id, {0x50, 2, 0x6b});
        }
    }
    return ESP_OK;
}
esp_err_t twai_receive(twai_message_t* out, unsigned long) {
    if (motion_io::canRx.empty()) return ESP_FAIL;
    *out = motion_io::canRx.front(); motion_io::canRx.pop_front(); return ESP_OK;
}
