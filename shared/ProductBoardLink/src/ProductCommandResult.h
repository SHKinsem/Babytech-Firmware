#pragma once

#include "BoardProtocolV4.h"

namespace babytech { namespace boardlink {

struct CommandResult {
    v4::Source source = v4::Source::LocalTouch;
    uint64_t sequence = 0;
    char commandId[129]{};
    bool accepted = false;
    char reason[65]{};
};

// Ordinary COMMAND_RESULT only; Stop's seq=0/already_idle uses another path.
// Both codecs leave output unchanged on failure. Encoding clears the envelope.
bool encodeCommandResult(const CommandResult& result, v4::Message& output);
bool decodeCommandResult(const v4::Message& message, CommandResult& output);

} }
