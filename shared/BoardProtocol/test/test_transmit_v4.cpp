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
} // namespace

int main() {
    priorityAndBounds();
    partialAndCancellation();
    backpressureAndBadSink();
    shortWritesKeepFrameAtomic();
    std::puts("PASS v4 bounded transmit, priority, partial write and cancellation");
}
