#ifndef BABYTECH_X42S_CAN_ID_H
#define BABYTECH_X42S_CAN_ID_H

#include <stdint.h>

// The X firmware used by X28S and X42S stores the motor address in bits
// 8..15 and the packet index in bits 0..7 of an extended CAN identifier.
constexpr uint32_t x42sCanFrameId(uint8_t address, uint8_t packetIndex) {
  return (static_cast<uint32_t>(address) << 8) | packetIndex;
}

constexpr uint8_t x42sCanAddress(uint32_t frameId) {
  return static_cast<uint8_t>((frameId >> 8) & 0xFFU);
}

constexpr uint8_t x42sCanPacketIndex(uint32_t frameId) {
  return static_cast<uint8_t>(frameId & 0xFFU);
}

constexpr bool x42sCanIsSinglePacketDataFrame(
    uint32_t frameId, bool extended, bool remote) {
  return extended && !remote && (frameId & ~0xFFFFU) == 0 &&
      x42sCanPacketIndex(frameId) == 0;
}

#endif
