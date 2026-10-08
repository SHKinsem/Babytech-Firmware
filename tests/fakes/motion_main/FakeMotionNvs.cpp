#include "FakeMotionNvs.h"
#include "Preferences.h"

#include <cstring>

namespace {
using namespace fake_brain;
constexpr size_t kMaxValueSize = 4096;
// Extra tags live in the same persisted Value, not a volatile side type map.
// They model type checks, not ESP-IDF's on-flash representation.
enum class ExtraType { I8 = 3, U8, I16, U16, I32, I64, U64, Deleted };
Type tag(ExtraType type) { return static_cast<Type>(type); }

std::optional<Fault> enter(Op op, const Handle& opened, const char* key,
                           nvs_handle_t handle) {
    const Call call{op, count(op) + 1, opened.name, key ? key : "", handle};
    io.calls.push_back(call);
    if (io.before) io.before(call);
    for (auto& fault : io.faults) {
        if (fault.op == op && fault.occurrence == call.occurrence) {
            check(!fault.hit, "fault applied twice");
            fault.hit = true;
            return fault;
        }
    }
    return std::nullopt;
}
Handle& getHandle(nvs_handle_t handle, bool write = false) {
    auto found = io.handles.find(handle);
    check(found != io.handles.end(), "invalid/closed Motion handle");
    check(!write || found->second.mode == NVS_READWRITE, "Motion write on read-only handle");
    return found->second;
}
const Value* findValue(const Handle& opened, const char* key) {
    auto pending = opened.pending.find(key);
    if (pending != opened.pending.end()) {
        return pending->second.type == tag(ExtraType::Deleted) ? nullptr : &pending->second;
    }
    auto space = io.disk.find(opened.name);
    if (space == io.disk.end()) return nullptr;
    auto found = space->second.find(key);
    return found == space->second.end() || found->second.type == tag(ExtraType::Deleted)
        ? nullptr : &found->second;
}
esp_err_t setValue(nvs_handle_t handle, const char* key, const void* data,
                   size_t size, Type type) {
    if (!key || !data || !size || size > kMaxValueSize) return ESP_FAIL;
    auto& opened = getHandle(handle, true);
    auto fault = enter(Op::Set, opened, key, handle);
    if (!fault || fault->error == ESP_OK || fault->apply) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        Value value{Bytes(bytes, bytes + size), type};
        opened.pending[key] = value;
        if (io.durableOnSet || (fault && fault->apply)) io.disk[opened.name][key] = value;
    }
    return fault ? fault->error : ESP_OK;
}
template <class T>
esp_err_t getScalar(nvs_handle_t handle, const char* key, T* output, Type type) {
    if (!key || !output) return ESP_FAIL;
    auto& opened = getHandle(handle);
    auto fault = enter(Op::Read, opened, key, handle);
    if (fault && fault->error != ESP_OK) return fault->error;
    const auto* value = findValue(opened, key);
    if (!value) return ESP_ERR_NVS_NOT_FOUND;
    if (value->type != type) return ESP_ERR_NVS_TYPE_MISMATCH;
    if (value->bytes.size() != sizeof(T)) return ESP_ERR_NVS_INVALID_LENGTH;
    std::memcpy(output, value->bytes.data(), sizeof(T));
    return ESP_OK;
}
void purgeDeleted() {
    for (auto& space : io.disk) {
        for (auto it = space.second.begin(); it != space.second.end();) {
            if (it->second.type == tag(ExtraType::Deleted)) it = space.second.erase(it);
            else ++it;
        }
    }
}
}  // namespace

namespace fake_motion_nvs {
std::vector<Deletion> deletions;
bool preferencesDeletionAllowed = false;
void reset() {
    fake_brain::reset();
    deletions.clear();
    preferencesDeletionAllowed = false;
}
void reboot() {
    fake_brain::reboot();
    deletions.clear();
    preferencesDeletionAllowed = false;
}
void verifyNoEraseOrInit() {
    check(count(Op::Erase) == 0 && count(Op::Init) == 0 && deletions.empty(),
          "ordinary setup erased/initialized NVS or attempted Preferences deletion");
}
AllowPreferencesDeletion::AllowPreferencesDeletion()
    : previous_(preferencesDeletionAllowed) { preferencesDeletionAllowed = true; }
AllowPreferencesDeletion::~AllowPreferencesDeletion() { preferencesDeletionAllowed = previous_; }
bool contains(nvs_handle_t handle, const char* key) {
    if (!key) return false;
    auto& opened = getHandle(handle);
    auto fault = enter(Op::Query, opened, key, handle);
    return (!fault || fault->error == ESP_OK) && findValue(opened, key);
}
bool erasePreferences(nvs_handle_t handle, const char* key, bool clear) {
    auto& opened = getHandle(handle, true);
    deletions.push_back({clear ? DeletionKind::Clear : DeletionKind::Remove,
                         opened.name, key ? key : "", preferencesDeletionAllowed});
    check(preferencesDeletionAllowed, "Preferences deletion requires an explicit test scope");
    auto fault = enter(Op::Erase, opened, key, handle);
    if (!clear && (!key || !findValue(opened, key))) return false;
    if (!fault || fault->error == ESP_OK || fault->apply) {
        const Value deleted{{}, tag(ExtraType::Deleted)};
        if (clear) {
            for (const auto& entry : io.disk[opened.name]) opened.pending[entry.first] = deleted;
            for (auto& entry : opened.pending) entry.second = deleted;
        } else opened.pending[key] = deleted;
        if (io.durableOnSet || (fault && fault->apply)) {
            for (const auto& entry : opened.pending) {
                if (entry.second.type == tag(ExtraType::Deleted)) io.disk[opened.name].erase(entry.first);
            }
        }
    }
    if (fault && fault->error != ESP_OK) return false;
    const auto result = nvs_commit(handle);
    // The base commit also models errors after persistence. Remove only durable tombstones.
    purgeDeleted();
    return result == ESP_OK;
}
}  // namespace fake_motion_nvs

esp_err_t nvs_get_str(nvs_handle_t handle, const char* key, char* output, size_t* length) {
    if (!key || !length) return ESP_FAIL;
    auto& opened = getHandle(handle);
    const size_t capacity = *length;
    check(!output || capacity <= kMaxValueSize, "unbounded Motion string read");
    auto fault = enter(output ? Op::Read : Op::Query, opened, key, handle);
    if (fault && fault->error != ESP_OK) {
        if (output) std::memset(output, 0xe5, capacity);
        return fault->error;
    }
    const auto* value = findValue(opened, key);
    if (!value) return ESP_ERR_NVS_NOT_FOUND;
    if (value->type != Type::String) return ESP_ERR_NVS_TYPE_MISMATCH;
    if (value->bytes.empty() || value->bytes.back() != 0 || value->bytes.size() > kMaxValueSize)
        return ESP_ERR_NVS_INVALID_LENGTH;
    *length = fault && fault->reportedLength ? *fault->reportedLength : value->bytes.size();
    if (!output) return ESP_OK;
    if (capacity < value->bytes.size()) return ESP_ERR_NVS_INVALID_LENGTH;
    std::memcpy(output, value->bytes.data(), value->bytes.size());
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t handle, const char* key, const char* value) {
    if (!value) return ESP_FAIL;
    return setValue(handle, key, value, std::strlen(value) + 1, Type::String);
}
#define MOTION_INTEGER_API(suffix, type, typeTag) \
    esp_err_t nvs_get_##suffix(nvs_handle_t h, const char* k, type* v) { \
        return getScalar(h, k, v, typeTag); \
    } \
    esp_err_t nvs_set_##suffix(nvs_handle_t h, const char* k, type v) { \
        return setValue(h, k, &v, sizeof(v), typeTag); \
    }
MOTION_INTEGER_API(i8, int8_t, tag(ExtraType::I8))
MOTION_INTEGER_API(u8, uint8_t, tag(ExtraType::U8))
MOTION_INTEGER_API(i16, int16_t, tag(ExtraType::I16))
MOTION_INTEGER_API(u16, uint16_t, tag(ExtraType::U16))
MOTION_INTEGER_API(i32, int32_t, tag(ExtraType::I32))
MOTION_INTEGER_API(u32, uint32_t, Type::U32)
MOTION_INTEGER_API(i64, int64_t, tag(ExtraType::I64))
MOTION_INTEGER_API(u64, uint64_t, tag(ExtraType::U64))
#undef MOTION_INTEGER_API

Preferences::~Preferences() { end(); }
bool Preferences::begin(const char* name, bool readOnly, const char* partitionLabel) {
    if (opened_ || !name || (partitionLabel && std::strcmp(partitionLabel, "nvs") != 0)) return false;
    if (nvs_open(name, readOnly ? NVS_READONLY : NVS_READWRITE, &handle_) != ESP_OK) return false;
    opened_ = true;
    readOnly_ = readOnly;
    return true;
}
void Preferences::end() {
    if (opened_) nvs_close(handle_);
    opened_ = false;
    handle_ = 0;
    readOnly_ = true;
}
bool Preferences::isKey(const char* key) {
    return opened_ && fake_motion_nvs::contains(handle_, key);
}
bool Preferences::remove(const char* key) {
    return writable() && fake_motion_nvs::erasePreferences(handle_, key, false);
}
bool Preferences::clear() {
    return writable() && fake_motion_nvs::erasePreferences(handle_, nullptr, true);
}
size_t Preferences::finishWrite(esp_err_t result, size_t size) {
    if (result != ESP_OK) return 0;
    const auto committed = nvs_commit(handle_);
    purgeDeleted();
    return committed == ESP_OK ? size : 0;
}
size_t Preferences::getBytesLength(const char* key) {
    if (!opened_ || !key) return 0;
    size_t size = 0;
    return nvs_get_blob(handle_, key, nullptr, &size) == ESP_OK ? size : 0;
}
size_t Preferences::getBytes(const char* key, void* output, size_t capacity) {
    if (!opened_ || !key) return 0;
    const auto size = getBytesLength(key);
    if (!size || !output || !capacity) return size;
    if (size > capacity || size > kMaxValueSize) return 0;
    size_t actual = size;
    return nvs_get_blob(handle_, key, output, &actual) == ESP_OK ? actual : 0;
}
size_t Preferences::putBytes(const char* key, const void* data, size_t size) {
    if (!writable() || !key || !data || !size || size > kMaxValueSize) return 0;
    return finishWrite(nvs_set_blob(handle_, key, data, size), size);
}
String Preferences::getString(const char* key, String defaultValue) {
    if (!opened_ || !key) return defaultValue;
    size_t size = 0;
    if (nvs_get_str(handle_, key, nullptr, &size) != ESP_OK || !size || size > kMaxValueSize)
        return defaultValue;
    std::vector<char> output(size);
    if (nvs_get_str(handle_, key, output.data(), &size) != ESP_OK) return defaultValue;
    return String(output.data());
}
size_t Preferences::getString(const char* key, char* output, size_t capacity) {
    if (!opened_ || !key || !output || !capacity) return 0;
    size_t size = 0;
    if (nvs_get_str(handle_, key, nullptr, &size) != ESP_OK || !size ||
        size > capacity || size > kMaxValueSize) return 0;
    return nvs_get_str(handle_, key, output, &size) == ESP_OK ? size : 0;
}
size_t Preferences::putString(const char* key, const char* value) {
    if (!writable() || !key || !value) return 0;
    return finishWrite(nvs_set_str(handle_, key, value), std::strlen(value));
}
size_t Preferences::putString(const char* key, const String& value) {
    return putString(key, value.c_str());
}
bool Preferences::getBool(const char* key, bool defaultValue) {
    return getUChar(key, defaultValue ? 1 : 0) == 1;
}
size_t Preferences::putBool(const char* key, bool value) { return putUChar(key, value ? 1 : 0); }
#define MOTION_PREFERENCES_API(name, type, getFunction, setFunction) \
    type Preferences::get##name(const char* key, type defaultValue) { \
        type value{}; \
        return opened_ && key && getFunction(handle_, key, &value) == ESP_OK ? value : defaultValue; \
    } \
    size_t Preferences::put##name(const char* key, type value) { \
        if (!writable() || !key) return 0; \
        return finishWrite(setFunction(handle_, key, value), sizeof(value)); \
    }
MOTION_PREFERENCES_API(Char, int8_t, nvs_get_i8, nvs_set_i8)
MOTION_PREFERENCES_API(UChar, uint8_t, nvs_get_u8, nvs_set_u8)
MOTION_PREFERENCES_API(Short, int16_t, nvs_get_i16, nvs_set_i16)
MOTION_PREFERENCES_API(UShort, uint16_t, nvs_get_u16, nvs_set_u16)
MOTION_PREFERENCES_API(Int, int32_t, nvs_get_i32, nvs_set_i32)
MOTION_PREFERENCES_API(UInt, uint32_t, nvs_get_u32, nvs_set_u32)
MOTION_PREFERENCES_API(Long, int32_t, nvs_get_i32, nvs_set_i32)
MOTION_PREFERENCES_API(ULong, uint32_t, nvs_get_u32, nvs_set_u32)
MOTION_PREFERENCES_API(Long64, int64_t, nvs_get_i64, nvs_set_i64)
MOTION_PREFERENCES_API(ULong64, uint64_t, nvs_get_u64, nvs_set_u64)
#undef MOTION_PREFERENCES_API

float Preferences::getFloat(const char* key, float defaultValue) {
    getBytes(key, &defaultValue, sizeof(defaultValue));
    return defaultValue;
}
double Preferences::getDouble(const char* key, double defaultValue) {
    getBytes(key, &defaultValue, sizeof(defaultValue));
    return defaultValue;
}
size_t Preferences::putFloat(const char* key, float value) {
    return putBytes(key, &value, sizeof(value));
}
size_t Preferences::putDouble(const char* key, double value) {
    return putBytes(key, &value, sizeof(value));
}
