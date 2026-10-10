#pragma once
#include "nvs.h"

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fake {
using Bytes = std::vector<uint8_t>;
enum class Type { Blob, String, U32 };
struct Value {
    Bytes bytes;
    Type type = Type::Blob;
    bool operator==(const Value& other) const { return bytes == other.bytes && type == other.type; }
};
using Namespace = std::map<std::string, Value>;
using Database = std::map<std::string, Namespace>;
enum class Op { OpenRO, OpenRW, Query, Read, Set, Commit, Close, Mac, Erase };
struct Call {
    Op op;
    unsigned occurrence;
    std::string name;
    std::string key;
    nvs_handle_t handle;
};
struct Fault {
    Op op;
    unsigned occurrence;
    esp_err_t error;
    bool apply = false;
    std::optional<size_t> reportedLength;
    bool hit = false;
};
struct Handle {
    std::string name;
    nvs_open_mode_t mode;
    Namespace pending;
};
struct State {
    Database disk;
    std::map<nvs_handle_t, Handle> handles;
    nvs_handle_t nextHandle = 1;
    std::array<uint8_t, 6> mac{{0x01, 0x23, 0x45, 0xab, 0xcd, 0xef}};
    std::vector<Call> calls;
    std::vector<Fault> faults;
    std::function<void(const Call&)> before;
    // Exercise both early durability and deferred durability; neither claims
    // to emulate the real ESP-IDF power-loss implementation.
    bool durableOnSet = false;
};
extern State io;
void check(bool condition, const char* message);
void reset();
void reboot();
unsigned count(Op op);
void fail(Op op, unsigned occurrence, esp_err_t error = ESP_FAIL, bool apply = false,
          std::optional<size_t> reportedLength = std::nullopt);
void verifyFaults();
}  // namespace fake
