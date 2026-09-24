#include "DemoFlowConfig.h"
#include <cJSON.h>
#include <cmath>
#include <cstring>
#include <memory>
#include <sstream>

namespace motion {
const char* const kDemoStageIds[5] = {"open_cap", "water", "powder", "close_cap", "mix"};
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
const cJSON* field(const cJSON* o, const char* k) { return cJSON_GetObjectItemCaseSensitive(o, k); }
bool number(const cJSON* o, const char* key, double low, double high, double& value, bool integer = false) {
    const auto* v = field(o, key);
    if (!cJSON_IsNumber(v) || !std::isfinite(v->valuedouble)) return false;
    value = v->valuedouble;
    return value >= low && value <= high && (!integer || std::floor(value) == value);
}
bool text(const cJSON* o, const char* key, std::string& value, size_t max) {
    const auto* v = field(o, key);
    if (!cJSON_IsString(v) || !v->valuestring || std::strlen(v->valuestring) > max) return false;
    value = v->valuestring;
    return true;
}
bool uniqueKeys(const cJSON* v) {
    for (const auto* a = v->child; a; a = a->next) {
        if (cJSON_IsObject(v)) for (const auto* b = a->next; b; b = b->next)
            if (a->string && b->string && std::strcmp(a->string, b->string) == 0) return false;
        if (!uniqueKeys(a)) return false;
    }
    return true;
}
bool script(const cJSON* o, DemoScript& out) {
    double ms;
    if (!cJSON_IsObject(o) || !number(o, "timeout_ms", 100, 3600000, ms, true)) return false;
    out.timeoutMs = static_cast<uint32_t>(ms);
    const auto* commands = field(o, "commands");
    if (!cJSON_IsArray(commands) || cJSON_GetArraySize(commands) > kQueueMaxSteps) return false;
    size_t bytes = 0;
    for (const auto* c = commands->child; c; c = c->next) {
        if (!cJSON_IsString(c) || !c->valuestring) return false;
        std::string line(c->valuestring);
        if (line.find_first_of("\r\n") != std::string::npos) return false;
        bytes += line.size() + 1;
        if (bytes > kQueueMaxTextBytes) return false;
        if (line.find_first_not_of(" \t") != std::string::npos) out.commands.push_back(std::move(line));
    }
    return true;
}
class ConfigRotation : public QueueRotationSource {
public:
    explicit ConfigRotation(const DemoConfig& c) : config(c) {}
    bool rotationMm(uint8_t id, double& value) const override {
        for (const auto& a : config.axes) if (a.id == id && a.rotationMm > 0) { value = a.rotationMm; return true; }
        return false;
    }
    const DemoConfig& config;
};
}
bool buildDemoProgram(const DemoScript& script, const DemoConfig& config,
                      bool initializing, const std::array<int32_t, 256>& zeros,
                      QueueProgram& out, std::string& error) {
    out.count = 0; out.hasRaw = false;
    ConfigRotation rotation(config);
    std::unique_ptr<QueueProgram> one(new QueueProgram);
    for (size_t i = 0; i < script.commands.size(); ++i) {
        std::string line = script.commands[i];
        std::istringstream tokens(line);
        std::string verb;
        tokens >> verb;
        bool zero = verb == "zero";
        if (zero) {
            std::string id, speed, accel, decel, current, extra;
            if (initializing || !(tokens >> id >> speed >> accel >> decel >> current) || tokens >> extra) {
                error = "zero_syntax: zero ID RPM ACCEL DECEL CURRENT"; return false;
            }
            line = "move " + id + " 1 deg " + speed + " " + accel + " " + decel + " " + current + " await";
        }
        QueueError qe;
        if (!parseQueueProgram(line.c_str(), line.size(), rotation, *one, qe) || one->count != 1) {
            error = std::string("command_") + std::to_string(i + 1) + ":" + qe.message; return false;
        }
        auto step = one->steps[0];
        step.line = static_cast<uint16_t>(i + 1);
        const DemoAxis* axis = nullptr;
        for (const auto& a : config.axes) if (a.id == step.id) axis = &a;
        if (step.action != QueueAction::Wait && !axis) { error = "command_axis_not_declared"; return false; }
        if (step.action == QueueAction::Hex || step.action == QueueAction::Can ||
            ((step.action == QueueAction::Move || step.action == QueueAction::Home) && !step.awaitCompletion) ||
            ((step.action == QueueAction::Torque || step.action == QueueAction::Velocity) && !step.durationMs) ||
            (step.action == QueueAction::Home && (!initializing || step.mode != 2 || !axis->zero))) {
            error = "demo_requires_bounded_commands_and_collision_home_await"; return false;
        }
        if (zero) {
            if (!axis->zero) { error = "zero_axis_not_configured"; return false; }
            step.absolute = true; step.distanceTenths = zeros[step.id];
        }
        out.steps[out.count++] = step;
    }
    return true;
}
bool parseDemoConfig(const char* json, size_t length, DemoConfig& out, std::string& error) {
    error = "invalid_demo_json";
    if (!json || !length || length > kDemoMaxJsonBytes) return false;
    // Bound parser recursion before allocating. Reject embedded NUL strings.
    std::string source(json, length);
    if (source.find('\0') != std::string::npos || source.find("\\u0000") != std::string::npos) return false;
    int depth = 0; bool quoted = false, escape = false;
    for (char c : source) {
        if (quoted) { if (escape) escape = false; else if (c == '\\') escape = true; else if (c == '"') quoted = false; }
        else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > 16) return false; }
        else if (c == '}' || c == ']') { if (--depth < 0) return false; }
    }
    if (depth || quoted) return false;
    Json root(cJSON_ParseWithOpts(source.c_str(), nullptr, true), cJSON_Delete);
    if (!root || !cJSON_IsObject(root.get()) || !uniqueKeys(root.get())) return false;
    DemoConfig config;
    double value;
    if (!number(root.get(), "schema_version", 1, 1, value, true) ||
        !text(root.get(), "name", config.name, 64)) return false;
    const auto* display = field(root.get(), "display");
    if (!text(display, "baby_name", config.baby, 31) || !text(display, "formula_brand", config.brand, 31) ||
        !number(display, "water_ml", 0, 65535, value, true)) return false;
    config.waterMl = static_cast<uint16_t>(value);
    if (!number(display, "temperature_c", -32768, 32767, value, true)) return false;
    config.temperatureC = static_cast<int16_t>(value);
    const auto* axes = field(root.get(), "axes");
    if (!cJSON_IsArray(axes) || cJSON_GetArraySize(axes) > int(kDemoMaxAxes)) return false;
    for (const auto* a = axes->child; a; a = a->next) {
        DemoAxis axis;
        if (!number(a, "motor_id", 1, 255, value, true)) return false;
        axis.id = static_cast<uint8_t>(value);
        for (const auto& old : config.axes) if (old.id == axis.id) return false;
        if (!number(a, "rotation_distance_mm", 0, 1000000, axis.rotationMm)) return false;
        config.axes.push_back(axis);
    }
    const auto* init = field(root.get(), "initialization");
    if (!script(init, config.initialization)) return false;
    const auto* zeroAxes = field(init, "zero_axes");
    if (!cJSON_IsArray(zeroAxes)) return false;
    for (const auto* a = zeroAxes->child; a; a = a->next) {
        if (!number(a, "motor_id", 1, 255, value, true)) return false;
        DemoAxis* found = nullptr;
        for (auto& axis : config.axes) if (axis.id == value) found = &axis;
        if (!found || found->zero || !number(a, "zero_tolerance_deg", 0.1, 10, value)) return false;
        found->zero = true; found->tolerance = static_cast<int32_t>(std::round(value * 10));
    }
    const auto* stages = field(root.get(), "stages");
    if (!cJSON_IsArray(stages) || cJSON_GetArraySize(stages) != 5) return false;
    bool seen[5] = {};
    for (const auto* s = stages->child; s; s = s->next) {
        std::string id;
        if (!text(s, "id", id, 16)) return false;
        int index = -1;
        for (int i = 0; i < 5; ++i) if (id == kDemoStageIds[i]) index = i;
        if (index < 0 || seen[index] || !script(s, config.stages[index])) return false;
        seen[index] = true;
    }
    config.configured = !config.axes.empty() && cJSON_GetArraySize(zeroAxes) > 0 && !config.initialization.commands.empty();
    std::unique_ptr<QueueProgram> program(new QueueProgram);
    std::array<int32_t, 256> zeros{};
    if (!buildDemoProgram(config.initialization, config, true, zeros, *program, error)) return false;
    for (const auto& axis : config.axes) if (axis.zero) {
        bool homed = false;
        for (uint8_t i = 0; i < program->count; ++i)
            if (program->steps[i].id == axis.id && program->steps[i].action == QueueAction::Home) homed = true;
        if (!homed) config.configured = false;
    }
    for (const auto& s : config.stages) {
        if (!buildDemoProgram(s, config, false, zeros, *program, error)) return false;
        if (!program->count) config.configured = false;
    }
    out = std::move(config);
    error.clear(); return true;
}
bool demoRotationMatches(const DemoConfig& config, const QueueRotationSource& actual) {
    for (const auto& axis : config.axes) {
        double value = 0;
        const bool exists = actual.rotationMm(axis.id, value);
        if (axis.rotationMm == 0 ? exists : (!exists || std::abs(value - axis.rotationMm) > 0.0000005)) return false;
    }
    return true;
}
}
