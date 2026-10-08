#pragma once

#include "Arduino.h"
#include "display_model.h"
#include <deque>
#include <string>
#include <vector>

namespace fake_main {
struct UsbPort : FakeSerial {
    std::deque<uint8_t> input;
    std::string output;
    void begin(unsigned) {}
    void setTimeout(unsigned) {}
    void setTxTimeoutMs(unsigned value) { txTimeout = value; }
    unsigned txTimeout = 0;
    int available() const { return int(input.size()); }
    int availableForWrite() const { return 7; }
    int read();
    size_t write(const uint8_t*, size_t);
    void printf(const char*, ...);
};
extern UsbPort usb;
extern uint32_t randomCounter;
extern std::deque<uint8_t> uartRx;
extern std::vector<uint8_t> uartTx;
extern bool panelReady, stopClick;
extern babytech::display::DisplayIntent intent;
extern babytech::display::DisplaySnapshot shown;
extern bool shownConnected, shownPending;
void input(const char* line);
}
