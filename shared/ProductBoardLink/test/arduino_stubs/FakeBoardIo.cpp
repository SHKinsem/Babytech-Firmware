#include "FakeBoardIo.h"
#include "Arduino.h"
#include "driver/uart.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>

namespace fake {
State io;
void assertReadOnly() {
    assert(!io.handleOpen && io.mutations == 0);
}
void reset() {
    assertReadOnly();
    io = State{};
}
}  // namespace fake

using fake::io;
namespace {
constexpr nvs_handle_t kHandle = 42;
[[noreturn]] esp_err_t forbiddenMutation() {
    ++io.mutations;
    std::abort();
}
}

esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* handle) {
    ++io.opens;
    io.calls.push_back("open");
    assert(std::strcmp(name, "productpair") == 0);
    if (mode != NVS_READONLY) return forbiddenMutation();
    assert(handle && !io.handleOpen);
    if (io.openError != ESP_OK) return io.openError;
    io.handleOpen = true;
    *handle = kHandle;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* output, size_t* length) {
    assert(handle == kHandle && io.handleOpen && length);
    assert(std::strcmp(key, "record") == 0);
    if (!output) {
        ++io.queries;
        io.calls.push_back("query");
        assert(*length == 0);
        if (io.queryError != ESP_OK) return io.queryError;
        *length = io.queryLength == SIZE_MAX ? io.blob.size() : io.queryLength;
        return ESP_OK;
    }
    ++io.reads;
    io.calls.push_back("read");
    const size_t capacity = *length;
    assert(capacity <= 256);
    if (io.readError != ESP_OK) {
        // An SDK error may still have touched its own destination buffer.
        std::memset(output, 0xe5, capacity);
        return io.readError;
    }
    const size_t reported = io.readLength == SIZE_MAX ? io.blob.size() : io.readLength;
    std::memcpy(output, io.blob.data(), std::min({capacity, reported, io.blob.size()}));
    *length = reported;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) {
    assert(handle == kHandle && io.handleOpen);
    io.handleOpen = false;
    ++io.closes;
    io.calls.push_back("close");
}
esp_err_t nvs_set_blob(nvs_handle_t, const char*, const void*, size_t) { return forbiddenMutation(); }
esp_err_t nvs_erase_key(nvs_handle_t, const char*) { return forbiddenMutation(); }
esp_err_t nvs_erase_all(nvs_handle_t) { return forbiddenMutation(); }
esp_err_t nvs_commit(nvs_handle_t) { return forbiddenMutation(); }
esp_err_t nvs_flash_erase() { return forbiddenMutation(); }
esp_err_t nvs_flash_erase_partition(const char*) { return forbiddenMutation(); }

esp_err_t esp_read_mac(uint8_t* mac, esp_mac_type_t type) {
    assert(type == ESP_MAC_WIFI_STA && mac && !io.handleOpen);
    ++io.macReads;
    io.calls.push_back("mac");
    if (io.macError != ESP_OK) return io.macError;
    std::memcpy(mac, io.mac.data(), io.mac.size());
    return ESP_OK;
}

uint32_t esp_random() {
    ++io.randomReads;
    assert(!io.randomWords.empty());
    const uint32_t word = io.randomWords.front();
    io.randomWords.pop_front();
    return word;
}

HardwareSerial::HardwareSerial(uint8_t number) {
    assert(number == 1);
    ++io.constructors;
}
size_t HardwareSerial::setRxBufferSize(size_t size) {
    ++io.rxConfigs;
    io.requestedRx = size;
    assert(io.begins == 0);
    return io.rxResult;
}
size_t HardwareSerial::setTxBufferSize(size_t size) {
    ++io.txConfigs;
    io.requestedTx = size;
    io.txRingSize = size;
    assert(false && "Adapter must preserve HardwareSerial's default zero TX ring");
    return size;
}
void HardwareSerial::begin(unsigned long baud, uint32_t config, int8_t rxPin, int8_t txPin) {
    ++io.begins;
    io.baud = baud;
    io.config = config;
    io.rxPin = rxPin;
    io.txPin = txPin;
    assert(io.rxConfigs == 1 && io.txConfigs == 0 && io.txRingSize == 0);
}
HardwareSerial::operator bool() const {
    ++io.boolChecks;
    return io.serialReady;
}
int HardwareSerial::available() {
    ++io.availableCalls;
    return int(io.rx.size());
}
int HardwareSerial::read() {
    ++io.byteReads;
    if (io.negativeRead) { io.negativeRead = false; return -1; }
    if (io.rx.empty()) return -1;
    const uint8_t value = io.rx.front();
    io.rx.pop_front();
    return value;
}
int HardwareSerial::availableForWrite() {
    ++io.roomChecks;
    int room = io.writeRoom;
    if (!io.roomScript.empty()) {
        room = io.roomScript.front();
        io.roomScript.pop_front();
    }
    io.lastRoom = room;
    return room;
}
size_t HardwareSerial::write(const uint8_t* bytes, size_t size) {
    assert(io.begins == 1 && io.serialReady);
    assert(size <= size_t(std::max(0, io.lastRoom)));
    ++io.writeCalls;
    io.writeRequests.push_back(size);
    const size_t written = std::min(size, io.maxWrite);
    io.tx.insert(io.tx.end(), bytes, bytes + written);
    return io.overreportWrite ? size + 1 : written;
}
esp_err_t uart_wait_tx_done(uart_port_t port, TickType_t ticks) {
    assert(port == UART_NUM_1 && ticks == 0);
    ++io.idleChecks;
    return io.idleResult;
}
