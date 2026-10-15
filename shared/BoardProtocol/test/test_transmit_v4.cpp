#include "BoardTransmitV4.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace babytech::v4;

namespace {
Message make(Kind kind, uint32_t id, size_t length) {
    Message message;
    message.kind = kind; message.messageId = id;
    message.senderBoot = 1; message.receiverBoot = 2;
    message.length = uint16_t(length);
    for (size_t i = 0; i < length; ++i) message.payload[i] = uint8_t(i);
    return message;
}

class Sink : public ByteSink {
public:
    bool empty = true;
    bool zeroWrite = false;
    bool invalidCount = false;
    size_t room = kMaxFrame;
    size_t maxWrite = kMaxFrame;
    std::vector<uint8_t> bytes;
    bool idle() const override { return empty; }
    size_t available() const override { return room; }
    size_t write(const uint8_t* data, size_t size) override {
        assert(size <= room);
        if (invalidCount) return size + 1;
        if (zeroWrite) return 0;
        if (size > maxWrite) size = maxWrite;
        bytes.insert(bytes.end(), data, data + size);
        empty = false;
        return size;
    }
};

std::vector<Frame> frames(const Sink& sink) {
    Parser parser;
    Frame frame;
    std::vector<Frame> result;
    for (uint8_t byte : sink.bytes) if (parser.push(byte, 0, frame)) result.push_back(frame);
    return result;
}

void drain(Transmitter& tx, Sink& sink) {
    for (size_t i = 0; i < 2000 && tx.pending(); ++i) {
        sink.empty = true;
        assert(tx.pump(sink));
    }
    assert(!tx.pending());
}

void priorityAndBounds() {
    static_assert(sizeof(Transmitter) + sizeof(Parser) + sizeof(Assembler) +
                  sizeof(Message) + 2 * sizeof(Frame) + 256 <= 8192,
                  "transport buffers including RX driver ring and nested receipt");
    Transmitter tx;
    Sink sink;
    const Message ordinary = make(Kind::Context, 1, kMaxMessage);
    assert(tx.enqueue(ordinary));
    assert(!tx.enqueue(ordinary));
    assert(tx.pump(sink));
    assert(sink.bytes.size() == kMaxFrame);
    assert(!tx.pump(sink)); // No second frame in a still-busy driver.
    assert(tx.enqueue(make(Kind::LinkAck, 2, 2)));
    assert(tx.enqueue(make(Kind::StatusQuery, 3, 0)));
    assert(!tx.enqueue(make(Kind::LinkReject, 4, 2)));
    assert(tx.enqueue(make(Kind::Heartbeat, 5, 0)));
    assert(!tx.enqueue(make(Kind::Heartbeat, 6, 0)));
    assert(tx.enqueue(make(Kind::Stop, 7, 28)));
    assert(!tx.enqueue(make(Kind::Stop, 8, 28)));
    assert(!tx.pump(sink));
    drain(tx, sink);
    const std::vector<Frame> sent = frames(sink);
    assert(sent.size() == 17);
    assert(sent[0].kind == Kind::Context && sent[0].offset == 0);
    assert(sent[1].kind == Kind::Stop && sent[1].messageId == 7);
    assert(sent[2].kind == Kind::Heartbeat);
    assert(sent[3].kind == Kind::LinkAck && sent[4].kind == Kind::StatusQuery);
    Assembler assembler;
    Message result;
    for (const Frame& frame : sent) assembler.accept(frame, 0, result);
    assert(result.kind == Kind::Context && result.length == ordinary.length);
    assert(std::memcmp(result.payload, ordinary.payload, ordinary.length) == 0);
}

void partialAndCancellation() {
    Transmitter tx;
    Sink sink;
    sink.room = 7;
    assert(tx.enqueue(make(Kind::Command, 1, 400)));
    assert(tx.pump(sink) && sink.bytes.size() == 7);
    assert(tx.enqueue(make(Kind::Stop, 2, 28)));
    tx.cancelOrdinary();
    drain(tx, sink);
    const auto sent = frames(sink);
    assert(sent.size() == 2);
    assert(sent[0].kind == Kind::Command && sent[0].length == kMaxFragment);
    assert(sent[1].kind == Kind::Stop);
    assert(!tx.ordinaryPending());
    assert(tx.enqueue(make(Kind::Context, 3, 5)));
    tx.cancelOrdinary();
    assert(!tx.pending());

    sink = Sink{};
    sink.zeroWrite = true;
    assert(tx.enqueue(make(Kind::Context, 4, 300)));
    assert(!tx.pump(sink));
    assert(tx.enqueue(make(Kind::Stop, 5, 28)));
    sink.zeroWrite = false;
    assert(tx.pump(sink));
    assert(frames(sink).front().kind == Kind::Stop);
    tx.cancelOrdinary();
    assert(!tx.pending());
}

void backpressureAndBadSink() {
    Transmitter tx;
    Sink sink;
    Message invalid = make(Kind::Context, 0, 10);
    assert(!tx.enqueue(invalid) && !tx.pending());
    invalid = make(Kind::Stop, 1, 161);
    assert(!tx.enqueue(invalid));
    Frame control;
    assert(fragment(make(Kind::Context, 1, 10), 0, control));
    assert(!tx.enqueueControl(control));
    assert(fragment(make(Kind::Heartbeat, 1, 0), 0, control));
    control.messageId = 0;
    assert(!tx.enqueueControl(control));
    control.messageId = 1;
    assert(tx.enqueueControl(control));
    drain(tx, sink);
    assert(frames(sink).size() == 1 && frames(sink)[0].kind == Kind::Heartbeat);
    sink = Sink{};
    assert(tx.enqueue(make(Kind::Context, 1, 1)));
    sink.room = 0;
    assert(!tx.pump(sink) && tx.pending() && sink.bytes.empty());
    tx.cancelOrdinary();
    assert(!tx.pending());
    assert(tx.enqueue(make(Kind::Context, 2, 1)));
    sink.room = kMaxFrame;
    sink.invalidCount = true;
    assert(!tx.pump(sink) && !tx.healthy());
    assert(!tx.enqueue(make(Kind::Context, 3, 1)));
    assert(!tx.pump(sink));
    tx.reset();
    assert(tx.healthy() && !tx.pending());
}

void shortWritesKeepFrameAtomic() {
    for (size_t first = 1; first < kMaxFrame; ++first) {
        Transmitter tx;
        Sink sink;
        sink.maxWrite = first;
        assert(tx.enqueue(make(Kind::Context, 1, 400)));
        assert(tx.pump(sink) && sink.bytes.size() == first);
        assert(tx.enqueue(make(Kind::Stop, 2, 28)));
        sink.zeroWrite = true;
        assert(!tx.pump(sink) && sink.bytes.size() == first);
        sink.zeroWrite = false;
        sink.maxWrite = kMaxFrame;
        drain(tx, sink);
        const auto sent = frames(sink);
        assert(sent.size() == 4);
        assert(sent[0].kind == Kind::Context && sent[0].offset == 0);
        assert(sent[1].kind == Kind::Stop);
        assert(sent[2].kind == Kind::Context && sent[2].offset == kMaxFragment);
        assert(sent[3].kind == Kind::Context && sent[3].offset == 2 * kMaxFragment);
    }
}

std::vector<uint8_t> encoded(const Message& message, size_t offset = 0) {
    Frame frame;
    assert(fragment(message, offset, frame));
    uint8_t wire[kMaxFrame];
    const size_t size = encode(frame, wire, sizeof(wire));
    assert(size);
    return std::vector<uint8_t>(wire, wire + size);
}

void timedValidationAndStickyOutcome() {
    Transmitter tx;
    Sink sink;
    const Message command = make(Kind::Command, 1, 5);
    assert(!tx.enqueueTimedOrdinary(make(Kind::Context, 2, 5), 0, 100));
    assert(!tx.enqueueTimedOrdinary(make(Kind::Stop, 2, 28), 0, 100));
    assert(!tx.enqueueTimedOrdinary(command, 0, 0));
    assert(!tx.enqueueTimedOrdinary(command, 0, 5001));
    assert(!tx.enqueueTimedOrdinary(command, 0, 100, 0));
    assert(!tx.enqueueTimedOrdinary(command, 0, 50));
    assert(!tx.enqueueTimedOrdinary(command, 0, 49));
    assert(!tx.enqueueTimedOrdinary(command, 0, 1, 1));
    assert(!tx.enqueueTimedOrdinary(command, 0, 100, UINT32_MAX));
    assert(!tx.enqueueTimedOrdinary(make(Kind::Command, 0, 5), 0, 100));
    assert(!tx.pending() && !tx.ordinaryTimedOut() && tx.healthy());
    assert(tx.enqueueTimedOrdinary(command, 100, 5000));
    assert(!tx.enqueueTimedOrdinary(command, 149, 5000)); // Cannot refresh origin.
    sink.empty = false;
    assert(!tx.pump(150, sink)); // Check expiration even while driver is busy.
    assert(!tx.pending() && !tx.ordinaryPending() && tx.ordinaryTimedOut());
    assert(sink.bytes.empty() && tx.healthy());
    assert(!tx.enqueueTimedOrdinary(command, 150, 50));
    assert(!tx.enqueue(make(Kind::Command, 0, 5)));
    tx.cancelOrdinary();
    tx.invalidateOrdinary();
    assert(!tx.pump(1000, sink) && tx.ordinaryTimedOut());
    assert(tx.enqueue(command) && !tx.ordinaryTimedOut());
    tx.cancelOrdinary();
    assert(tx.enqueueTimedOrdinary(command, 100, 100));
    assert(!tx.pump(150, sink) && tx.ordinaryTimedOut());
    assert(tx.enqueue(make(Kind::Heartbeat, 2, 0)) && !tx.ordinaryTimedOut());
    tx.reset();
    assert(!tx.pending() && !tx.ordinaryTimedOut() && tx.healthy());

    assert(tx.enqueueTimedOrdinary(command, 0, 2, 1));
    sink = Sink{};
    assert(tx.pump(0, sink)); // Minimal valid TTL/budget.
    assert(!tx.pending() && !tx.ordinaryTimedOut());
    assert(frames(sink).size() == 1);
    assert(!tx.pump(2, sink) && !tx.ordinaryTimedOut());
}

void clocklessGuardAndControlPriority() {
    Transmitter tx;
    Sink sink;
    assert(tx.enqueueTimedOrdinary(make(Kind::Command, 1, 400), 100, 500));
    assert(!tx.pump(sink) && sink.bytes.empty() && tx.ordinaryPending());
    assert(tx.enqueue(make(Kind::LinkAck, 2, 2)));
    assert(tx.enqueue(make(Kind::Heartbeat, 3, 0)));
    assert(tx.enqueue(make(Kind::Stop, 4, 28)));
    for (int i = 0; i < 3; ++i) {
        sink.empty = true;
        assert(tx.pump(sink));
    }
    auto sent = frames(sink);
    assert(sent.size() == 3 && sent[0].kind == Kind::Stop &&
           sent[1].kind == Kind::Heartbeat && sent[2].kind == Kind::LinkAck);
    sink.empty = true;
    assert(!tx.pump(sink) && tx.ordinaryPending());
    sink.maxWrite = 7;
    assert(tx.pump(149, sink));
    const size_t partialSize = sink.bytes.size();
    assert(tx.enqueue(make(Kind::Stop, 5, 28)));
    assert(!tx.pump(sink) && sink.bytes.size() == partialSize);
    assert(!tx.ordinaryTimedOut() && tx.healthy());
    sink.zeroWrite = true;
    assert(!tx.pump(150, sink) && tx.ordinaryTimedOut());
    assert(tx.pending() && tx.ordinaryPending()); // Residual bytes still queued.
    sink.zeroWrite = false;
    sink.maxWrite = kMaxFrame;
    drain(tx, sink); // Invalidated bytes no longer need a clock.
    sent = frames(sink);
    assert(sent.size() == 4 && sent.back().messageId == 5);
    assert(tx.ordinaryTimedOut() && !tx.ordinaryPending());
}

void expiredBeforeFirstWrite() {
    for (int mode = 0; mode < 3; ++mode) {
        Transmitter tx;
        Sink sink;
        assert(tx.enqueueTimedOrdinary(make(Kind::Command, 1, 400), 100, 500));
        if (mode == 0) sink.zeroWrite = true;
        if (mode == 1) sink.room = 0;
        if (mode == 2) sink.empty = false;
        assert(!tx.pump(149, sink) && tx.pending());
        assert(tx.enqueue(make(Kind::Stop, 2, 28)));
        assert(tx.enqueue(make(Kind::Heartbeat, 3, 0)));
        assert(tx.enqueue(make(Kind::StatusQuery, 4, 0)));
        assert(!tx.pump(150, sink));
        assert(tx.ordinaryTimedOut() && !tx.ordinaryPending() && tx.pending());
        sink.zeroWrite = false;
        sink.room = kMaxFrame;
        drain(tx, sink);
        const auto sent = frames(sink);
        assert(sent.size() == 3 && sent[0].kind == Kind::Stop &&
               sent[1].kind == Kind::Heartbeat && sent[2].kind == Kind::StatusQuery);
        assert(tx.ordinaryTimedOut());
    }
}

void partialInvalidationAtEveryByte() {
    // Includes both CRC cut points and the final-byte-only residual, for short
    // and full frames. Actual parser must reject the complete poisoned frame.
    const size_t lengths[] = {1, kMaxFragment, 400};
    for (size_t length : lengths) {
        const Message command = make(Kind::Command, 1, length);
        const auto original = encoded(command);
        for (size_t cut = 1; cut < original.size(); ++cut) {
            for (int timeout = 0; timeout < 2; ++timeout) {
                Transmitter tx;
                Sink sink;
                sink.maxWrite = cut;
                assert(tx.enqueueTimedOrdinary(command, 100, 500));
                assert(tx.pump(149, sink) && sink.bytes.size() == cut);
                assert(tx.enqueue(make(Kind::Stop, 2, 28)));
                assert(tx.enqueue(make(Kind::Heartbeat, 3, 0)));
                sink.zeroWrite = true;
                if (timeout) {
                    assert(!tx.pump(150, sink));
                } else {
                    tx.invalidateOrdinary();
                }
                assert(tx.ordinaryTimedOut() == bool(timeout));
                assert(tx.pending() && tx.ordinaryPending());
                assert(!tx.enqueueTimedOrdinary(command, 150, 500));
                tx.invalidateOrdinary();
                tx.invalidateOrdinary(); // Must not flip the CRC back.
                assert(!tx.pump(600, sink) && sink.bytes.size() == cut);
                sink.zeroWrite = false;
                sink.maxWrite = 1; // Repeated short drains preserve atomicity.
                drain(tx, sink);
                auto poisoned = original;
                poisoned.back() ^= 1;
                assert(sink.bytes.size() == original.size() +
                       encoded(make(Kind::Stop, 2, 28)).size() +
                       encoded(make(Kind::Heartbeat, 3, 0)).size());
                assert(std::memcmp(sink.bytes.data(), poisoned.data(), poisoned.size()) == 0);
                const auto sent = frames(sink);
                assert(sent.size() == 2 && sent[0].kind == Kind::Stop &&
                       sent[1].kind == Kind::Heartbeat);
                assert(!tx.pending() && !tx.ordinaryPending() && tx.healthy());
                assert(tx.ordinaryTimedOut() == bool(timeout));
            }
        }
    }
}

void firstFrameBudgetAndOriginalTtl() {
    const Message command = make(Kind::Command, 1, 400);
    Transmitter tx;
    Sink sink;
    assert(tx.enqueueTimedOrdinary(command, 100, 100));
    assert(tx.pump(149, sink));
    sink.empty = true;
    assert(tx.pump(150, sink)); // First frame complete: budget no longer applies.
    sink.empty = true;
    assert(tx.pump(199, sink));
    assert(!tx.pending() && !tx.ordinaryTimedOut());
    auto sent = frames(sink);
    assert(sent.size() == 3);
    Assembler assembler;
    Message result;
    assert(assembler.accept(sent[0], 149, result) == AssemblyResult::Incomplete);
    assert(assembler.accept(sent[1], 150, result) == AssemblyResult::Incomplete);
    assert(assembler.accept(sent[2], 199, result) == AssemblyResult::Complete);
    assert(result.length == command.length &&
           std::memcmp(result.payload, command.payload, command.length) == 0);

    // Expiry during the next frame must use the original deadline, not the
    // first-frame completion or the start of this partial frame.
    const auto second = encoded(command, kMaxFragment);
    for (size_t cut = 0; cut < second.size(); ++cut) {
        tx.reset();
        sink = Sink{};
        assert(tx.enqueueTimedOrdinary(command, 100, 100));
        assert(tx.pump(149, sink));
        assert(tx.enqueue(make(Kind::Heartbeat, 2, 0)));
        sink.empty = true;
        assert(tx.pump(151, sink));
        sink.empty = true;
        sink.maxWrite = cut;
        assert(tx.pump(199, sink) == (cut != 0));
        assert(tx.enqueue(make(Kind::Stop, 3, 28)));
        assert(tx.pump(200, sink) == (cut != 0));
        assert(tx.ordinaryTimedOut());
        sink.maxWrite = kMaxFrame;
        drain(tx, sink);
        sent = frames(sink);
        assert(sent.size() == 3 && sent[0].kind == Kind::Command &&
               sent[0].offset == 0 && sent[1].kind == Kind::Heartbeat &&
               sent[2].kind == Kind::Stop);
        assembler.reset();
        for (const Frame& frame : sent) {
            if (frame.kind == Kind::Command)
                assert(assembler.accept(frame, 200, result) != AssemblyResult::Complete);
        }
        assert(!tx.ordinaryPending());
    }
}

void timedWraparoundAndDefaultCompatibility() {
    const uint32_t start = UINT32_MAX - 20;
    const Message command = make(Kind::Command, 1, 400);
    Transmitter tx;
    Sink sink;
    assert(tx.enqueueTimedOrdinary(command, start, 100));
    assert(tx.pump(uint32_t(start + 49), sink));
    sink.empty = true;
    assert(tx.pump(uint32_t(start + 99), sink));
    assert(tx.enqueue(make(Kind::Stop, 2, 28)));
    sink.empty = true;
    assert(tx.pump(uint32_t(start + 100), sink));
    assert(tx.ordinaryTimedOut() && !tx.pending());
    assert(frames(sink).size() == 3 && frames(sink).back().kind == Kind::Stop);

    tx.reset();
    sink = Sink{};
    sink.maxWrite = 1;
    assert(tx.enqueueTimedOrdinary(command, start, 100));
    assert(tx.pump(uint32_t(start + 49), sink));
    assert(tx.pump(uint32_t(start + 50), sink));
    assert(tx.ordinaryTimedOut());
    sink.maxWrite = kMaxFrame;
    drain(tx, sink);
    assert(frames(sink).empty());

    tx.reset();
    sink = Sink{};
    assert(tx.enqueueTimedOrdinary(command, 100, 100, 10));
    assert(!tx.pump(110, sink) && tx.ordinaryTimedOut() && !tx.pending());
    assert(tx.enqueue(command));
    assert(tx.pump(UINT32_MAX, sink)); // Timed overload leaves old enqueue unguarded.
    drain(tx, sink);
    assert(frames(sink).size() == 3 && !tx.ordinaryTimedOut());

    tx.reset();
    sink = Sink{};
    sink.maxWrite = kMaxFrame - 1;
    assert(tx.enqueueTimedOrdinary(command, 100, 100));
    assert(tx.pump(101, sink));
    assert(tx.enqueue(make(Kind::Stop, 2, 28)));
    tx.cancelOrdinary(); // Original cancellation still completes valid partial.
    sink.maxWrite = kMaxFrame;
    assert(tx.pump(102, sink));
    drain(tx, sink);
    const auto sent = frames(sink);
    assert(sent.size() == 2 && sent[0].kind == Kind::Command &&
           sent[1].kind == Kind::Stop && !tx.ordinaryTimedOut());

    // Explicit invalidation also works for legacy ordinary sends, never Stop.
    tx.reset();
    sink = Sink{};
    sink.maxWrite = 1;
    assert(tx.enqueue(make(Kind::Context, 1, 400)));
    assert(tx.enqueue(make(Kind::Stop, 2, 28)));
    assert(tx.pump(sink));
    tx.invalidateOrdinary();
    assert(!tx.ordinaryPending() && tx.pending());
    sink.maxWrite = kMaxFrame;
    drain(tx, sink);
    assert(frames(sink).size() == 1 && frames(sink)[0].kind == Kind::Stop);

    tx.reset();
    sink = Sink{};
    sink.maxWrite = kMaxFrame - 1;
    assert(tx.enqueue(make(Kind::Context, 1, 400)));
    assert(tx.pump(sink));
    tx.cancelOrdinary();
    tx.invalidateOrdinary(); // Also poison a partial already marked for cancel.
    assert(tx.enqueue(make(Kind::Stop, 2, 28)));
    sink.maxWrite = kMaxFrame;
    drain(tx, sink);
    assert(frames(sink).size() == 1 && frames(sink)[0].kind == Kind::Stop);
    assert(!tx.ordinaryTimedOut());
}

void partialControlSurvivesOrdinaryExpiry() {
    const Kind controls[] = {Kind::Stop, Kind::Heartbeat, Kind::LinkAck};
    for (Kind kind : controls) {
        Transmitter tx;
        Sink sink;
        sink.maxWrite = 1;
        assert(tx.enqueueTimedOrdinary(make(Kind::Command, 1, 400), 100, 500));
        const Message control = make(kind, 2, kind == Kind::Stop ? 28 :
                                              kind == Kind::LinkAck ? 2 : 0);
        assert(tx.enqueue(control));
        assert(tx.pump(149, sink));
        assert(tx.pump(150, sink));
        assert(tx.ordinaryTimedOut() && !tx.ordinaryPending() && tx.pending());
        sink.maxWrite = kMaxFrame;
        drain(tx, sink);
        assert(sink.bytes == encoded(control));
        const auto sent = frames(sink);
        assert(sent.size() == 1 && sent[0].kind == kind);
        assert(tx.ordinaryTimedOut());
    }
}

void maximumTimedMessage() {
    Transmitter tx;
    Sink sink;
    const Message command = make(Kind::Command, 1, kMaxMessage);
    assert(tx.enqueueTimedOrdinary(command, 100, 5000));
    assert(tx.pump(149, sink));
    for (size_t i = 0; i < 2000 && tx.pending(); ++i) {
        sink.empty = true;
        assert(tx.pump(5099, sink));
    }
    assert(!tx.pending() && !tx.ordinaryTimedOut());
    const auto sent = frames(sink);
    assert(sent.size() == (kMaxMessage + kMaxFragment - 1) / kMaxFragment);
    Assembler assembler;
    Message result;
    for (size_t i = 0; i < sent.size(); ++i) {
        const AssemblyResult outcome = assembler.accept(sent[i], 0, result);
        assert(outcome == (i + 1 == sent.size() ? AssemblyResult::Complete :
                                                 AssemblyResult::Incomplete));
    }
    assert(result.length == command.length &&
           std::memcmp(result.payload, command.payload, command.length) == 0);
}

void ownedQueryCancellation() {
    Transmitter tx;
    Sink sink;
    assert(tx.enqueue(make(Kind::ResultQuery, 10, 200)));
    tx.invalidateOrdinary(Kind::Command, 10);
    tx.invalidateOrdinary(Kind::ResultQuery, 9);
    assert(tx.ordinaryPending()); // Neither wrong kind nor stale owner cancels it.
    tx.invalidateOrdinary(Kind::ResultQuery, 10);
    assert(!tx.ordinaryPending() && !tx.pending() && sink.bytes.empty());
    assert(tx.enqueueTimedOrdinary(make(Kind::Command, 11, 1), 100, 5000));
    tx.invalidateOrdinary(Kind::ResultQuery, 10);
    assert(tx.ordinaryPending()); // Late cancellation cannot drop the replacement.
    assert(tx.pump(100, sink));
    assert(frames(sink).size() == 1 && frames(sink)[0].kind == Kind::Command);

    tx.reset(); sink = Sink{}; sink.room = 7;
    assert(tx.enqueue(make(Kind::ResultQuery, 20, 200)));
    assert(tx.pump(sink));
    assert(tx.enqueue(make(Kind::Stop, 21, 28)));
    assert(tx.enqueue(make(Kind::Heartbeat, 22, 0)));
    tx.invalidateOrdinary(Kind::ResultQuery, 20);
    tx.invalidateOrdinary(Kind::ResultQuery, 20); // Do not restore a partial CRC.
    assert(tx.ordinaryPending());
    assert(!tx.enqueue(make(Kind::Command, 23, 1))); // Residual frame must drain.
    drain(tx, sink);
    const auto sent = frames(sink);
    assert(sent.size() == 2 && sent[0].kind == Kind::Stop && sent[1].kind == Kind::Heartbeat);
    assert(tx.enqueue(make(Kind::Command, 23, 1)));
    drain(tx, sink);
    assert(frames(sink).back().kind == Kind::Command);
}
} // namespace

int main() {
    priorityAndBounds();
    partialAndCancellation();
    backpressureAndBadSink();
    shortWritesKeepFrameAtomic();
    timedValidationAndStickyOutcome();
    clocklessGuardAndControlPriority();
    expiredBeforeFirstWrite();
    partialInvalidationAtEveryByte();
    firstFrameBudgetAndOriginalTtl();
    timedWraparoundAndDefaultCompatibility();
    partialControlSurvivesOrdinaryExpiry();
    maximumTimedMessage();
    ownedQueryCancellation();
    std::puts("PASS v4 bounded transmit, timed ordinary, CRC invalidation and priority");
}
