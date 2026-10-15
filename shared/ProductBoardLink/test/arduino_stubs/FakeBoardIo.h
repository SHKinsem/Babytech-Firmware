#pragma once

#include "esp_err.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// Only SDK/serial I/O is faked. No pairing, frame, JSON or session decisions.
namespace fake {
struct State {
    esp_err_t openError = ESP_OK;
    esp_err_t queryError = ESP_OK;
    esp_err_t readError = ESP_OK;
    esp_err_t macError = ESP_OK;
    std::vector<uint8_t> blob;
    size_t queryLength = SIZE_MAX;
    size_t readLength = SIZE_MAX;
    std::array<uint8_t, 6> mac{{0x11, 0x22, 0x33, 0x44, 0x55, 0x66}};
    bool handleOpen = false;
    unsigned opens = 0, queries = 0, reads = 0, closes = 0, macReads = 0;
    unsigned mutations = 0;
    std::vector<std::string> calls;
    std::deque<uint32_t> randomWords{0x10203040, 0x50607080};
    unsigned randomReads = 0;

    size_t rxResult = 256;
    size_t txRingSize = 0;
    bool serialReady = true;
    unsigned constructors = 0, begins = 0, boolChecks = 0;
    unsigned rxConfigs = 0, txConfigs = 0;
    size_t requestedRx = SIZE_MAX, requestedTx = SIZE_MAX;
    unsigned long baud = 0;
    uint32_t config = 0;
    int rxPin = -1, txPin = -1;
    std::deque<uint8_t> rx;
    bool negativeRead = false;
    unsigned availableCalls = 0, byteReads = 0;
    std::vector<uint8_t> tx;
    int writeRoom = 512;
    std::deque<int> roomScript;
    size_t maxWrite = SIZE_MAX;
    bool overreportWrite = false;
    esp_err_t idleResult = ESP_OK;
    unsigned idleChecks = 0, roomChecks = 0, writeCalls = 0;
    int lastRoom = 0;
    std::vector<size_t> writeRequests;
};

extern State io;
void reset();
void assertReadOnly();
}  // namespace fake
