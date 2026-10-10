#pragma once

#include <Arduino.h>
#include "nvs.h"

class Preferences {
public:
    ~Preferences();
    Preferences() = default;
    Preferences(const Preferences&) = delete;
    Preferences& operator=(const Preferences&) = delete;
    bool begin(const char* name, bool readOnly = false, const char* partitionLabel = nullptr);
    void end();
    bool isKey(const char* key);
    bool remove(const char* key);
    bool clear();
    size_t getBytesLength(const char* key);
    size_t getBytes(const char* key, void* output, size_t capacity);
    size_t putBytes(const char* key, const void* data, size_t size);
    String getString(const char* key, String defaultValue = String());
    size_t getString(const char* key, char* output, size_t capacity);
    size_t putString(const char* key, const char* value);
    size_t putString(const char* key, const String& value);
    bool getBool(const char* key, bool defaultValue = false);
    size_t putBool(const char* key, bool value);
#define MOTION_PREFERENCES_DECLARATIONS(name, type) \
    type get##name(const char* key, type defaultValue = 0); \
    size_t put##name(const char* key, type value);
    MOTION_PREFERENCES_DECLARATIONS(Char, int8_t)
    MOTION_PREFERENCES_DECLARATIONS(UChar, uint8_t)
    MOTION_PREFERENCES_DECLARATIONS(Short, int16_t)
    MOTION_PREFERENCES_DECLARATIONS(UShort, uint16_t)
    MOTION_PREFERENCES_DECLARATIONS(Int, int32_t)
    MOTION_PREFERENCES_DECLARATIONS(UInt, uint32_t)
    MOTION_PREFERENCES_DECLARATIONS(Long, int32_t)
    MOTION_PREFERENCES_DECLARATIONS(ULong, uint32_t)
    MOTION_PREFERENCES_DECLARATIONS(Long64, int64_t)
    MOTION_PREFERENCES_DECLARATIONS(ULong64, uint64_t)
    MOTION_PREFERENCES_DECLARATIONS(Float, float)
    MOTION_PREFERENCES_DECLARATIONS(Double, double)
#undef MOTION_PREFERENCES_DECLARATIONS
private:
    bool writable() const { return opened_ && !readOnly_; }
    size_t finishWrite(esp_err_t result, size_t size);
    nvs_handle_t handle_ = 0;
    bool opened_ = false;
    bool readOnly_ = true;
};
