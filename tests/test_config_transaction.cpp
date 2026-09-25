#include "ConfigTransaction.h"

#include <cassert>
#include <cstdio>
#include <cstring>

using motion::ConfigTransaction;
using FrameResult = ConfigTransaction::FrameResult;

static const uint8_t config[] = {
    1, 0x4C, 0xAE, 1, 2, 0, 0, 100, 0, 0, 0x27, 0x10, 0, 5, 0,
    120, 0, 60, 0, 0x6B
};

static CanRawFrame frame(uint8_t id, uint8_t packet,
                         const uint8_t* payload, uint8_t size) {
    CanRawFrame result{};
    result.identifier = (uint32_t(id) << 8) | packet;
    result.extended = true;
    result.length = size;
    std::memcpy(result.data, payload, size);
    return result;
}

static CanRawFrame ack(uint8_t id, uint8_t status) {
    const uint8_t bytes[] = {0x4C, status, 0x6B};
    return frame(id, 0, bytes, sizeof(bytes));
}

static CanRawFrame part(uint8_t packet, bool mismatch = false) {
    uint8_t bytes[8] = {0x22};
    const uint8_t count = packet < 2 ? 7 : 2;
    for (uint8_t i = 0; i < count; ++i) {
        const uint8_t offset = packet * 7 + i;
        bytes[i + 1] = offset == 15 ? 0x6B : config[4 + offset];
    }
    if (mismatch && packet == 0) bytes[1] ^= 1;
    return frame(1, packet, bytes, count + 1);
}

static void beginRead(ConfigTransaction& transaction, uint32_t at = 10) {
    transaction.start(config, at);
    assert(transaction.pending());
    assert(std::strcmp(transaction.message(), "config_wait_ack") == 0);
    assert(transaction.acceptFrame(ack(1, 2), at + 1) == FrameResult::Consumed);
    assert(transaction.wantsReadQuery());
}

int main() {
    // An unissued query cannot accept a buffered reply, even after ACK.
    {
        ConfigTransaction tx;
        beginRead(tx);
        assert(tx.acceptFrame(part(0), 11) == FrameResult::Consumed);
        assert(tx.acceptFrame(part(1), 11) == FrameResult::Consumed);
        assert(tx.acceptFrame(part(2), 11) == FrameResult::Consumed);
        assert(tx.received() == 0);
        assert(!tx.readQuerySent(2, 0x22, true, 20));
        assert(!tx.readQuerySent(1, 0x36, true, 20));
        assert(tx.wantsReadQuery());
        assert(tx.readQuerySent(1, 0x22, true, 20));
        assert(!tx.wantsReadQuery());
        assert(tx.acceptFrame(part(0), 21) == FrameResult::Consumed);
        assert(tx.received() == 7);
        assert(tx.acceptFrame(part(1), 22) == FrameResult::Consumed);
        assert(tx.acceptFrame(part(2), 23) == FrameResult::ReadbackComplete);
        assert(std::strcmp(tx.message(), "config_verified") == 0);
        assert(!tx.pending() && !tx.failed());
        assert(tx.received() == 16);
        assert(std::memcmp(tx.actual(), config + 4, 15) == 0);
        assert(tx.acceptFrame(part(0), 24) == FrameResult::Ignored);
        assert(tx.acceptFrame(part(1), 24) != FrameResult::ReadbackComplete);
    }
    // Reordered, duplicated, truncated, wrong-node and remote frames cannot
    // advance the assembly. Valid frames after them can still complete it.
    {
        ConfigTransaction tx;
        beginRead(tx);
        tx.readQuerySent(1, 0x22, true, 20);
        auto remote = part(0); remote.remote = true;
        auto wrongNode = part(0); wrongNode.identifier = uint32_t(2) << 8;
        auto shortPart = part(1); shortPart.length = 7;
        assert(tx.acceptFrame(remote, 21) == FrameResult::Ignored);
        assert(tx.acceptFrame(wrongNode, 21) == FrameResult::Ignored);
        assert(tx.acceptFrame(part(1), 21) == FrameResult::Consumed);
        assert(tx.received() == 0);
        assert(tx.acceptFrame(part(0), 22) == FrameResult::Consumed);
        assert(tx.acceptFrame(part(0), 23) == FrameResult::Consumed);
        assert(tx.acceptFrame(shortPart, 24) == FrameResult::Consumed);
        assert(tx.acceptFrame(part(2), 25) == FrameResult::Consumed);
        assert(tx.received() == 7);
        assert(tx.acceptFrame(part(1), 26) == FrameResult::Consumed);
        assert(tx.acceptFrame(part(2), 27) == FrameResult::ReadbackComplete);
        assert(std::strcmp(tx.message(), "config_verified") == 0);
    }
    // Bad terminator restarts assembly; a value mismatch is terminal evidence.
    {
        ConfigTransaction tx;
        beginRead(tx);
        tx.readQuerySent(1, 0x22, true, 20);
        auto badEnd = part(2); badEnd.data[2] = 0;
        tx.acceptFrame(part(0), 21); tx.acceptFrame(part(1), 22);
        assert(tx.acceptFrame(badEnd, 23) == FrameResult::Consumed);
        assert(tx.received() == 0 && tx.pending());
        tx.acceptFrame(part(0, true), 24); tx.acceptFrame(part(1), 25);
        assert(tx.acceptFrame(part(2), 26) == FrameResult::ReadbackComplete);
        assert(tx.failed());
        assert(std::strcmp(tx.message(), "config_mismatch") == 0);
    }
    // The exact 3-second boundary remains valid; expiry happens after it.
    {
        ConfigTransaction tx;
        tx.start(config, 10);
        tx.poll(3010); assert(tx.pending());
        tx.poll(3011);
        assert(std::strcmp(tx.message(), "config_ack_timeout") == 0);
        assert(tx.acceptFrame(ack(1, 2), 3012) == FrameResult::Ignored);
        assert(!tx.wantsReadQuery());
    }
    {
        ConfigTransaction tx;
        beginRead(tx);
        tx.readQuerySent(1, 0x22, true, 20);
        tx.poll(3020); assert(tx.pending());
        tx.poll(3021);
        assert(std::strcmp(tx.message(), "config_readback_timeout") == 0);
        assert(tx.acceptFrame(part(0), 3022) == FrameResult::Ignored);
    }
    // The deadline also applies when congestion prevented the query itself.
    {
        ConfigTransaction tx;
        beginRead(tx);
        tx.poll(3011); assert(tx.pending());
        tx.poll(3012);
        assert(std::strcmp(tx.message(), "config_readback_timeout") == 0);
        assert(!tx.wantsReadQuery());
        assert(!tx.readQuerySent(1, 0x22, true, 3013));
    }
    // Millisecond wrap uses the same elapsed-time rule.
    {
        ConfigTransaction tx;
        const uint32_t start = 0xFFFFFF00u;
        tx.start(config, start);
        tx.poll(start + 3000u); assert(tx.pending());
        tx.poll(start + 3001u);
        assert(std::strcmp(tx.message(), "config_ack_timeout") == 0);
    }
    // Query transmission failure and cancellation are terminal and sticky.
    {
        ConfigTransaction tx;
        beginRead(tx);
        assert(tx.readQuerySent(1, 0x22, false, 20));
        assert(tx.failed());
        assert(std::strcmp(tx.message(), "config_read_tx_failed") == 0);
        tx.poll(5000);
        assert(tx.acceptFrame(part(0), 5001) == FrameResult::Ignored);
        assert(std::strcmp(tx.message(), "config_read_tx_failed") == 0);
    }
    {
        ConfigTransaction tx;
        tx.start(config, 10);
        tx.cancel();
        assert(std::strcmp(tx.message(), "config_cancelled") == 0);
        assert(tx.acceptFrame(ack(1, 2), 11) == FrameResult::Ignored);
        assert(!tx.pending());
        tx.start(config, 20);
        assert(tx.sequence() == 2);
        assert(tx.acceptFrame(ack(1, 0xE2), 21) == FrameResult::Consumed);
        assert(std::strcmp(tx.message(), "config_rejected") == 0);
        tx.cancel();
        assert(std::strcmp(tx.message(), "config_rejected") == 0);
    }
    {
        ConfigTransaction tx;
        beginRead(tx);
        tx.cancel();
        assert(std::strcmp(tx.message(), "config_cancelled") == 0);
        assert(!tx.wantsReadQuery());
        assert(!tx.readQuerySent(1, 0x22, true, 12));
        assert(tx.acceptFrame(part(0), 13) == FrameResult::Ignored);
    }
    puts("PASS 4C transaction state contract");
}
