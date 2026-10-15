#include "ProductRequest.h"
#include "RecordBytes.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
              "Request identity uses IEEE-754 binary32");

namespace babytech { namespace boardlink {
namespace {
template <size_t N> bool text(const char (&value)[N], bool nonempty) {
    size_t length = 0;
    while (length < N && value[length]) ++length;
    return length < N && (!nonempty || length) &&
        v4::validUtf8(reinterpret_cast<const uint8_t*>(value), length);
}
bool alphanumeric(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}
bool deviceId(const char (&value)[65]) {
    if (!text(value, true) || !alphanumeric(value[0])) return false;
    for (const char* at = value; *at; ++at)
        if (!alphanumeric(*at) && *at != '-' && *at != '_') return false;
    return true;
}
}

bool validProductIdentity(const char (&device)[65], const char (&command)[129]) {
    return deviceId(device) && text(command, true);
}

bool validProductRequest(const ProductRequest& request) {
    if ((request.source != v4::Source::CloudCommand && request.source != v4::Source::LocalTouch) ||
        request.command < ProductCommand::Initialize || request.command > ProductCommand::CheckFirmwareUpdate ||
        !request.sequence || request.sequence > v4::kMaxSequence ||
        !validProductIdentity(request.deviceId, request.commandId)) return false;
    if (request.command == ProductCommand::Initialize && request.source != v4::Source::LocalTouch)
        return false;
    if (request.command == ProductCommand::Prepare)
        return text(request.babyId, true) && request.profileVersion > 0 &&
            request.profileVersion <= INT32_MAX && request.waterMl >= 30 && request.waterMl <= 500 &&
            request.temperatureC >= 35 && request.temperatureC <= 60 &&
            std::isfinite(request.powderGPer100Ml) &&
            request.powderGPer100Ml >= 1 && request.powderGPer100Ml <= 50;
    if (request.babyId[0] || request.profileVersion || request.waterMl ||
        request.powderGPer100Ml != 0 || std::signbit(request.powderGPer100Ml)) return false;
    return request.command == ProductCommand::SetTargetTemp
        ? request.temperatureC >= 35 && request.temperatureC <= 60 : request.temperatureC == 0;
}

size_t encodeRequestIdentity(const ProductRequest& request, uint8_t* output, size_t capacity) {
    if (!output || !validProductRequest(request)) return 0;
    uint8_t bytes[kRequestIdentityMaxSize];
    detail::ByteWriter writer(bytes, sizeof(bytes));
    writer.integer(1, 1);
    writer.integer(uint8_t(request.source), 1);
    writer.integer(uint8_t(request.command), 1);
    writer.integer(request.sequence, 8);
    writer.text(request.deviceId);
    writer.text(request.commandId);
    writer.text(request.babyId);
    writer.integer(request.profileVersion, 4);
    writer.integer(request.waterMl, 2);
    writer.integer(request.temperatureC, 1);
    uint32_t bits;
    std::memcpy(&bits, &request.powderGPer100Ml, sizeof(bits));
    writer.integer(bits, 4);
    const size_t length = writer.size();
    if (!length || capacity < length) return 0;
    std::memcpy(output, bytes, length);
    return length;
}

bool decodeRequestIdentity(const uint8_t* bytes, size_t length, ProductRequest& output) {
    if (!bytes || !length || length > kRequestIdentityMaxSize) return false;
    detail::ByteReader reader(bytes, length);
    if (reader.integer(1) != 1) return false;
    ProductRequest request;
    request.source = v4::Source(reader.integer(1));
    request.command = ProductCommand(reader.integer(1));
    request.sequence = reader.integer(8);
    if (!reader.text(request.deviceId, sizeof(request.deviceId)) ||
        !reader.text(request.commandId, sizeof(request.commandId)) ||
        !reader.text(request.babyId, sizeof(request.babyId))) return false;
    request.profileVersion = uint32_t(reader.integer(4));
    request.waterMl = uint16_t(reader.integer(2));
    request.temperatureC = uint8_t(reader.integer(1));
    const uint32_t bits = uint32_t(reader.integer(4));
    std::memcpy(&request.powderGPer100Ml, &bits, sizeof(bits));
    if (!reader.done() || !validProductRequest(request)) return false;
    output = request;
    return true;
}

bool sameProductRequest(const ProductRequest& left, const ProductRequest& right) {
    return validProductRequest(left) && validProductRequest(right) &&
        left.source == right.source && left.command == right.command && left.sequence == right.sequence &&
        !std::strcmp(left.deviceId, right.deviceId) && !std::strcmp(left.commandId, right.commandId) &&
        !std::strcmp(left.babyId, right.babyId) && left.profileVersion == right.profileVersion &&
        left.waterMl == right.waterMl && left.temperatureC == right.temperatureC &&
        left.powderGPer100Ml == right.powderGPer100Ml;
}

bool makeLocalCommandId(const v4::Pairing& pairing, uint64_t sequence, char (&output)[129]) {
    if (!v4::validPairing(pairing) || pairing.role != v4::Role::Brain ||
        !sequence || sequence > v4::kMaxSequence) return false;
    char value[129]{};
    const int length = std::snprintf(value, sizeof(value), "local-%s-%llu", pairing.epoch,
                                     static_cast<unsigned long long>(sequence));
    if (length <= 0 || size_t(length) >= sizeof(value)) return false;
    std::memcpy(output, value, sizeof(value));
    return true;
}

} }
