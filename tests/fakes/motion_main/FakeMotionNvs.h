#pragma once

#include "nvs.h"
#include "../brain_state_store/FakeBrainNvs.h"

namespace fake_motion_nvs {
// The runner links FakeBrainNvs.cpp exactly once. No second disk or blob API.
using fake_brain::io;
using fake_brain::Op;
using fake_brain::count;
using fake_brain::fail;
using fake_brain::verifyFaults;

enum class DeletionKind { Remove, Clear };
struct Deletion {
    DeletionKind kind;
    std::string name;
    std::string key;
    bool allowed;
};
extern std::vector<Deletion> deletions;
extern bool preferencesDeletionAllowed;
void reset();
void reboot();
void verifyNoEraseOrInit();

class AllowPreferencesDeletion {
public:
    AllowPreferencesDeletion();
    ~AllowPreferencesDeletion();
    AllowPreferencesDeletion(const AllowPreferencesDeletion&) = delete;
    AllowPreferencesDeletion& operator=(const AllowPreferencesDeletion&) = delete;
private:
    bool previous_;
};

// Only Preferences uses this path; the base raw/global erase APIs remain fatal.
bool erasePreferences(nvs_handle_t, const char* key, bool clear);
bool contains(nvs_handle_t, const char* key);
}  // namespace fake_motion_nvs
