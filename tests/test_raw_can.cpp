// Compile the REAL production X42sProtocol.cpp against only a fake TWAI HAL.
#include "X42sProtocol.h"
#include <cassert>
#include <cstdio>
#include <vector>
FakeSerial Serial;
static std::vector<twai_message_t> attempts;
static std::vector<CanRawFrame> traces;
static int failAt=-1;
void delay(unsigned long) {}
esp_err_t twai_transmit(const twai_message_t* frame,uint32_t timeout) {
    assert(timeout<=50); attempts.push_back(*frame);
    return int(attempts.size())==failAt ? -1 : ESP_OK;
}
static void capture(void*,const CanRawFrame& frame,bool tx) {assert(tx);traces.push_back(frame);}
static void reset() {attempts.clear();traces.clear();failAt=-1;}
int main() {
    X42sProtocol driver(4,5,500000);driver.setTraceSink(capture,nullptr);
    const uint8_t velocity[]={3,0xC6,1,0,0x3C,1,0x2C,0,3,0x20,0x6B};
    assert(!driver.sendRawLogical(velocity,sizeof(velocity)));
    assert(attempts.empty());assert(driver.begin());reset();
    assert(driver.sendRawLogical(velocity,sizeof(velocity)));
    assert(attempts.size()==2 && traces.size()==2);
    const uint8_t first[]={0xC6,1,0,0x3C,1,0x2C,0,3};
    const uint8_t second[]={0xC6,0x20,0x6B};
    assert(attempts[0].identifier==0x300 && attempts[0].data_length_code==8);
    assert(attempts[1].identifier==0x301 && attempts[1].data_length_code==3);
    assert(!memcmp(attempts[0].data,first,8) && !memcmp(attempts[1].data,second,3));
    for(const auto& f:attempts) assert(f.extd && f.ss && !f.rtr);
    reset();failAt=1;assert(!driver.sendRawLogical(velocity,sizeof(velocity)));
    assert(attempts.size()==1 && traces.empty()); // don't send fragment2 after failure
    reset();const uint8_t data[]={0xDE,0xAD};
    assert(driver.sendRawFrame(0x123,false,data,2));
    assert(attempts.size()==1 && !attempts[0].extd && attempts[0].identifier==0x123);
    assert(attempts[0].data_length_code==2 && !memcmp(attempts[0].data,data,2));
    assert(traces.size()==1 && !traces[0].extended && traces[0].identifier==0x123);
    reset();assert(!driver.sendRawFrame(0x800,false,data,2));
    assert(!driver.sendRawFrame(0x20000000,true,data,2));
    assert(!driver.sendRawFrame(0,true,data,9));assert(attempts.empty());
    assert(driver.sendRawFrame(0x1FFFFFFF,true,nullptr,0));
    assert(attempts.back().data_length_code==0);
    reset();const uint8_t unknown[]={0,0xFF,0x66,0xAB};
    assert(driver.sendRawLogical(unknown,4)); // no checksum rewriting
    assert(attempts[0].identifier==0 && attempts[0].data[2]==0xAB);
    // Independent manual p54-55 examples: preserve FB/CB rather than translating
    // direct position into a trapezoidal CD command. Each packet repeats opcode.
    reset();const uint8_t direct[]={2,0xFB,0,1,0x2C,0,0,3,0x84,2,0,0x6B};
    assert(driver.sendValidatedCommand(direct,sizeof(direct)));
    const uint8_t fb0[]={0xFB,0,1,0x2C,0,0,3,0x84};
    const uint8_t fb1[]={0xFB,2,0,0x6B};
    assert(attempts.size()==2 && attempts[0].identifier==0x200 && attempts[1].identifier==0x201);
    assert(attempts[0].data_length_code==8 && attempts[1].data_length_code==4);
    assert(!memcmp(attempts[0].data,fb0,8) && !memcmp(attempts[1].data,fb1,4));
    reset();const uint8_t limited[]={2,0xCB,0,1,0x2C,0,0,3,0x84,2,0,3,0x20,0x6B};
    assert(driver.sendValidatedCommand(limited,sizeof(limited)));
    const uint8_t cb0[]={0xCB,0,1,0x2C,0,0,3,0x84};
    const uint8_t cb1[]={0xCB,2,0,3,0x20,0x6B};
    assert(attempts.size()==2 && attempts[0].identifier==0x200 && attempts[1].identifier==0x201);
    assert(attempts[0].data_length_code==8 && attempts[1].data_length_code==6);
    assert(!memcmp(attempts[0].data,cb0,8) && !memcmp(attempts[1].data,cb1,6));
    reset();driver.passthroughPositionControl(2,0,300,900,2,false);
    assert(attempts.size()==2 && !memcmp(attempts[0].data,fb0,8) && !memcmp(attempts[1].data,fb1,4));
    reset();driver.passthroughPositionControlWithCurrentLimit(2,0,300,900,2,false,800);
    assert(attempts.size()==2 && !memcmp(attempts[0].data,cb0,8) && !memcmp(attempts[1].data,cb1,6));
    reset();const uint8_t currentLimit[]={1,0x45,0x66,0,0,0x78,0x6B};
    const uint8_t currentPayload[]={0x45,0x66,0,0,0x78,0x6B};
    assert(driver.sendValidatedCommand(currentLimit,sizeof(currentLimit)));
    assert(attempts.size()==1 && attempts[0].identifier==0x100 && attempts[0].extd);
    assert(attempts[0].data_length_code==6 && !memcmp(attempts[0].data,currentPayload,6));
    puts("PASS actual CAN driver: fragmentation, exact FB/CB/45 bytes/IDs, bounds, trace, partial TX failure");
}
