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
inline int lowWaterLevel = HIGH;
inline bool hxReady = false, flowFeedback = false;
inline bool homeCompletionReply = true, markerReply = true;
inline int32_t hxRaw = 0;
inline uint32_t hxLatched = 0;
inline uint32_t hxNextConversionAt = 0;
inline int hxBit = -1;
inline unsigned hxSamples = 0, hxPulses = 0;
inline std::array<uint8_t, 256> driverFlags{};
void reply(uint8_t id, std::initializer_list<uint8_t> data);
}
