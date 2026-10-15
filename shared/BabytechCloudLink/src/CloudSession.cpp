#include "CloudSession.h"

#include <cstring>

namespace babytech { namespace cloud {
namespace {
bool validToken(const char* value) {
    if (!value) return false;
    bool nonzero = false;
    for (unsigned i = 0; i < 32; ++i) {
        const char ch = value[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
        nonzero |= ch != '0';
    }
    return nonzero && value[32] == 0;
}
}

bool CloudSession::open(const char* randomId, uint32_t generation, uint32_t nowMs) {
    // Failed replacement must not leave the previous connection authorized.
    close();
    if (!generation || !validToken(randomId) || std::strcmp(randomId, id_) == 0)
        return false;
    std::memcpy(id_, randomId, sizeof(id_));
    generation_ = generation;
    openedAtMs_ = nowMs;
    active_ = true;
    return true;
}

void CloudSession::close() { active_ = false; }

void CloudSession::poll(uint32_t nowMs) {
    if (active_ && uint32_t(nowMs - openedAtMs_) >= kSessionLifetimeMs) close();
}

bool CloudSession::snapshot(uint32_t nowMs, SessionSnapshot& output) {
    poll(nowMs);
    if (!active_) return false;
    SessionSnapshot value;
    std::memcpy(value.id, id_, sizeof(value.id));
    value.generation = generation_;
    value.uptimeMs = nowMs;
    output = value;
    return true;
}

bool CloudSession::probe(const char* targetSession, const char* challenge,
                         uint32_t generation, uint32_t nowMs, ProbeReply& output) {
    poll(nowMs);
    if (!active_ || generation != generation_ || !validToken(targetSession) ||
        std::strcmp(targetSession, id_) || !validToken(challenge)) return false;
    ProbeReply value;
    if (!snapshot(nowMs, value.session)) return false;
    std::memcpy(value.challenge, challenge, sizeof(value.challenge));
    output = value;
    return true;
}

Freshness CloudSession::check(const char* commandSession, uint32_t generation,
                            uint32_t sampledAtMs, uint32_t ttlMs, uint32_t nowMs) {
    poll(nowMs);
    if (!active_) return Freshness::Disconnected;
    if (generation != generation_ || !validToken(commandSession) ||
        std::strcmp(commandSession, id_)) return Freshness::WrongSession;
    if (ttlMs != kCommandTtlMs) return Freshness::InvalidTtl;
    // Unsigned deltas cover wrap, reject future/pre-connection samples, and do
    // not extend the deadline while an inbound command waits for dispatch.
    if (uint32_t(nowMs - sampledAtMs) > ttlMs ||
        uint32_t(sampledAtMs - openedAtMs_) > uint32_t(nowMs - openedAtMs_))
        return Freshness::Expired;
    return Freshness::Current;
}

} }
