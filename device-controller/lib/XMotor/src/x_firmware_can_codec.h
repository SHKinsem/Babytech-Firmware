#ifndef BABYTECH_X_FIRMWARE_CAN_CODEC_H
#define BABYTECH_X_FIRMWARE_CAN_CODEC_H

#include <stdint.h>

constexpr uint8_t kXProtocolFixedChecksum = 0x6B;

enum class XCommandResponseStatus : uint8_t {
  None,
  Received,
  Completed,
  ParameterError,
  FormatError,
};

inline bool decodeXCommandResponse(
    const uint8_t* data,
    uint8_t length,
    XCommandResponseStatus& status) {
  status = XCommandResponseStatus::None;
  if (data == nullptr || length != 3 ||
      data[2] != kXProtocolFixedChecksum) {
    return false;
  }
  switch (data[1]) {
    case 0x02:
      status = XCommandResponseStatus::Received;
      return true;
    case 0x9F:
      status = XCommandResponseStatus::Completed;
      return true;
    case 0xE2:
      status = XCommandResponseStatus::ParameterError;
      return true;
    case 0xEE:
      status = XCommandResponseStatus::FormatError;
      return true;
    default:
      return false;
  }
}

inline const char* xCommandResponseStatusToString(
    XCommandResponseStatus status) {
  switch (status) {
    case XCommandResponseStatus::None: return "none";
    case XCommandResponseStatus::Received: return "received";
    case XCommandResponseStatus::Completed: return "completed";
    case XCommandResponseStatus::ParameterError: return "parameter_error";
    case XCommandResponseStatus::FormatError: return "format_error";
  }
  return "unknown";
}

#endif
