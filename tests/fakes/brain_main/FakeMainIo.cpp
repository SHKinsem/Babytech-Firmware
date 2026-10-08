#include "FakeMainIo.h"
#include "FakeCloudIo.h"
#include "FakeBrainNvs.h"
#include "driver/uart.h"
#include <algorithm>
#include <cstdarg>
#include <stdexcept>

namespace fake_main {
UsbPort usb;
std::deque<uint8_t> uartRx;
std::vector<uint8_t> uartTx;
size_t uartWriteLimit = 7;
bool panelReady = true, stopClick = false;
babytech::display::DisplayIntent intent = babytech::display::DisplayIntent::None;
babytech::display::DisplaySnapshot shown;
bool shownConnected = false, shownPending = false;
int UsbPort::read() {
    if (input.empty()) return -1;
    const auto value = input.front(); input.pop_front(); return value;
}
size_t UsbPort::write(const uint8_t* data, size_t size) {
    const auto count = std::min(size, size_t(availableForWrite()));
    output.append(reinterpret_cast<const char*>(data), count);
    return count;
}
void UsbPort::printf(const char* format, ...) {
    char bytes[256];
    va_list args; va_start(args, format);
    const int size = std::vsnprintf(bytes, sizeof(bytes), format, args);
    va_end(args);
    fake::check(size >= 0 && size_t(size) < sizeof(bytes), "unbounded main log");
    fake::io.serial.emplace_back(bytes);
}
void input(const char* line) {
    for (const auto* byte = line; *byte; ++byte) usb.input.push_back(uint8_t(*byte));
    usb.input.push_back('\n');
}
}
void delay(uint32_t value) { fake::io.now += value; }
uint32_t fake_main::randomCounter = 100;
uint32_t esp_random() { return ++fake_main::randomCounter; }
HardwareSerial::HardwareSerial(uint8_t number) { fake::check(number == 1, "wrong UART"); }
size_t HardwareSerial::setRxBufferSize(size_t size) { return size; }
size_t HardwareSerial::setTxBufferSize(size_t size) { return size; }
void HardwareSerial::begin(unsigned long baud, uint32_t config, int8_t, int8_t) {
    fake::check(baud == 115200 && config == SERIAL_8N1, "wrong UART settings");
}
HardwareSerial::operator bool() const { return true; }
int HardwareSerial::available() { return int(fake_main::uartRx.size()); }
int HardwareSerial::read() {
    if (fake_main::uartRx.empty()) return -1;
    const auto value = fake_main::uartRx.front(); fake_main::uartRx.pop_front(); return value;
}
int HardwareSerial::availableForWrite() { return 23; }
size_t HardwareSerial::write(const uint8_t* data, size_t size) {
    fake::check(fake_main::uartWriteLimit > 0 && fake_main::uartWriteLimit <= 23, "invalid SDK UART write limit");
    const auto count = std::min(size, fake_main::uartWriteLimit);
    fake_main::uartTx.insert(fake_main::uartTx.end(), data, data + count);
    return count;
}
esp_err_t uart_wait_tx_done(uart_port_t port, TickType_t ticks) {
    fake::check(port == UART_NUM_1 && !ticks, "blocking UART TX check");
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t, const char*, const char*) {
    // This fixture deliberately does not emulate credential writes. A future
    // test must supply write/readback semantics before exercising NET WIFI.
    throw std::runtime_error("credential writes not supported by main fixture");
}
