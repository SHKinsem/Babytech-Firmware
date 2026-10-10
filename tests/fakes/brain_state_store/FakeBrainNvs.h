#pragma once
#include "nvs.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fake_brain {
using Bytes = std::vector<uint8_t>;
enum class Type { Blob, String, U32 };
struct Value {
    Bytes bytes;
    Type type = Type::Blob;
    bool operator==(const Value& other) const { return bytes == other.bytes && type == other.type; }
};
using Namespace = std::map<std::string, Value>;
using Database = std::map<std::string, Namespace>;
enum class Op { OpenRO, OpenRW, Query, Read, Set, Commit, Close, Erase, Init };
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
    bool apply;
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
    std::vector<Call> calls;
    std::vector<Fault> faults;
    std::function<void(const Call&)> before;
    bool durableOnSet = false;
};
extern State io;
void check(bool condition, const char* message);
void reset();
// Drops handles/staged writes/faults while retaining only durable bytes.
void reboot();
unsigned count(Op op);
void fail(Op op, unsigned occurrence, esp_err_t error = ESP_FAIL, bool apply = false,
          std::optional<size_t> reportedLength = std::nullopt);
void verifyFaults();
esp_err_t setString(nvs_handle_t handle, const char* key, const char* value);
}  // namespace fake_brain
