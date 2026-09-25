#pragma once
#include "DemoFlowController.h"
#include "QueueProgram.h"

namespace motion {
extern const char* const kDemoStageIds[5];
// Parses into a temporary config; callers swap only after successful validation.
bool parseDemoConfig(const char* json, size_t length, DemoConfig& out, std::string& error,
                     bool unverified = false);
bool buildDemoProgram(const DemoScript& script, const DemoConfig& config,
                      bool initializing, const std::array<int32_t, 256>& zeros,
                      QueueProgram& out, std::string& error, bool unverified = false);
bool demoRotationMatches(const DemoConfig& config, const QueueRotationSource& actual);
}
