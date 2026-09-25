// Contract tests for the hardware-independent program execution state machine.
// The CAN-facing CommandQueue tests separately assert the transmitted frames.

#include "ProgramRunner.h"

#include <cstdio>
#include <cstring>

using namespace motion;

static int checks = 0;
static int failures = 0;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        ++failures; \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    } \
} while (0)

static QueueProgram plan(QueueAction first, QueueAction second = QueueAction::None) {
    QueueProgram result{};
    result.count = second == QueueAction::None ? 1 : 2;
    result.steps[0].action = first;
    result.steps[0].line = 7;
    result.steps[0].id = 1;
    if (result.count == 2) {
        result.steps[1].action = second;
        result.steps[1].line = 11;
        result.steps[1].id = 2;
    }
    result.hasRaw = first == QueueAction::Hex || first == QueueAction::Can ||
                    second == QueueAction::Hex || second == QueueAction::Can;
    return result;
}

static void test_one_dispatch_per_tick_and_repeat() {
    ProgramRunner runner;
    const QueueProgram program = plan(QueueAction::Enable, QueueAction::Stop);
    runner.begin(program, 123, 2, false, 100);
    CHECK(runner.state() == QueueState::Running);
    CHECK(runner.runId() == 1);
    CHECK(runner.programHash() == 123);
    CHECK(runner.tick(100) == ProgramRunner::Action::Dispatch);
    CHECK(runner.currentStep() && runner.currentStep()->action == QueueAction::Enable);
    runner.advance(100);  // CommandQueue sent exactly one first action.
    CHECK(runner.tick(100) == ProgramRunner::Action::Dispatch);
    CHECK(runner.currentStep() && runner.currentStep()->action == QueueAction::Stop);
    runner.advance(100);
    // Crossing the iteration boundary consumes its own poll. It cannot send
    // the next iteration's first action in the same loop pass.
    CHECK(runner.tick(101) == ProgramRunner::Action::None);
    CHECK(runner.iteration() == 1);
    CHECK(runner.tick(101) == ProgramRunner::Action::Dispatch);
    CHECK(runner.currentStep() && runner.currentStep()->action == QueueAction::Enable);
    runner.advance(101);
    CHECK(runner.tick(102) == ProgramRunner::Action::Dispatch);
    runner.advance(102);
    CHECK(runner.tick(103) == ProgramRunner::Action::None);
    CHECK(runner.state() == QueueState::Done);
    CHECK(runner.runId() == 1);  // Repetition is still one accepted run.
}

static void test_wait_and_timed_stop_across_clock_wrap() {
    ProgramRunner waitRunner;
    const QueueProgram waitProgram = plan(QueueAction::Wait, QueueAction::Stop);
    const uint32_t start = 0xfffffff0u;
    waitRunner.begin(waitProgram, 1, 1, false, start);
    CHECK(waitRunner.tick(start) == ProgramRunner::Action::Dispatch);
    waitRunner.waitStep(start, 32);  // Deadline wraps to 0x10.
    CHECK(waitRunner.tick(0xffffffffu) == ProgramRunner::Action::None);
    CHECK(waitRunner.currentStep() && waitRunner.currentStep()->action == QueueAction::Wait);
    CHECK(waitRunner.tick(0x0000000fu) == ProgramRunner::Action::None);
    CHECK(waitRunner.tick(0x00000010u) == ProgramRunner::Action::None);
    CHECK(waitRunner.tick(0x00000010u) == ProgramRunner::Action::Dispatch);
    CHECK(waitRunner.currentStep() && waitRunner.currentStep()->action == QueueAction::Stop);

    ProgramRunner timedRunner;
    const QueueProgram timedProgram = plan(QueueAction::Velocity, QueueAction::Stop);
    timedRunner.begin(timedProgram, 2, 1, false, start);
    CHECK(timedRunner.tick(start) == ProgramRunner::Action::Dispatch);
    timedRunner.timedStep(start, 32);
    CHECK(timedRunner.tick(0x0000000fu) == ProgramRunner::Action::None);
    CHECK(timedRunner.tick(0x00000010u) == ProgramRunner::Action::TimedStop);
    // The timer asks the adapter to send FE. It must not claim that the FE was
    // sent, or advance to the next action, before that callback succeeds.
    CHECK(timedRunner.currentStep() && timedRunner.currentStep()->action == QueueAction::Velocity);
    timedRunner.advance(0x00000010u);
    CHECK(timedRunner.tick(0x00000010u) == ProgramRunner::Action::Dispatch);
    CHECK(timedRunner.currentStep() && timedRunner.currentStep()->action == QueueAction::Stop);
}

static void test_raw_spacing_across_clock_wrap() {
    ProgramRunner runner;
    const QueueProgram program = plan(QueueAction::Hex, QueueAction::Can);
    runner.begin(program, 3, 1, false, 0xffffffffu);
    CHECK(runner.tick(0xffffffffu) == ProgramRunner::Action::Dispatch);
    runner.rawSent(0xffffffffu);
    runner.advance(0xffffffffu);
    CHECK(runner.tick(0x00000000u) == ProgramRunner::Action::None);
    CHECK(runner.tick(0x00000001u) == ProgramRunner::Action::Dispatch);
    CHECK(runner.currentStep() && runner.currentStep()->action == QueueAction::Can);
}

static void test_first_error_cancel_and_late_sync_callback() {
    ProgramRunner runner;
    QueueProgram program = plan(QueueAction::SyncBegin, QueueAction::Stop);
    program.steps[0].groupSize = 1;
    runner.begin(program, 4, 1, false, 0);
    CHECK(runner.tick(0) == ProgramRunner::Action::Dispatch);
    runner.syncStep();
    CHECK(runner.tick(1) == ProgramRunner::Action::PollSync);
    runner.fail("first_failure", 7);
    runner.fail("second_failure", 11);
    CHECK(runner.state() == QueueState::Failed);
    CHECK(runner.errorLine() == 7);
    CHECK(std::strcmp(runner.message(), "first_failure") == 0);
    runner.completeSync(1, 2);  // A late callback cannot revive a failed run.
    CHECK(runner.state() == QueueState::Failed);
    CHECK(runner.tick(2) == ProgramRunner::Action::None);

    runner.begin(program, 5, 1, false, 10);
    CHECK(runner.runId() == 2);
    CHECK(runner.tick(10) == ProgramRunner::Action::Dispatch);
    runner.syncStep();
    runner.cancel("operator_cancel");
    runner.completeSync(1, 11);  // CAN RX may race with an operator stop.
    CHECK(runner.state() == QueueState::Cancelled);
    CHECK(std::strcmp(runner.message(), "operator_cancel") == 0);
    CHECK(runner.tick(11) == ProgramRunner::Action::None);
}

static void test_sync_completion_skips_group_without_extra_dispatch() {
    ProgramRunner runner;
    QueueProgram program{};
    program.count = 4;
    program.steps[0].action = QueueAction::SyncBegin;
    program.steps[0].groupSize = 1;
    program.steps[1].action = QueueAction::Move;
    program.steps[2].action = QueueAction::SyncEnd;
    program.steps[3].action = QueueAction::Stop;
    runner.begin(program, 6, 1, false, 10);
    CHECK(runner.tick(10) == ProgramRunner::Action::Dispatch);
    runner.syncStep();
    CHECK(runner.tick(11) == ProgramRunner::Action::PollSync);
    runner.completeSync(1, 12);
    CHECK(runner.state() == QueueState::Running);
    CHECK(runner.currentStep() && runner.currentStep()->action == QueueAction::Stop);
    CHECK(runner.tick(12) == ProgramRunner::Action::Dispatch);
    runner.advance(12);
    CHECK(runner.tick(13) == ProgramRunner::Action::None);
    CHECK(runner.state() == QueueState::Done);
    CHECK(std::strcmp(runner.message(), "sync_motion_complete") == 0);
}

static void test_rejected_validation_keeps_run_identity() {
    ProgramRunner runner;
    runner.recordValidationFailure(23, "unknown_action");
    CHECK(runner.state() == QueueState::Idle);
    CHECK(runner.runId() == 0);
    CHECK(runner.errorLine() == 23);
    CHECK(std::strcmp(runner.message(), "unknown_action") == 0);
    const QueueProgram program = plan(QueueAction::Enable);
    runner.begin(program, 9, 1, false, 100);
    CHECK(runner.runId() == 1);
    CHECK(runner.errorLine() == 0);
    CHECK(runner.tick(100) == ProgramRunner::Action::Dispatch);
}

int main() {
    test_one_dispatch_per_tick_and_repeat();
    test_wait_and_timed_stop_across_clock_wrap();
    test_raw_spacing_across_clock_wrap();
    test_first_error_cancel_and_late_sync_callback();
    test_sync_completion_skips_group_without_extra_dispatch();
    test_rejected_validation_keeps_run_identity();
    if (failures) {
        std::printf("program-runner: %d/%d checks failed\n", failures, checks);
        return 1;
    }
    std::printf("program-runner: %d checks passed\n", checks);
    return 0;
}
