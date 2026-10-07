#include "BoardProtocolV4.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace babytech::v4;

namespace {

typedef std::vector<uint8_t> Bytes;

Frame frame(Kind kind = Kind::Context, size_t length = 2) {
    Frame value;
    value.kind = kind;
    value.senderBoot = UINT64_C(0x0123456789abcdef);
    value.receiverBoot = UINT64_C(0xfedcba9876543210);
    value.messageId = UINT32_C(0x89abcdef);
    value.total = value.length = static_cast<uint16_t>(length);
    for (size_t i = 0; i < length && i < kMaxFragment; ++i)
        value.payload[i] = static_cast<uint8_t>(i * 37 + 11);
    return value;
}

Message message(size_t length = kMaxMessage) {
    const Frame seed = frame();
    Message value;
    value.kind = seed.kind;
    value.senderBoot = seed.senderBoot;
    value.receiverBoot = seed.receiverBoot;
    value.messageId = seed.messageId;
    value.length = static_cast<uint16_t>(length);
    for (size_t i = 0; i < length && i < kMaxMessage; ++i)
        value.payload[i] = static_cast<uint8_t>(i * 37 + 11);
    return value;
}

void sameFrame(const Frame& a, const Frame& b, bool entirePayload = false) {
    assert(a.kind == b.kind && a.senderBoot == b.senderBoot);
    assert(a.receiverBoot == b.receiverBoot && a.messageId == b.messageId);
    assert(a.total == b.total && a.offset == b.offset && a.length == b.length);
    assert(std::memcmp(a.payload, b.payload,
                       entirePayload ? kMaxFragment : a.length) == 0);
}

void sameMessage(const Message& a, const Message& b, bool entirePayload = false) {
    assert(a.kind == b.kind && a.senderBoot == b.senderBoot);
    assert(a.receiverBoot == b.receiverBoot && a.messageId == b.messageId);
    assert(a.length == b.length);
    assert(std::memcmp(a.payload, b.payload,
                       entirePayload ? kMaxMessage : a.length) == 0);
}

void sameStop(const StopRequest& a, const StopRequest& b, bool entireId = false) {
    assert(a.source == b.source && a.sequence == b.sequence && a.scope == b.scope);
    assert(std::memcmp(a.executionId, b.executionId, 16) == 0);
    assert(a.commandIdLength == b.commandIdLength);
    assert(std::memcmp(a.commandId, b.commandId,
                       entireId ? sizeof(a.commandId) : a.commandIdLength) == 0);
}

// Independent wire fixture builder: malformed headers must not go through encode().
void putLe(Bytes& bytes, size_t at, uint64_t value, size_t width) {
    for (size_t i = 0; i < width; ++i)
        bytes[at + i] = static_cast<uint8_t>(value >> (8 * i));
}

void checksum(Bytes& bytes) {
    uint16_t crc = 0xffff;
    for (size_t i = 4; i + 2 < bytes.size(); ++i) {
        crc ^= static_cast<uint16_t>(bytes[i]) << 8;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = static_cast<uint16_t>((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    putLe(bytes, bytes.size() - 2, crc, 2);
}

Bytes raw(const Frame& value) {
    Bytes bytes(kHeaderSize + value.length + 2, 0);
    std::memcpy(bytes.data(), "BTM4", 4);
    bytes[4] = 4;
    bytes[5] = static_cast<uint8_t>(value.kind);
    putLe(bytes, 8, value.senderBoot, 8);
    putLe(bytes, 16, value.receiverBoot, 8);
    putLe(bytes, 24, value.messageId, 4);
    putLe(bytes, 28, value.total, 2);
    putLe(bytes, 30, value.offset, 2);
    putLe(bytes, 32, value.length, 2);
    assert(value.length <= kMaxFragment);
    std::memcpy(bytes.data() + kHeaderSize, value.payload, value.length);
    checksum(bytes);
    return bytes;
}

Bytes encoded(const Frame& value) {
    Bytes bytes(kMaxFrame + 8, 0xa5);
    const size_t size = encode(value, bytes.data(), kMaxFrame);
    assert(size == kHeaderSize + value.length + 2);
    for (size_t i = size; i < bytes.size(); ++i) assert(bytes[i] == 0xa5);
    bytes.resize(size);
    assert(bytes == raw(value));
    return bytes;
}

size_t feed(Parser& parser, const Bytes& bytes, uint32_t now, Frame& output) {
    size_t count = 0;
    for (size_t i = 0; i < bytes.size(); ++i) {
        const Frame before = output;
        if (parser.push(bytes[i], now, output)) ++count;
        else sameFrame(output, before, true);
    }
    return count;
}

void rejectedWire(const Bytes& bytes) {
    Parser parser;
    Frame output = frame(Kind::Terminal, kMaxFragment);
    assert(feed(parser, bytes, 10, output) == 0);
    // After an incomplete/corrupt length, the inter-byte deadline restores framing.
    const Frame good = frame(Kind::Heartbeat, 0);
    assert(feed(parser, encoded(good), 111, output) == 1);
    sameFrame(output, good);
}

void rejectedFrame(const Frame& bad) {
    assert(!validFrame(bad));
    Bytes output(kMaxFrame + 8, 0xa5);
    const Bytes before = output;
    assert(encode(bad, output.data(), output.size()) == 0);
    assert(output == before);
    if (bad.length <= kMaxFragment) rejectedWire(raw(bad));
}

AssemblyResult accept(Assembler& assembler, const Frame& value, uint32_t now,
                      Message& output, AssemblyResult expected) {
    const Message before = output;
    const AssemblyResult actual = assembler.accept(value, now, output);
    assert(actual == expected);
    if (actual != AssemblyResult::Complete) sameMessage(output, before, true);
    return actual;
}

Frame part(const Message& value, size_t offset) {
    Frame result;
    assert(fragment(value, offset, result));
    assert(validFrame(result));
    return result;
}

StopRequest stop() {
    StopRequest value;
    value.commandIdLength = 1;
    value.commandId[0] = 's';
    return value;
}

Bytes stopBytes(const StopRequest& request) {
    Bytes output(160, 0xa5);
    const size_t size = encodeStop(request, output.data(), output.size());
    assert(size == 27u + request.commandIdLength);
    for (size_t i = size; i < output.size(); ++i) assert(output[i] == 0xa5);
    output.resize(size);
    assert(output[0] == static_cast<uint8_t>(request.source));
    for (size_t i = 0; i < 8; ++i)
        assert(output[i + 1] == static_cast<uint8_t>(request.sequence >> (i * 8)));
    assert(output[9] == static_cast<uint8_t>(request.scope));
    assert(std::memcmp(output.data() + 10, request.executionId, 16) == 0);
    assert(output[26] == request.commandIdLength);
    assert(std::memcmp(output.data() + 27, request.commandId, request.commandIdLength) == 0);
    StopRequest decoded;
    assert(decodeStop(output.data(), output.size(), decoded));
    sameStop(decoded, request);
    assert(decoded.commandId[decoded.commandIdLength] == '\0');
    return output;
}

void rejectedStopBytes(const Bytes& bytes) {
    StopRequest output = stop();
    std::memset(output.commandId, 'z', sizeof(output.commandId));
    const StopRequest before = output;
    assert(!decodeStop(bytes.data(), bytes.size(), output));
    sameStop(output, before, true);
}

void rejectedStop(const StopRequest& request) {
    Bytes output(160, 0xa5);
    const Bytes before = output;
    assert(encodeStop(request, output.data(), output.size()) == 0);
    assert(output == before);
}

void goldenAndKinds() {
    static_assert(kHeaderSize == 34 && kMaxFragment == 160 && kMaxMessage == 2047,
                  "v0.4 capacities");
    static_assert(kMaxFrame == 196 && kByteTimeoutMs == 100 && kMessageTimeoutMs == 1000,
                  "v0.4 frame/time limits");
    static_assert(kHeartbeatIntervalMs == 250 && kLinkTimeoutMs == 1500 &&
                  kStatusIntervalMs == 500, "v0.4 link policy");
    static_assert(kMaxSequence == UINT64_C(9223372036854775807), "sequence bound");
    const uint8_t golden[] = {
        0x42,0x54,0x4d,0x34,0x04,0x03,0x00,0x00,
        0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x55,0x3c
    };
    Frame heartbeat = frame(Kind::Heartbeat, 0);
    heartbeat.senderBoot = 1;
    heartbeat.receiverBoot = 2;
    heartbeat.messageId = 1;
    assert(encoded(heartbeat) == Bytes(golden, golden + sizeof(golden)));
    assert(isControl(Kind::Heartbeat) && isControl(Kind::Stop));
    assert(isControl(Kind::LinkAck) && isControl(Kind::LinkReject));
    assert(!isControl(Kind::Command) && !isControl(Kind::Context));

    assert(!isControl(Kind::Discovery));
    static_assert(uint8_t(Kind::Discovery) == 17, "Discovery wire kind");
    for (unsigned kind = 1; kind <= 17; ++kind) {
        Frame value = frame(static_cast<Kind>(kind));
        value.payload[0] = '{'; value.payload[1] = '}';
        if (value.kind == Kind::Heartbeat || value.kind == Kind::StatusQuery)
            value.total = value.length = 0;
        if (value.kind == Kind::Stop) {
            const Bytes payload = stopBytes(stop());
            value.total = value.length = static_cast<uint16_t>(payload.size());
            std::memcpy(value.payload, payload.data(), payload.size());
        }
        if (value.kind == Kind::Hello || value.kind == Kind::Discovery) value.receiverBoot = 0;
        assert(validFrame(value));
        Parser parser;
        Frame output;
        assert(feed(parser, encoded(value), 5, output) == 1);
        sameFrame(output, value);
        Assembler assembler;
        Message complete;
        accept(assembler, output, 5, complete, AssemblyResult::Complete);
        assert(complete.kind == value.kind && complete.length == value.length);
        assert(complete.senderBoot == value.senderBoot && complete.receiverBoot == value.receiverBoot);
        assert(complete.messageId == value.messageId);
        assert(std::memcmp(complete.payload, value.payload, value.length) == 0);
        assert(!assembler.active());
    }
    Frame full = frame(Kind::Context, kMaxFragment);
    full.total = kMaxMessage;
    full.offset = kMaxMessage - kMaxFragment;
    assert(encoded(full).size() == kMaxFrame);
    Parser parser;
    Frame output;
    assert(feed(parser, encoded(full), 0, output) == 1);
    sameFrame(output, full);
    full.messageId = UINT32_MAX;
    full.senderBoot = full.receiverBoot = UINT64_MAX;
    assert(feed(parser, encoded(full), 1, output) == 1);
    sameFrame(output, full);
}

void invalidHeadersAndCapacity() {
    Frame bad = frame(); bad.kind = static_cast<Kind>(0); rejectedFrame(bad);
    bad = frame(); bad.kind = static_cast<Kind>(18); rejectedFrame(bad);
    bad = frame(); bad.kind = static_cast<Kind>(255); rejectedFrame(bad);
    bad = frame(); bad.senderBoot = 0; rejectedFrame(bad);
    bad = frame(); bad.receiverBoot = 0; rejectedFrame(bad);
    bad = frame(); bad.messageId = 0; rejectedFrame(bad);
    bad = frame(); bad.total = kMaxMessage + 1; rejectedFrame(bad);
    bad = frame(); bad.length = kMaxFragment + 1; rejectedFrame(bad);
    bad = frame(); bad.offset = 1; rejectedFrame(bad);
    bad = frame(); bad.offset = UINT16_MAX; rejectedFrame(bad);
    bad = frame(); bad.total = 1; rejectedFrame(bad);
    bad = frame(); bad.total = 0; rejectedFrame(bad);
    bad = frame(Kind::Heartbeat, 1); rejectedFrame(bad);
    bad = frame(Kind::StatusQuery, 1); rejectedFrame(bad);
    bad = frame(Kind::Stop, 28); bad.total = 29; rejectedFrame(bad);
    bad = frame(Kind::Stop, 28); bad.total = 29; bad.offset = 1; rejectedFrame(bad);

    const Frame good = frame(Kind::Context, kMaxFragment);
    for (size_t capacity = 0; capacity < kMaxFrame; ++capacity) {
        Bytes output(kMaxFrame + 8, 0xa5);
        const Bytes before = output;
        assert(encode(good, output.data(), capacity) == 0);
        assert(output == before);
    }
    assert(encode(good, NULL, 0) == 0);
    const Bytes wire = encoded(good);
    const size_t fields[] = {4, 6, 7};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        Bytes bytes = wire;
        bytes[fields[i]] ^= 1;
        checksum(bytes);
        rejectedWire(bytes);
    }
    Bytes oversized = wire;
    oversized.insert(oversized.end() - 2, 0x55);
    putLe(oversized, 28, 161, 2);
    putLe(oversized, 32, 161, 2);
    checksum(oversized);
    rejectedWire(oversized);
    // Every single-bit mutation, including magic and CRC, must fail validation.
    for (size_t i = 0; i < wire.size(); ++i) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            Bytes bytes = wire;
            bytes[i] ^= static_cast<uint8_t>(1u << bit);
            rejectedWire(bytes);
        }
    }
    // Arbitrary payload bytes are legal here; JSON/Stop semantics are separate APIs.
    Frame opaque = frame(Kind::Command, 3);
    opaque.payload[0] = 0xff; opaque.payload[1] = 0; opaque.payload[2] = 0xc0;
    assert(validFrame(opaque));
    opaque.kind = Kind::Stop;
    assert(validFrame(opaque));
}

void parserStreaming() {
    const Frame value = frame(Kind::Context, kMaxFragment);
    const Bytes wire = encoded(value);
    for (size_t cut = 0; cut < wire.size(); ++cut) {
        Parser parser;
        Frame output = frame(Kind::Terminal, kMaxFragment);
        assert(feed(parser, Bytes(wire.begin(), wire.begin() + cut), 0, output) == 0);
        assert(feed(parser, Bytes(wire.begin() + cut, wire.end()), 99, output) == 1);
        sameFrame(output, value);

        Parser truncated;
        assert(feed(truncated, Bytes(wire.begin(), wire.begin() + cut), 0, output) == 0);
        assert(feed(truncated, wire, 101, output) == 1);
        sameFrame(output, value);
    }
    Parser parser;
    Frame output;
    const uint8_t noise[] = {0, 0xff, 'B', 'B', 'T', 'B', 'T', 'M', 0, 'B', 'T'};
    assert(feed(parser, Bytes(noise, noise + sizeof(noise)), 1, output) == 0);
    assert(feed(parser, wire, 1, output) == 1);
    Bytes corrupt = wire;
    corrupt.back() ^= 1;
    assert(feed(parser, corrupt, 2, output) == 0);
    assert(feed(parser, wire, 2, output) == 1);
    assert(feed(parser, wire, 2, output) == 1); // No codec-level complete-message dedup.
    parser.reset();
    assert(feed(parser, Bytes(wire.begin(), wire.begin() + 20), 3, output) == 0);
    parser.reset();
    assert(feed(parser, wire, 3, output) == 1);

    const uint32_t start = UINT32_MAX - 50;
    const Bytes prefix(wire.begin(), wire.begin() + 20);
    const Bytes suffix(wire.begin() + 20, wire.end());
    parser.reset();
    assert(feed(parser, prefix, start, output) == 0);
    assert(feed(parser, suffix, start + uint32_t(99), output) == 1);
    parser.reset();
    assert(feed(parser, prefix, start, output) == 0);
    assert(feed(parser, suffix, start + uint32_t(101), output) == 0);
    assert(feed(parser, wire, start + uint32_t(102), output) == 1);
    parser.reset();
    assert(feed(parser, prefix, start, output) == 0);
    assert(feed(parser, suffix, start + uint32_t(100), output) == 0);
    assert(feed(parser, wire, start + uint32_t(100), output) == 1);
    // Deadline is inter-byte, not total frame duration.
    parser.reset();
    for (size_t i = 0; i < wire.size(); ++i) {
        const Frame before = output;
        const bool ready = parser.push(wire[i], start + static_cast<uint32_t>(i * 99), output);
        assert(ready == (i + 1 == wire.size()));
        if (!ready) sameFrame(output, before, true);
    }
}

void discoveryFrames() {
    for (uint64_t receiver : {UINT64_C(0), UINT64_C(0xfedcba9876543210)}) {
        Frame input = frame(Kind::Discovery, kMaxFragment);
        input.receiverBoot = receiver;
        const Bytes wire = encoded(input);
        Parser parser;
        Frame output;
        assert(feed(parser, wire, 0, output) == 1);
        sameFrame(output, input);
        for (size_t i = 0; i < wire.size(); ++i) {
            for (unsigned bit = 0; bit < 8; ++bit) {
                Bytes corrupt = wire;
                corrupt[i] ^= uint8_t(1u << bit);
                rejectedWire(corrupt);
            }
        }
    }
    // Discovery is not control traffic and cannot bypass an active assembly.
    Assembler assembler;
    Message output;
    const Message context = message(321);
    accept(assembler, part(context, 0), 0, output, AssemblyResult::Incomplete);
    Frame discovery = frame(Kind::Discovery);
    discovery.receiverBoot = 0;
    ++discovery.messageId;
    accept(assembler, discovery, 1, output, AssemblyResult::Busy);
    assert(assembler.active());
    assembler.reset();
    accept(assembler, discovery, 2, output, AssemblyResult::Complete);
}

void fragmentsAndAssembly() {
    const size_t lengths[] = {1, 159, 160, 161, 320, 321, 2046, 2047};
    for (size_t n = 0; n < sizeof(lengths) / sizeof(lengths[0]); ++n) {
        const Message input = message(lengths[n]);
        Assembler assembler;
        Message output = message();
        size_t offset = 0;
        while (offset < input.length) {
            const Frame value = part(input, offset);
            const size_t remaining = input.length - offset;
            assert(value.length == (remaining < kMaxFragment ? remaining : kMaxFragment));
            assert(value.total == input.length && value.offset == offset);
            assert(value.kind == input.kind && value.messageId == input.messageId);
            assert(value.senderBoot == input.senderBoot && value.receiverBoot == input.receiverBoot);
            assert(std::memcmp(value.payload, input.payload + offset, value.length) == 0);
            Parser parser;
            Frame decoded;
            assert(feed(parser, encoded(value), 0, decoded) == 1);
            offset += value.length;
            accept(assembler, decoded, static_cast<uint32_t>(offset / 10), output,
                   offset == input.length ? AssemblyResult::Complete : AssemblyResult::Incomplete);
            assert(assembler.active() == (offset != input.length));
        }
        sameMessage(output, input);
    }
    const Message input = message();
    Frame output = frame(Kind::Terminal, kMaxFragment);
    const Frame before = output;
    const size_t badOffsets[] = {kMaxMessage, kMaxMessage + 1, 65536, static_cast<size_t>(-1)};
    for (size_t i = 0; i < sizeof(badOffsets) / sizeof(badOffsets[0]); ++i) {
        assert(!fragment(input, badOffsets[i], output));
        sameFrame(output, before, true);
    }
    Message invalid = input;
    invalid.length = kMaxMessage + 1;
    assert(!fragment(invalid, 0, output));
    sameFrame(output, before, true);
    invalid = input; invalid.messageId = 0;
    assert(!fragment(invalid, 0, output));
    sameFrame(output, before, true);
    Message empty = message(0);
    empty.kind = Kind::Heartbeat;
    assert(fragment(empty, 0, output));
    assert(output.total == 0 && output.offset == 0 && output.length == 0);
    assert(validFrame(output));

    // A UTF-8 code point may straddle the 160-byte wire boundary.
    Message utf = message(162);
    std::memset(utf.payload, 'a', utf.length);
    utf.payload[159] = 0xe4; utf.payload[160] = 0xb8; utf.payload[161] = 0xad;
    assert(validUtf8(utf.payload, utf.length));
    Assembler assembler;
    Message complete;
    accept(assembler, part(utf, 0), 0, complete, AssemblyResult::Incomplete);
    accept(assembler, part(utf, 160), 1, complete, AssemblyResult::Complete);
    sameMessage(complete, utf);
}

void assemblyRejectionsAndControls() {
    const Message input = message(481);
    const Frame first = part(input, 0);
    Assembler assembler;
    Message output = message();
    accept(assembler, first, 0, output, AssemblyResult::Incomplete);
    accept(assembler, first, 1, output, AssemblyResult::DuplicateFragment);
    accept(assembler, part(input, 160), 2, output, AssemblyResult::Incomplete);
    accept(assembler, first, 3, output, AssemblyResult::DuplicateFragment);
    assert(assembler.active());
    Message other = input; ++other.messageId;
    accept(assembler, part(other, 0), 4, output, AssemblyResult::Busy);
    assert(assembler.active());
    accept(assembler, part(input, 320), 5, output, AssemblyResult::Incomplete);
    accept(assembler, part(input, 480), 6, output, AssemblyResult::Complete);
    sameMessage(output, input);

    assembler.reset();
    accept(assembler, part(input, 160), 0, output, AssemblyResult::Invalid);
    assert(!assembler.active());
    accept(assembler, first, 1, output, AssemblyResult::Incomplete);
    Frame conflict = first; conflict.payload[50] ^= 1;
    accept(assembler, conflict, 2, output, AssemblyResult::Invalid);
    assert(!assembler.active());
    accept(assembler, part(input, 160), 3, output, AssemblyResult::Invalid);
    accept(assembler, first, 4, output, AssemblyResult::Incomplete);
    accept(assembler, part(input, 320), 5, output, AssemblyResult::Invalid);
    assert(!assembler.active());
    accept(assembler, first, 6, output, AssemblyResult::Incomplete);
    Frame wrongTotal = part(input, 160); ++wrongTotal.total;
    accept(assembler, wrongTotal, 7, output, AssemblyResult::Invalid);
    assert(!assembler.active());
    accept(assembler, first, 8, output, AssemblyResult::Incomplete);
    assembler.reset();
    assert(!assembler.active() && !assembler.expire(5000));

    accept(assembler, first, 10, output, AssemblyResult::Incomplete);
    const Kind controls[] = {Kind::Stop, Kind::Heartbeat, Kind::LinkAck, Kind::LinkReject};
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {
        Frame control = frame(controls[i], controls[i] == Kind::Heartbeat ? 0 : 2);
        control.messageId += static_cast<uint32_t>(i + 1);
        if (control.kind == Kind::Stop) {
            const Bytes payload = stopBytes(stop());
            control.total = control.length = static_cast<uint16_t>(payload.size());
            std::memcpy(control.payload, payload.data(), payload.size());
        }
        accept(assembler, control, 11 + static_cast<uint32_t>(i), output, AssemblyResult::Complete);
        assert(output.kind == control.kind && output.messageId == control.messageId);
        assert(output.length == control.length);
        assert(std::memcmp(output.payload, control.payload, control.length) == 0);
        assert(assembler.active());
    }
    accept(assembler, part(input, 160), 20, output, AssemblyResult::Incomplete);
    accept(assembler, part(input, 320), 21, output, AssemblyResult::Incomplete);
    accept(assembler, part(input, 480), 22, output, AssemblyResult::Complete);
    sameMessage(output, input);
    // A new assembly of identical bytes is legal; endpoint owns complete-message dedup.
    accept(assembler, first, 23, output, AssemblyResult::Incomplete);
    assembler.reset();
}

void assemblyDeadlines() {
    const Message input = message(321);
    const Frame first = part(input, 0);
    const uint32_t starts[] = {0, UINT32_MAX - 500};
    for (size_t i = 0; i < sizeof(starts) / sizeof(starts[0]); ++i) {
        const uint32_t start = starts[i];
        Assembler assembler;
        Message output = message();
        assert(!assembler.expire(start));
        accept(assembler, first, start, output, AssemblyResult::Incomplete);
        assert(!assembler.expire(start + uint32_t(999)));
        accept(assembler, first, start + uint32_t(999), output, AssemblyResult::DuplicateFragment);
        assert(assembler.expire(start + uint32_t(1000)));
        assert(!assembler.active() && !assembler.expire(start + uint32_t(1002)));
        accept(assembler, part(input, 160), start + uint32_t(1002), output, AssemblyResult::Invalid);

        accept(assembler, first, start, output, AssemblyResult::Incomplete);
        accept(assembler, part(input, 160), start + uint32_t(900), output, AssemblyResult::Incomplete);
        accept(assembler, first, start + uint32_t(999), output, AssemblyResult::DuplicateFragment);
        // accept() must enforce the total deadline even without an explicit expire().
        accept(assembler, part(input, 320), start + uint32_t(1000), output, AssemblyResult::Invalid);
        assert(!assembler.active());

        accept(assembler, first, start, output, AssemblyResult::Incomplete);
        accept(assembler, frame(Kind::Heartbeat, 0), start + uint32_t(999), output,
               AssemblyResult::Complete);
        assert(assembler.expire(start + uint32_t(1001)));
    }
}

void utf8AndStop() {
    const Bytes valid[] = {
        Bytes(), Bytes{0x01, 0x7f}, Bytes{0xc2, 0x80}, Bytes{0xdf, 0xbf},
        Bytes{0xe0, 0xa0, 0x80}, Bytes{0xed, 0x9f, 0xbf}, Bytes{0xee, 0x80, 0x80},
        Bytes{0xef, 0xbf, 0xbf}, Bytes{0xf0, 0x90, 0x80, 0x80},
        Bytes{0xf4, 0x8f, 0xbf, 0xbf}
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i)
        assert(validUtf8(valid[i].data(), valid[i].size()));
    const Bytes invalid[] = {
        Bytes{0}, Bytes{'a', 0, 'b'}, Bytes{0x80}, Bytes{0xbf}, Bytes{0xc0, 0x80},
        Bytes{0xc1, 0xbf}, Bytes{0xc2}, Bytes{0xc2, 'x'}, Bytes{0xe0, 0x9f, 0xbf},
        Bytes{0xed, 0xa0, 0x80}, Bytes{0xed, 0xbf, 0xbf}, Bytes{0xe1, 0x80},
        Bytes{0xf0, 0x8f, 0xbf, 0xbf}, Bytes{0xf1, 0x80, 0x80},
        Bytes{0xf4, 0x90, 0x80, 0x80}, Bytes{0xf5, 0x80, 0x80, 0x80}, Bytes{0xff}
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(!validUtf8(invalid[i].data(), invalid[i].size()));
        StopRequest bad = stop();
        bad.commandIdLength = static_cast<uint8_t>(invalid[i].size());
        std::memcpy(bad.commandId, invalid[i].data(), invalid[i].size());
        rejectedStop(bad);
        Bytes bytes = stopBytes(stop());
        bytes.resize(27 + invalid[i].size());
        bytes[26] = bad.commandIdLength;
        std::memcpy(bytes.data() + 27, invalid[i].data(), invalid[i].size());
        rejectedStopBytes(bytes);
    }

    for (unsigned source = 1; source <= 2; ++source) {
        for (unsigned scope = 1; scope <= 3; ++scope) {
            StopRequest request = stop();
            request.source = static_cast<Source>(source);
            request.sequence = source == 1 ? kMaxSequence : 0;
            request.scope = static_cast<StopScope>(scope);
            if (scope != 3) request.executionId[15] = 0x80;
            stopBytes(request);
            if (source == 1) { request.sequence = 1; stopBytes(request); }
        }
    }
    StopRequest maximum = stop();
    maximum.commandIdLength = 128;
    std::memset(maximum.commandId, 'a', 125);
    maximum.commandId[125] = static_cast<char>(0xe4);
    maximum.commandId[126] = static_cast<char>(0xb8);
    maximum.commandId[127] = static_cast<char>(0xad);
    maximum.commandId[128] = '\0';
    const Bytes bytes = stopBytes(maximum);
    assert(bytes.size() == 155);
    Frame stopFrame = frame(Kind::Stop, bytes.size());
    std::memcpy(stopFrame.payload, bytes.data(), bytes.size());
    assert(encoded(stopFrame).size() == 191);
    for (size_t capacity = 0; capacity < bytes.size(); ++capacity) {
        Bytes output(160, 0xa5);
        const Bytes before = output;
        assert(encodeStop(maximum, output.data(), capacity) == 0);
        assert(output == before);
    }
    assert(encodeStop(maximum, NULL, 0) == 0);
    for (size_t length = 0; length < bytes.size(); ++length)
        rejectedStopBytes(Bytes(bytes.begin(), bytes.begin() + length));
    Bytes extra = bytes; extra.push_back('a'); rejectedStopBytes(extra);
    extra[26] = 129; rejectedStopBytes(extra);
    extra = bytes; extra[26] = 0; rejectedStopBytes(extra);

    StopRequest bad = stop(); bad.source = static_cast<Source>(0); rejectedStop(bad);
    bad = stop(); bad.source = static_cast<Source>(3); rejectedStop(bad);
    bad = stop(); bad.sequence = 1; rejectedStop(bad);
    bad = stop(); bad.source = Source::CloudCommand; rejectedStop(bad);
    bad.sequence = kMaxSequence + 1; rejectedStop(bad);
    bad.sequence = UINT64_MAX; rejectedStop(bad);
    bad = stop(); bad.scope = static_cast<StopScope>(0); rejectedStop(bad);
    bad = stop(); bad.scope = static_cast<StopScope>(4); rejectedStop(bad);
    bad = stop(); bad.executionId[0] = 1; rejectedStop(bad);
    bad = stop(); bad.scope = StopScope::Product; rejectedStop(bad);
    bad = stop(); bad.scope = StopScope::Workbench; rejectedStop(bad);
    bad = stop(); bad.commandIdLength = 0; rejectedStop(bad);
    bad = maximum; bad.commandIdLength = 129; rejectedStop(bad);
    bad = maximum; bad.commandIdLength = 255; rejectedStop(bad);
    bad = maximum; bad.commandIdLength = 127; rejectedStop(bad); // Split UTF-8 tail.

    const Bytes base = stopBytes(stop());
    const size_t indexes[] = {0, 0, 1, 9, 9, 10, 9, 9, 26};
    const uint8_t values[] = {0, 3, 1, 0, 4, 1, 1, 2, 0};
    for (size_t i = 0; i < sizeof(indexes) / sizeof(indexes[0]); ++i) {
        Bytes malformed = base;
        malformed[indexes[i]] = values[i];
        rejectedStopBytes(malformed);
    }
    Bytes cloud = base; cloud[0] = 1;
    rejectedStopBytes(cloud); // Cloud Stop cannot use the local-only zero sequence.
    putLe(cloud, 1, kMaxSequence + 1, 8); rejectedStopBytes(cloud);
    putLe(cloud, 1, UINT64_MAX, 8); rejectedStopBytes(cloud);
}

} // namespace

int main() {
    goldenAndKinds();
    invalidHeadersAndCapacity();
    parserStreaming();
    discoveryFrames();
    fragmentsAndAssembly();
    assemblyRejectionsAndControls();
    assemblyDeadlines();
    utf8AndStop();
    std::puts("BoardProtocol v4 contract tests passed");
    return 0;
}
