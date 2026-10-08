#pragma once
#include <Arduino.h>
#include <driver/twai.h>
#include <array>
namespace motion_io {
inline std::array<uint8_t, 6> mac{{0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff}};
inline std::deque<twai_message_t> canRx;
inline std::vector<twai_message_t> canTx;
inline int rejectedOpcode = -1, rejectedPacket = -1;
inline bool automaticFeedback = false;
inline unsigned missingId = 0, movingId = 0;
inline unsigned gpioWrites = 0, canStarts = 0;
void reply(uint8_t id, std::initializer_list<uint8_t> data);
}
