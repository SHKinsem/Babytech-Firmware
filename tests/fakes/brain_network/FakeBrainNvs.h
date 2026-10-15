#pragma once
#include "nvs.h"
#include <map>
#include <string>
#include <vector>

namespace fake {
struct NvsCall { std::string operation; std::string key; bool worker; };
struct BrainNvs {
    bool exists = true;
    bool opened = false;
    bool allowWrites = false;
    bool writable = false;
    esp_err_t openError = ESP_OK;
    unsigned writes = 0;
    std::map<std::string, std::vector<char>> values;
    std::map<std::string, esp_err_t> readErrors;
    // Deliberately inconsistent SDK metadata for corruption/fault injection.
    std::map<std::string, size_t> reportedLengths;
    std::vector<NvsCall> calls;
    void seed(const std::string& key, const std::string& value);
};
extern BrainNvs nvs;
}
