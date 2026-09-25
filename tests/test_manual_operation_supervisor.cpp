// Public state-transition contract for manual Move and Home supervision.
// The real CAN adapter remains covered by test_motor_control.cpp.
#include "ManualOperationSupervisor.h"

#include <cstdio>

using motion::ManualOperationSupervisor;
using Supervisor = ManualOperationSupervisor;

static int checks = 0;
static int failures = 0;

#define CHECK(condition) do {                                             \
    ++checks;                                                              \
    if (!(condition)) {                                                    \
        ++failures;                                                        \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    }                                                                      \
} while (0)

static Supervisor::MoveStart moveAt(uint32_t start, uint8_t opcode = 0xFD,
                                    bool targetProof = false) {
    Supervisor::MoveStart move;
    move.id = 1;
    move.opcode = opcode;
    move.startTenths = 0;
    move.targetTenths = 900;
    move.toleranceTenths = 5;
    move.expectedDurationMs = 1000;
    move.startMs = start;
    move.deadlineMs = 3000;
    move.targetProof = targetProof;
    return move;
}

static Supervisor::Observation feedback(uint32_t now, int32_t position = 900,
                                        int32_t velocity = 0) {
    Supervisor::Observation observation;
    observation.now = now;
    observation.enabled = true;
    observation.positionFresh = true;
    observation.velocityFresh = true;
    observation.position = position;
    observation.velocity = velocity;
    observation.positionMs = now;
    observation.velocityMs = now;
    return observation;
}

static void checkPending(const Supervisor::Verdict& verdict) {
    CHECK(verdict.kind == Supervisor::Kind::None);
    CHECK(verdict.fault == Supervisor::Fault::None);
    CHECK(!verdict.done);
}

static void moveNeedsMatchingAckAndTwoIndependentPairs() {
    Supervisor supervisor;
    CHECK(supervisor.startMove(moveAt(100)));
    CHECK(supervisor.kind() == Supervisor::Kind::Move);
    supervisor.acceptedAck(2, 0xFD, true, 101); // wrong node
    supervisor.acceptedAck(1, 0xFB, true, 101); // wrong opcode
    checkPending(supervisor.poll(feedback(110)));
    supervisor.acceptedAck(1, 0xFD, true, 111); // receipt is not completion

    auto first = feedback(120);
    checkPending(supervisor.poll(first));
    checkPending(supervisor.poll(first)); // replayed feedback cannot count twice
    auto oneFieldOld = feedback(130);
    oneFieldOld.velocityMs = first.velocityMs;
    checkPending(supervisor.poll(oneFieldOld));
    CHECK(supervisor.active());
    auto second = feedback(140);
    auto verdict = supervisor.poll(second);
    CHECK(verdict.kind == Supervisor::Kind::Move);
    CHECK(verdict.fault == Supervisor::Fault::None);
    CHECK(verdict.done);
    CHECK(!supervisor.active());
    CHECK(supervisor.moveOutcome() == Supervisor::MoveOutcome::Done);
    checkPending(supervisor.poll(second)); // terminal verdict is emitted once
}

static void directMovesNeedTheirOwnTargetReadback() {
    const uint8_t opcodes[] = {0xFB, 0xCB};
    for (uint8_t opcode : opcodes) {
        Supervisor supervisor;
        CHECK(supervisor.startMove(moveAt(100, opcode, true)));
        supervisor.acceptedAck(1, opcode, true, 101);
        checkPending(supervisor.poll(feedback(110))); // no 0x33 target
        auto staleTarget = feedback(120);
        staleTarget.targetValid = true;
        staleTarget.target = 900;
        staleTarget.targetMs = 100; // prior to command
        checkPending(supervisor.poll(staleTarget));
        auto wrongTarget = feedback(130);
        wrongTarget.targetValid = true;
        wrongTarget.target = 1200;
        wrongTarget.targetMs = 125;
        checkPending(supervisor.poll(wrongTarget));
        auto first = feedback(140);
        first.targetValid = true;
        first.target = 900;
        first.targetMs = 135;
        checkPending(supervisor.poll(first));
        checkPending(supervisor.poll(first));
        auto second = feedback(150);
        second.targetValid = true;
        second.target = 900;
        second.targetMs = 135;
        auto verdict = supervisor.poll(second);
        CHECK(verdict.kind == Supervisor::Kind::Move);
        CHECK(verdict.done);
        CHECK(supervisor.moveOutcome() == Supervisor::MoveOutcome::Done);
    }
}

static void homeNeedsRunningThenStoppedAndTwoPairs() {
    Supervisor supervisor;
    CHECK(supervisor.startHome(1, 2, 100, 3000));
    supervisor.acceptedAck(2, 0x9A, false, 101);
    supervisor.acceptedAck(1, 0x9C, false, 101);
    supervisor.acceptedAck(1, 0x9A, false, 102);

    auto idle = feedback(110);
    idle.homeFlagsFresh = true;
    idle.homeFlags = 0;
    idle.homeFlagsMs = 110;
    checkPending(supervisor.poll(idle)); // one idle 0x3B is not proof
    auto running = feedback(120);
    running.homeFlagsFresh = true;
    running.homeFlags = 0x04;
    running.homeFlagsMs = 120;
    checkPending(supervisor.poll(running));
    auto stopped = feedback(130);
    stopped.homeFlagsFresh = true;
    stopped.homeFlags = 0;
    stopped.homeFlagsMs = 130;
    stopped.positionMs = 129;
    stopped.velocityMs = 129;
    checkPending(supervisor.poll(stopped)); // feedback predates stop evidence
    auto first = feedback(140);
    first.homeFlagsFresh = true;
    first.homeFlags = 0;
    first.homeFlagsMs = 130;
    checkPending(supervisor.poll(first));
    checkPending(supervisor.poll(first));
    auto second = feedback(150);
    second.homeFlagsFresh = true;
    second.homeFlags = 0;
    second.homeFlagsMs = 130;
    auto verdict = supervisor.poll(second);
    CHECK(verdict.kind == Supervisor::Kind::Home);
    CHECK(verdict.done);
    CHECK(supervisor.homeOutcome() == Supervisor::HomeOutcome::Done);
    CHECK(supervisor.homeId() == 1);
    CHECK(supervisor.homeMode() == 2);
    checkPending(supervisor.poll(second));
}

static void completedAckStillNeedsLaterStationaryFeedback() {
    Supervisor supervisor;
    CHECK(supervisor.startHome(1, 1, 100, 3000));
    supervisor.acceptedAck(1, 0x9A, true, 120); // 9F completion, no 0x3B
    auto old = feedback(121);
    old.positionMs = 119;
    old.velocityMs = 119;
    checkPending(supervisor.poll(old));
    auto moving = feedback(130, 300, 10);
    checkPending(supervisor.poll(moving));
    auto stopped = feedback(140, 300, 0);
    auto verdict = supervisor.poll(stopped);
    CHECK(verdict.kind == Supervisor::Kind::Home);
    CHECK(verdict.done);
    CHECK(supervisor.homeOutcome() == Supervisor::HomeOutcome::Done);
}

static void noMotionAndHomeFaultsRemainDistinct() {
    {
        Supervisor supervisor;
        CHECK(supervisor.startHome(1, 0, 100, 3000));
        supervisor.noMotion(2);
        CHECK(supervisor.homeActive());
        supervisor.noMotion(1); // decoded 12/22 response
        CHECK(!supervisor.active());
        CHECK(supervisor.homeOutcome() == Supervisor::HomeOutcome::NoMotion);
        checkPending(supervisor.poll(feedback(110)));
    }
    struct Case { uint8_t flags; Supervisor::Fault expected; };
    const Case cases[] = {
        {0x08, Supervisor::Fault::HomeFailed},
        {0x10, Supervisor::Fault::HomeProtection},
        {0x20, Supervisor::Fault::HomeProtection},
    };
    for (const auto& test : cases) {
        Supervisor supervisor;
        CHECK(supervisor.startHome(1, 0, 100, 3000));
        supervisor.acceptedAck(1, 0x9A, false, 101);
        auto flags = feedback(110);
        flags.homeFlagsFresh = true;
        flags.homeFlags = test.flags;
        flags.homeFlagsMs = 110;
        auto verdict = supervisor.poll(flags);
        CHECK(verdict.kind == Supervisor::Kind::Home);
        CHECK(verdict.fault == test.expected);
        CHECK(!verdict.done);
        CHECK(supervisor.homeOutcome() == Supervisor::HomeOutcome::Failed);
        checkPending(supervisor.poll(flags));
    }
}

static void deadlinesAndFailureSnapshotAreOwnedBySupervisor() {
    {
        Supervisor supervisor;
        CHECK(supervisor.startMove(moveAt(100)));
        checkPending(supervisor.poll(feedback(1600, 0))); // ACK window inclusive
        auto verdict = supervisor.poll(feedback(1601, 0));
        CHECK(verdict.kind == Supervisor::Kind::Move);
        CHECK(verdict.fault == Supervisor::Fault::MoveAckTimeout);
        CHECK(supervisor.moveOutcome() == Supervisor::MoveOutcome::Failed);
        CHECK(supervisor.moveFailure().id == 1);
        CHECK(supervisor.moveFailure().target == 900);
        CHECK(supervisor.moveFailure().elapsed == 1501);
        CHECK(supervisor.moveFailure().deadline == 3000);
        checkPending(supervisor.poll(feedback(1602, 0)));
    }
    {
        Supervisor supervisor;
        CHECK(supervisor.startMove(moveAt(100)));
        supervisor.acceptedAck(1, 0xFD, false, 101);
        checkPending(supervisor.poll(feedback(3100, 0)));
        auto verdict = supervisor.poll(feedback(3101, 0));
        CHECK(verdict.fault == Supervisor::Fault::MoveTimeout);
        CHECK(supervisor.moveFailure().positionValid);
        CHECK(supervisor.moveFailure().velocityValid);
        CHECK(supervisor.moveFailure().position == 0);
        CHECK(supervisor.moveFailure().enabled);
    }
    {
        Supervisor supervisor;
        CHECK(supervisor.startHome(1, 0, 100, 3000));
        supervisor.acceptedAck(1, 0x9A, false, 101);
        checkPending(supervisor.poll(feedback(700, 0)));
        auto verdict = supervisor.poll(feedback(701, 0));
        CHECK(verdict.fault == Supervisor::Fault::HomeStatusMissing);
        CHECK(supervisor.homeOutcome() == Supervisor::HomeOutcome::Failed);
    }
    {
        Supervisor supervisor;
        CHECK(supervisor.startHome(1, 0, 100, 3000));
        checkPending(supervisor.poll(feedback(1600, 0)));
        auto verdict = supervisor.poll(feedback(1601, 0));
        CHECK(verdict.kind == Supervisor::Kind::Home);
        CHECK(verdict.fault == Supervisor::Fault::HomeAckTimeout);
        CHECK(supervisor.homeOutcome() == Supervisor::HomeOutcome::Failed);
    }
    {
        Supervisor supervisor;
        CHECK(supervisor.startHome(1, 0, 100, 3000));
        supervisor.acceptedAck(1, 0x9A, false, 101);
        auto stillRunning = feedback(3100, 0);
        stillRunning.homeFlagsFresh = true;
        stillRunning.homeFlags = 0x04;
        stillRunning.homeFlagsMs = 3099;
        checkPending(supervisor.poll(stillRunning));
        stillRunning.now = 3101;
        auto verdict = supervisor.poll(stillRunning);
        CHECK(verdict.kind == Supervisor::Kind::Home);
        CHECK(verdict.fault == Supervisor::Fault::HomeTimeout);
        CHECK(supervisor.homeOutcome() == Supervisor::HomeOutcome::Failed);
    }
    {
        Supervisor supervisor;
        CHECK(supervisor.startMove(moveAt(100)));
        supervisor.acceptedAck(1, 0xFD, false, 101);
        auto stale = feedback(701, 0);
        stale.positionFresh = false;
        CHECK(supervisor.poll(stale).fault == Supervisor::Fault::FeedbackStale);
        CHECK(supervisor.moveOutcome() == Supervisor::MoveOutcome::Failed);
    }
}

static void cancellationAndExclusivityDoNotLoseOwnership() {
    Supervisor supervisor;
    CHECK(supervisor.startMove(moveAt(100)));
    CHECK(!supervisor.startHome(2, 0, 110, 3000));
    CHECK(!supervisor.startMove(moveAt(110)));
    CHECK(supervisor.moveActive());
    CHECK(supervisor.activeId() == 1);
    supervisor.cancelMove(2);
    CHECK(supervisor.moveActive());
    supervisor.cancelMove(1);
    CHECK(!supervisor.active());
    CHECK(supervisor.moveOutcome() == Supervisor::MoveOutcome::Cancelled);
    supervisor.acceptedAck(1, 0xFD, true, 111); // late ACK after cancel
    CHECK(supervisor.moveOutcome() == Supervisor::MoveOutcome::Cancelled);
    CHECK(supervisor.startHome(2, 1, 120, 3000));
    CHECK(!supervisor.startMove(moveAt(130)));
    CHECK(supervisor.homeActive());
    CHECK(supervisor.activeId() == 2);
    supervisor.cancelHome(1);
    CHECK(supervisor.homeActive());
    supervisor.cancelHome(2);
    CHECK(supervisor.homeOutcome() == Supervisor::HomeOutcome::Cancelled);
    CHECK(!supervisor.active());
    CHECK(supervisor.startMove(moveAt(140)));
    supervisor.rawInvalidation(2);
    CHECK(supervisor.moveActive());
    supervisor.rawInvalidation(1);
    CHECK(!supervisor.active());
    CHECK(supervisor.moveOutcome() == Supervisor::MoveOutcome::None);
    CHECK(supervisor.startHome(1, 0, 150, 3000));
    supervisor.reset();
    CHECK(!supervisor.active());
    CHECK(supervisor.homeOutcome() == Supervisor::HomeOutcome::None);
}

static void millisWrapPreservesFreshnessAndDeadlines() {
    const uint32_t start = 0xFFFFFFF0u;
    {
        Supervisor supervisor;
        CHECK(supervisor.startMove(moveAt(start)));
        supervisor.acceptedAck(1, 0xFD, false, start + 1u);
        checkPending(supervisor.poll(feedback(start + 2u)));
        auto verdict = supervisor.poll(feedback(start + 3u));
        CHECK(verdict.kind == Supervisor::Kind::Move);
        CHECK(verdict.done);
    }
    {
        Supervisor supervisor;
        CHECK(supervisor.startMove(moveAt(start)));
        supervisor.acceptedAck(1, 0xFD, false, start + 1u);
        checkPending(supervisor.poll(feedback(start + 3000u, 0)));
        CHECK(supervisor.poll(feedback(start + 3001u, 0)).fault ==
              Supervisor::Fault::MoveTimeout);
    }
    {
        Supervisor supervisor;
        CHECK(supervisor.startHome(1, 0, start, 3000));
        supervisor.acceptedAck(1, 0x9A, true, start + 20u);
        auto verdict = supervisor.poll(feedback(start + 21u));
        CHECK(verdict.kind == Supervisor::Kind::Home);
        CHECK(verdict.done);
    }
    {
        // Zero is a valid millisecond tick after wrap, not "no 9F seen".
        Supervisor supervisor;
        const uint32_t nearWrap = 0xFFFFFFFEu;
        CHECK(supervisor.startHome(1, 0, nearWrap, 3000));
        supervisor.acceptedAck(1, 0x9A, true, nearWrap + 2u);
        auto verdict = supervisor.poll(feedback(nearWrap + 3u));
        CHECK(verdict.kind == Supervisor::Kind::Home);
        CHECK(verdict.done);
    }
    {
        // The running-to-stopped 0x3B transition can likewise land on tick 0.
        Supervisor supervisor;
        const uint32_t nearWrap = 0xFFFFFFFEu;
        CHECK(supervisor.startHome(1, 0, nearWrap, 3000));
        supervisor.acceptedAck(1, 0x9A, false, nearWrap + 1u);
        auto running = feedback(nearWrap + 1u);
        running.homeFlagsFresh = true;
        running.homeFlags = 0x04;
        running.homeFlagsMs = nearWrap + 1u;
        checkPending(supervisor.poll(running));
        auto stopped = feedback(nearWrap + 2u);
        stopped.homeFlagsFresh = true;
        stopped.homeFlags = 0;
        stopped.homeFlagsMs = nearWrap + 2u;
        checkPending(supervisor.poll(stopped));
        auto first = feedback(nearWrap + 3u);
        first.homeFlagsFresh = true;
        first.homeFlags = 0;
        first.homeFlagsMs = nearWrap + 2u;
        checkPending(supervisor.poll(first));
        auto second = feedback(nearWrap + 4u);
        second.homeFlagsFresh = true;
        second.homeFlags = 0;
        second.homeFlagsMs = nearWrap + 2u;
        auto verdict = supervisor.poll(second);
        CHECK(verdict.kind == Supervisor::Kind::Home);
        CHECK(verdict.done);
    }
}

int main() {
    moveNeedsMatchingAckAndTwoIndependentPairs();
    directMovesNeedTheirOwnTargetReadback();
    homeNeedsRunningThenStoppedAndTwoPairs();
    completedAckStillNeedsLaterStationaryFeedback();
    noMotionAndHomeFaultsRemainDistinct();
    deadlinesAndFailureSnapshotAreOwnedBySupervisor();
    cancellationAndExclusivityDoNotLoseOwnership();
    millisWrapPreservesFreshnessAndDeadlines();
    if (failures) {
        std::printf("FAIL manual operation supervisor: %d/%d checks\n", failures, checks);
        return 1;
    }
    std::printf("PASS manual operation supervisor: %d checks\n", checks);
    return 0;
}
