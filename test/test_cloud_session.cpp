#include "CloudSession.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

using namespace babytech::cloud;

constexpr char a[] = "0123456789abcdef0123456789abcdef";
constexpr char b[] = "abcdef0123456789abcdef0123456789";
constexpr char challenge[] = "1234567890abcdef1234567890abcdef";

int main() {
    CloudSession session;
    SessionSnapshot snapshot;
    std::strcpy(snapshot.id, "unchanged");
    assert(!session.snapshot(0, snapshot));
    assert(!std::strcmp(snapshot.id, "unchanged"));
    assert(session.check(a, 1, 0, 5000, 0) == Freshness::Disconnected);
    assert(session.open(a, 1, 100));
    assert(session.snapshot(100, snapshot));
    assert(!std::strcmp(snapshot.id, a) && snapshot.generation == 1 && snapshot.uptimeMs == 100);
    assert(session.check(a, 1, 100, 5000, 100) == Freshness::Current);
    assert(session.check(a, 1, 100, 5000, 5100) == Freshness::Current);
    assert(session.check(a, 1, 100, 5000, 5101) == Freshness::Expired);
    assert(session.check(a, 1, 99, 5000, 101) == Freshness::Expired);
    assert(session.check(a, 1, 102, 5000, 101) == Freshness::Expired);
    assert(session.check(b, 1, 100, 5000, 101) == Freshness::WrongSession);
    assert(session.check(a, 2, 100, 5000, 101) == Freshness::WrongSession);
    for (uint32_t ttl : {0u, 1u, 4999u, 5001u, UINT32_MAX})
        assert(session.check(a, 1, 100, ttl, 101) == Freshness::InvalidTtl);
    ProbeReply reply;
    std::strcpy(reply.challenge, "unchanged");
    assert(!session.probe(b, challenge, 1, 102, reply));
    assert(!session.probe(a, challenge, 2, 102, reply));
    assert(!session.probe(a, "", 1, 102, reply));
    assert(!std::strcmp(reply.challenge, "unchanged"));
    assert(session.probe(a, challenge, 1, 102, reply));
    assert(!std::strcmp(reply.challenge, challenge));
    assert(!std::strcmp(reply.session.id, a) && reply.session.uptimeMs == 102);
    session.close();
    assert(session.check(a, 1, 100, 5000, 103) == Freshness::Disconnected);
    assert(!session.probe(a, challenge, 1, 103, reply));
    assert(!session.open(a, 2, 103)); // Never reuse the last session on reconnect.
    assert(!session.snapshot(103, snapshot));
    assert(session.open(b, 2, 103));
    assert(session.check(a, 1, 103, 5000, 104) == Freshness::WrongSession);
    assert(session.check(b, 2, 103, 5000, 104) == Freshness::Current);

    for (const std::string& bad : {std::string(), std::string(31, 'a'),
             std::string(33, 'a'), std::string(32, '0'), std::string(32, 'A'),
             std::string(32, 'g'), std::string(31, 'a') + "-"}) {
        assert(!session.open(bad.c_str(), 3, 200));
        assert(!session.snapshot(200, snapshot));
    }
    assert(!session.open(nullptr, 3, 200));
    assert(!session.open(a, 0, 200));
    assert(session.open(a, 3, 200));
    assert(session.snapshot(200 + kSessionLifetimeMs - 1, snapshot));
    assert(!session.snapshot(200 + kSessionLifetimeMs, snapshot));
    assert(session.check(a, 3, 200 + kSessionLifetimeMs, 5000,
                         200 + kSessionLifetimeMs) == Freshness::Disconnected);
    assert(!session.probe(a, challenge, 3, 200 + kSessionLifetimeMs, reply));
    assert(session.open(b, 4, 200 + kSessionLifetimeMs));

    CloudSession wrap;
    const uint32_t opened = UINT32_MAX - 1000;
    assert(wrap.open(a, 9, opened));
    assert(wrap.check(a, 9, UINT32_MAX - 10, 5000, 10) == Freshness::Current);
    assert(wrap.check(a, 9, UINT32_MAX - 10, 5000, 4990) == Freshness::Expired);
    assert(wrap.check(a, 9, 11, 5000, 10) == Freshness::Expired);
    assert(wrap.check(a, 9, opened - 1, 5000, 10) == Freshness::Expired);
    assert(wrap.snapshot(opened + kSessionLifetimeMs - 1, snapshot));
    assert(!wrap.snapshot(opened + kSessionLifetimeMs, snapshot));
    // Once polled expired, a later complete millis wrap cannot revive it.
    assert(!wrap.snapshot(opened, snapshot));
    std::puts("PASS cloud session, reconnect/replay, probe, TTL and uptime-wrap gates");
}
