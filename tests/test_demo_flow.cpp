#include "DemoFlowController.h"
#include "DisplayLinkCore.h"
#include <cassert>
#include <iostream>
using namespace motion;
using namespace babytech::display;

struct Fake : DemoExecutor {
    bool ok = true, free = true, fresh = true, stationary = true;
    int starts = 0, stops = 0, resets = 0;
    int32_t position = 100;
    DemoExecution state = DemoExecution::Done;
    bool healthy() const override { return ok; }
    bool available() const override { return free; }
    DemoEvidence evidence(uint8_t) const override { return {fresh, stationary, !ok, position}; }
    bool start(const DemoScript&, bool, const std::array<int32_t,256>&, uint32_t) override {
        ++starts; state = DemoExecution::Running; return true;
    }
    DemoExecution execution() const override { return state; }
    bool stop() override { ++stops; state = DemoExecution::Failed; return ok; }
    bool reset() override { ++resets; ok = true; return true; }
};
DemoConfig config() {
    DemoConfig c;
    c.configured = true; c.axes.push_back({1, 10, 0, true});
    c.initialization.timeoutMs = 5000;
    for (auto& s : c.stages) s.timeoutMs = 5000;
    return c;
}
void ready(DemoFlowController& f, Fake& e, uint32_t now = 10) {
    assert(f.apply(config())); assert(e.starts == 0);
    assert(f.initialize(now)); f.tick(now); assert(e.starts == 1);
    e.state = DemoExecution::Done; f.tick(now + 1);
    assert(f.stage() == DisplayStage::Ready && f.referenceValid());
}
int main() {
    {
        Fake e; DemoFlowController f(e); DisplayLinkCore link(f);
        auto send = [&](DisplayIntent intent, uint32_t seq, uint32_t now) {
            uint8_t payload[96], frame[108], reply[108];
            const auto p = encodeDisplayIntentPayload(intent,payload,sizeof(payload));
            const auto n = encodeDisplayFrame(DisplayMessageType::Intent,seq,payload,p,frame,sizeof(frame));
            size_t count = 0;
            for (size_t i=0;i<n;++i) count=link.receive(frame[i],now,reply,sizeof(reply));
            assert(count > 0);
            DisplayFrame decoded; DisplayFrameParser parser;
            for (size_t i=0;i<count;++i) parser.push(reply[i],decoded);
            DisplayAck ack; assert(decodeDisplayAckPayload(decoded,ack)); return ack.accepted;
        };
        assert(!send(DisplayIntent::Initialize,1,0)); // empty config never moves
        assert(f.apply(config())); assert(e.starts == 0);
        assert(send(DisplayIntent::Initialize,2,10));
        assert(f.stage() == DisplayStage::NotReady && !f.startEnabled());
        assert(send(DisplayIntent::Initialize,2,11)); // retransmission replays ACK
        assert(!send(DisplayIntent::StartFeeding,2,11)); // same sequence, different intent
        assert(!send(DisplayIntent::Initialize,3,11)); // second click while busy
        assert(!send(DisplayIntent::StartFeeding,4,11));
        f.tick(12); assert(e.starts == 1);
        e.state = DemoExecution::Done; f.tick(13);
        assert(f.startEnabled() && f.referenceValid());
        assert(send(DisplayIntent::Initialize,2,14)); assert(e.starts == 1);
        assert(!send(DisplayIntent::Initialize,5,14)); // Ready is not an initialize button state
        assert(send(DisplayIntent::StartFeeding,6,15)); f.tick(15);
        assert(!send(DisplayIntent::Initialize,7,16));
        f.tick(5015); f.tick(5016); assert(f.stage() == DisplayStage::Error);
        assert(send(DisplayIntent::Initialize,8,5017));
        f.tick(5018); assert(f.startEnabled() && e.resets == 1 && e.starts == 2);
    }
    {
        Fake e; DemoFlowController f(e);
        assert(!f.start(0)); assert(!f.initialize(0)); assert(e.starts == 0);
        ready(f, e);
        assert(f.snapshot().startEnabled && !f.snapshot().cloudConnected);
        assert(f.snapshot().primaryCondition == DisplayCondition::None);
        assert(f.start(20)); assert(!f.start(20)); assert(!f.apply(config()));
        for (uint32_t stage = 0; stage < 5; ++stage) {
            const auto now = 21 + stage * 10;
            f.tick(now); e.state = DemoExecution::Done; f.tick(now + 1);
        }
        assert(f.stage() == DisplayStage::Complete && e.starts == 6);
        f.tick(3061); assert(f.stage() == DisplayStage::Complete);
        f.tick(3062); assert(f.startEnabled() && e.starts == 6);
        assert(f.start(3100)); f.stop(3100); f.tick(3101);
        assert(f.stage() == DisplayStage::NotReady && f.error() == DisplayError::None);
        assert(f.initialize(3102)); assert(f.startEnabled() && e.starts == 6);
    }
    {
        Fake e; DemoFlowController f(e); ready(f, e);
        assert(f.start(20)); f.tick(20);
        f.tick(5020); assert(f.error() == DisplayError::CapUnscrewTimeout && e.stops == 1);
        f.tick(5021); assert(!f.busy() && f.stage() == DisplayStage::Error);
        assert(!f.start(6000)); assert(f.initialize(6000)); f.tick(6001);
        assert(f.startEnabled() && e.resets == 1);
    }
    {
        Fake e; DemoFlowController f(e); ready(f,e);
        e.position = 200; f.tick(12);
        assert(!f.startEnabled() && f.referenceValid());
        assert(!f.initialize(13)); // reset does not invent a new zero
        e.position = 100; assert(f.initialize(14));
        assert(f.start(20)); f.tick(20); e.fresh = false; f.tick(1021);
        assert(f.error() == DisplayError::CanFault && !f.referenceValid() && e.stops == 1);
        f.tick(4021); assert(!f.busy() && f.stage() == DisplayStage::Error);
    }
    {
        Fake e; DemoFlowController f(e); ready(f,e);
        assert(f.single(4,20)); f.tick(20); e.state = DemoExecution::Done; f.tick(21);
        assert(f.stage() == DisplayStage::NotReady);
        assert(f.initialize(22)); f.stop(23); e.stationary = false; f.tick(3023);
        assert(f.error() == DisplayError::Unknown && !f.referenceValid());
    }
    {
        Fake e; DemoFlowController f(e); ready(f,e,UINT32_MAX-20);
        assert(f.start(UINT32_MAX-10)); f.tick(UINT32_MAX-10); f.tick(4990);
        assert(f.error() == DisplayError::CapUnscrewTimeout);
    }
    {
        Fake e; DemoFlowController f(e); ready(f,e);
        DisplayLinkCore link(f);
        uint8_t payload[96], frame[108], reply[108];
        const auto p = encodeDisplayIntentPayload(DisplayIntent::StartFeeding,payload,sizeof(payload));
        const auto n = encodeDisplayFrame(DisplayMessageType::Intent,42,payload,p,frame,sizeof(frame));
        const uint8_t golden[] = {0x42,0x54,0x03,0x02,0x01,0x00,0x2a,0x00,0x00,0x00,0x01,0xca,0x5c};
        assert(n == sizeof(golden) && std::memcmp(frame,golden,n) == 0);
        auto send = [&]() {
            size_t count = 0;
            for (size_t i=0; i<n; ++i) count = link.receive(frame[i],20,reply,sizeof(reply));
            assert(count > 0);
            DisplayFrame decoded; DisplayFrameParser parser;
            for (size_t i=0;i<count;++i) parser.push(reply[i],decoded);
            DisplayAck ack; assert(decodeDisplayAckPayload(decoded,ack)); return ack.accepted;
        };
        assert(send()); f.tick(20); assert(e.starts == 2);
        assert(send()); f.tick(21); assert(e.starts == 2);
        assert(link.state(21,reply,sizeof(reply)) > 0);
        assert(link.state(22,reply,sizeof(reply)) == 0);
        assert(link.state(1021,reply,sizeof(reply)) > 0);
        assert(displayCrc16(reinterpret_cast<const uint8_t*>("123456789"),9) == 0x29B1);
    }
    std::cout << "PASS demo lifecycle, safety, timeout, rollover, snapshot, duplicate Start and heartbeat\n";
}
