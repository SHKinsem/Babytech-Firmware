#pragma once
#include <cstddef>
#include <string>

class Preferences {
public:
    ~Preferences();
    bool begin(const char* name, bool readOnly = false);
    size_t getBytesLength(const char* key);
    size_t getBytes(const char* key, void* output, size_t capacity);
    size_t putBytes(const char* key, const void* data, size_t size);
    void end();
private:
    std::string name_;
    bool opened_ = false;
    bool readOnly_ = true;
};
