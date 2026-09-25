#include "MotorBus.h"
#include "fake_x42s.h"
#include "x42s_can_id.h"

#include <cstdio>

using motion::CanQueryScheduler;
using motion::MotorBus;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        ++failures; \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    } \
} while (0)

bool isRead(size_t index, uint8_t id, uint8_t field) {
    if (index >= fakecan::capturedTX.size()) return false;
    const CanRawFrame& frame = fakecan::capturedTX[index];
    return frame.extended && !frame.remote &&
           x42sCanAddress(frame.identifier) == id &&
           frame.length == 2 && frame.data[0] == field && frame.data[1] == 0x6B;
}

void test_one_budget_and_priority_for_all_query_owners() {
    fakecan::fakeReset();
    MotorBus bus(4, 5, 500000);
    CHECK(bus.begin());
    CHECK(bus.demandQuery(1, 0x36, CanQueryScheduler::Page, 100, 1000, 0, 0));
    CHECK(bus.demandQuery(2, 0x35, CanQueryScheduler::Controller, 100, 1000, 3, 0));
    CHECK(bus.demandQuery(3, 0x3A, CanQueryScheduler::Await, 100, 1000, 2, 0));

    fakecan::setMillis(0);
    bus.dispatchQueries(0, true, nullptr, nullptr);
    CHECK(fakecan::capturedTX.size() == 1 && isRead(0, 2, 0x35));
    CHECK(bus.queryStatistics().queries == 1);
    bus.releaseQueries(CanQueryScheduler::Controller);

    // A real parsed RX frees a slot, but does not refill the global TX budget.
    fakecan::injectRx(fakecan::makeVelocity(2, 0));
    CanRawFrame reply;
    CHECK(bus.receive(reply, 0));
    CHECK(reply.data[0] == 0x35);
    bus.receiveQuery(2, 0x35, 20);
    CHECK(bus.queryStatistics().responses == 1);
    CHECK(bus.queryEvidence(2, 0x35).received);

    fakecan::setMillis(99);
    bus.dispatchQueries(99, true, nullptr, nullptr);
    CHECK(fakecan::capturedTX.size() == 1);
    fakecan::setMillis(100);
    bus.dispatchQueries(100, true, nullptr, nullptr);
    CHECK(fakecan::capturedTX.size() == 2 && isRead(1, 3, 0x3A));
    bus.releaseQueries(CanQueryScheduler::Await);
    fakecan::setMillis(199);
    bus.dispatchQueries(199, true, nullptr, nullptr);
    CHECK(fakecan::capturedTX.size() == 2);
    fakecan::setMillis(200);
    bus.dispatchQueries(200, true, nullptr, nullptr);
    CHECK(fakecan::capturedTX.size() == 3 && isRead(2, 1, 0x36));
    CHECK(bus.queryStatistics().queries == 3);
    CHECK(bus.queryInflight() == 2);
}

void test_raw_tx_defers_optional_query() {
    fakecan::fakeReset();
    MotorBus bus(4, 5, 500000);
    CHECK(bus.begin());
    CHECK(bus.demandQuery(1, 0x36, CanQueryScheduler::Page, 100, 1000, 0, 0));

    const uint8_t payload[] = {0xAA};
    fakecan::setMillis(0);
    CHECK(bus.sendRawFrame(0x123, false, payload, sizeof(payload)));
    CHECK(fakecan::capturedTX.size() == 1);
    bus.dispatchQueries(0, true, nullptr, nullptr);
    fakecan::setMillis(99);
    bus.dispatchQueries(99, true, nullptr, nullptr);
    CHECK(fakecan::capturedTX.size() == 1);
    fakecan::setMillis(100);
    bus.dispatchQueries(100, true, nullptr, nullptr);
    CHECK(fakecan::capturedTX.size() == 2 && isRead(1, 1, 0x36));
    CHECK(bus.queryStatistics().queries == 1);
}

void test_sync_exclusive_uses_the_same_budget() {
    fakecan::fakeReset();
    MotorBus bus(4, 5, 500000);
    CHECK(bus.begin());
    CHECK(bus.demandQuery(1, 0x36, CanQueryScheduler::Page, 100, 1000, 0, 0));
    CHECK(bus.demandQuery(2, 0x36, CanQueryScheduler::Sync, 600, 1000, 3, 0));
    bus.exclusiveSyncQueries(true);

    fakecan::setMillis(0);
    bus.dispatchQueries(0, true, nullptr, nullptr);
    CHECK(fakecan::capturedTX.size() == 1 && isRead(0, 2, 0x36));
    fakecan::setMillis(100);
    bus.dispatchQueries(100, true, nullptr, nullptr);
    CHECK(fakecan::capturedTX.size() == 1);

    fakecan::injectRx(fakecan::makePosition(2, 0));
    CanRawFrame reply;
    CHECK(bus.receive(reply, 0));
    bus.receiveQuery(2, 0x36, 120);
    bus.releaseQueries(CanQueryScheduler::Sync);
    bus.exclusiveSyncQueries(false);
    fakecan::setMillis(150);
    bus.dispatchQueries(150, true, nullptr, nullptr);
    CHECK(fakecan::capturedTX.size() == 2 && isRead(1, 1, 0x36));
    CHECK(bus.queryStatistics().queries == 2);
}

void test_stop_tx_precedes_and_defers_optional_query() {
    fakecan::fakeReset();
    MotorBus bus(4, 5, 500000);
    CHECK(bus.begin());
    CHECK(bus.demandQuery(1, 0x36, CanQueryScheduler::Page, 100, 1000, 0, 0));

    fakecan::setMillis(0);
    bus.stopNow(2, false);
    CHECK(fakecan::capturedTX.size() == 1);
    CHECK(fakecan::capturedTX[0].data[0] == 0xFE);
    bus.dispatchQueries(0, true, nullptr, nullptr);
    fakecan::setMillis(99);
    bus.dispatchQueries(99, true, nullptr, nullptr);
    CHECK(fakecan::capturedTX.size() == 1);
    fakecan::setMillis(100);
    bus.dispatchQueries(100, true, nullptr, nullptr);
    CHECK(fakecan::capturedTX.size() == 2 && isRead(1, 1, 0x36));
    CHECK(bus.queryStatistics().queries == 1);
}

void test_observability_reads_are_passive() {
    fakecan::fakeReset();
    MotorBus bus(4, 5, 500000);
    CHECK(bus.begin());
    for (int i = 0; i < 100; ++i) {
        (void)bus.queryBudget();
        (void)bus.queryStatistics();
        (void)bus.queryInflight();
        (void)bus.queryEvidence(1, 0x36);
    }
    CHECK(bus.queryBudget().queriesPerSecond == 10);
    CHECK(bus.queryStatistics().queries == 0);
    CHECK(bus.queryInflight() == 0);
    CHECK(!bus.queryEvidence(1, 0x36).pending);
    CHECK(fakecan::capturedTX.empty());
    fakecan::setMillis(100);
    bus.dispatchQueries(100, true, nullptr, nullptr);
    CHECK(fakecan::capturedTX.empty());
}

}  // namespace

int main() {
    test_one_budget_and_priority_for_all_query_owners();
    test_raw_tx_defers_optional_query();
    test_sync_exclusive_uses_the_same_budget();
    test_stop_tx_precedes_and_defers_optional_query();
    test_observability_reads_are_passive();
    if (failures) std::printf("motor-bus-queries: %d/%d checks failed\n", failures, checks);
    else std::printf("motor-bus-queries: %d checks passed\n", checks);
    return failures ? 1 : 0;
}
