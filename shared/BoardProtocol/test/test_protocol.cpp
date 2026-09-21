#include <BoardProtocol.h>
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace babytech;

bool feed(Parser& parser, const uint8_t* bytes, size_t count, Frame& result) {
    bool complete = false;
    for (size_t i = 0; i < count; ++i) {
        if (parser.push(bytes[i], result)) complete = true;
    }
    return complete;
}

int main() {
    uint8_t request[kMaxFrameSize]{};
    uint8_t response[kMaxFrameSize]{};
    Frame incoming{}, outgoing{};
    Parser motionParser, brainParser;

    // The observable first slice: a query reaches motion, and the matching
    // status returns to brain without pretending that motors are integrated.
    const size_t requestSize = encode(Type::GetStatus, 42, nullptr, 0, request, sizeof(request));
    assert(requestSize == 12);
    assert(!feed(motionParser, request, 5, incoming));
    assert(feed(motionParser, request + 5, requestSize - 5, incoming));
    const size_t responseSize = respondToStatusQuery(incoming, 1234, response, sizeof(response));
    assert(responseSize > 0);
    assert(feed(brainParser, response, responseSize, outgoing));
    Status status{};
    assert(readStatus(outgoing, status));
    assert(outgoing.sequence == 42);
    assert(status.uptimeMs == 1234);
    assert(status.state == MotionState::NotConfigured);
    assert(!status.motorsAvailable && !status.sensorsAvailable);

    // Noise, concatenated frames and a damaged frame must not become commands.
    Parser parser;
    const uint8_t noise[] = {0x11, 0x42, 0x42, 0x22};
    assert(!feed(parser, noise, sizeof(noise), incoming));
    uint8_t corrupt[kMaxFrameSize]{};
    std::memcpy(corrupt, request, requestSize);
    corrupt[6] ^= 0x80;
    assert(!feed(parser, corrupt, requestSize, incoming));
    assert(feed(parser, request, requestSize, incoming));
    assert(feed(parser, request, requestSize, incoming));
    assert(incoming.sequence == 42);

    // Reject a different protocol and malformed lengths; recover on a fresh frame.
    std::memcpy(corrupt, request, requestSize);
    corrupt[2] = 99;
    assert(!feed(parser, corrupt, requestSize, incoming));
    assert(feed(parser, request, requestSize, incoming));
    std::memcpy(corrupt, request, requestSize);
    corrupt[4] = 255;
    assert(!feed(parser, corrupt, requestSize, incoming));
    assert(feed(parser, request, requestSize, incoming));

    // A truncated frame is abandoned by the caller after a byte timeout.
    assert(!feed(parser, request, 8, incoming));
    parser.reset();
    assert(feed(parser, request, requestSize, incoming));

    assert(encode(Type::GetStatus, 1, nullptr, 1, request, sizeof(request)) == 0);
    assert(encode(Type::GetStatus, 1, nullptr, 0, request, 11) == 0);
    incoming.type = Type::Status;
    assert(respondToStatusQuery(incoming, 0, response, sizeof(response)) == 0);
    incoming.type = Type::GetStatus;
    incoming.length = 1;
    assert(respondToStatusQuery(incoming, 0, response, sizeof(response)) == 0);
    outgoing.length = 1;
    assert(!readStatus(outgoing, status));
    std::puts("PASS: UART query/response, framing, corruption and malformed input");
}
