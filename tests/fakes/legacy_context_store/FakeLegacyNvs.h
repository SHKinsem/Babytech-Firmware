#pragma once
#include "nvs.h"
#include <map>
#include <string>
#include <vector>

namespace fake {
using Bytes = std::vector<uint8_t>;
enum class Type { String, Blob, U32 };
struct Value {
    Bytes bytes;
    Type type = Type::String;
    bool operator==(const Value& other) const {
        return bytes == other.bytes && type == other.type;
    }
};
using Database = std::map<std::string, std::map<std::string, Value>>;
enum class Op { Open, Query, Read, Close };
struct Fault {
    Op op;
    esp_err_t error;
    bool overrideLength = false;
    size_t length = 0;
    bool used = false;
    size_t copyLimit = SIZE_MAX;
};
struct State {
    Database disk;
    std::vector<Op> calls;
    std::vector<Fault> faults;
    bool handleOpen = false;
};
extern State io;
void reset();
void verify();
size_t count(Op op);
}  // namespace fake
