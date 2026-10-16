#include "DemoFlowConfig.h"
#include "CommandQueue.h"
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
using namespace motion;
int main(int argc, char** argv) {
    assert(argc == 2);
    std::ifstream file(argv[1]);
    std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    DemoConfig c; std::string error;
    assert(parseDemoConfig(source.c_str(), source.size(), c, error) && c.configured);
    QueueProgram program; std::array<int32_t,256> zeros{}; zeros[1] = -123;
    const auto assertLift = [](const QueueStep& step, int32_t distance, uint16_t rpm, uint16_t ramp) {
        assert(step.action == QueueAction::Move && step.id == 1 && !step.absolute);
        assert(step.distanceTenths == distance && step.awaitCompletion);
        assert(step.speedTenths == rpm * 10 && step.accelRpmS == ramp && step.decelRpmS == ramp);
        assert(step.currentMa == 200);
    };
    const auto assertCarriage = [](const QueueStep& step, int32_t distance) {
        assert(step.action == QueueAction::Move && step.id == 3 && !step.absolute);
        assert(step.distanceTenths == distance && step.awaitCompletion);
        assert(step.speedTenths == 12000 && step.accelRpmS == 1200 && step.decelRpmS == 1200);
        assert(step.currentMa == 650);
    };
    assert(buildDemoProgram(c.stages[0], c, false, zeros, program, error));
    assert(program.count == 12);
    assertLift(program.steps[4], -100800, 2400, 2400); // -56 mm: fast first 80%.
    assertLift(program.steps[5], -25200, 600, 600);     // -14 mm: unchanged final 20%.
    assert(program.steps[4].distanceTenths + program.steps[5].distanceTenths == -126000);
    assertCarriage(program.steps[3], 6903); // +76.7 mm at 40 mm/rev.
    assertCarriage(program.steps[10], -6903);
    assertLift(program.steps[9], 36000, 1200, 600); // +20 mm clearance: doubled speed, unchanged ramp.
    assertLift(program.steps[11], -95400, 3000, 4800);
    const int32_t openLift = program.steps[4].distanceTenths + program.steps[5].distanceTenths +
        program.steps[7].distanceTenths + program.steps[9].distanceTenths + program.steps[11].distanceTenths;
    assert(openLift == -181800); // Dispensing height stays -101 mm.
    assert(buildDemoProgram(c.stages[4], c, false, zeros, program, error));
    assert(program.count == 8);
    assertLift(program.steps[0], 24552, 600, 600);   // 13.64 mm: unchanged first 20%.
    assertLift(program.steps[1], 98208, 2400, 2400); // 54.56 mm: fast final 80%.
    assertCarriage(program.steps[2], -6903);
    assert(program.steps[0].distanceTenths + program.steps[1].distanceTenths == 122760);
    const int32_t returnTravel = program.steps[0].distanceTenths + program.steps[1].distanceTenths;
    assert(buildDemoProgram(c.stages[3], c, false, zeros, program, error));
    assertLift(program.steps[0], 95400, 3000, 4800);
    assertLift(program.steps[2], -30600, 600, 600);
    assertCarriage(program.steps[1], 6903);
    assert(openLift + program.steps[0].distanceTenths == -86400); // -48 mm clearance.
    assert(openLift + program.steps[0].distanceTenths + program.steps[2].distanceTenths == -117000);
    assert(program.count == 13);
    assert(program.steps[3].action == QueueAction::SyncBegin &&
           program.steps[3].groupSize == 2 && program.steps[3].syncTriggerOnly);
    assert(program.steps[4].action == QueueAction::Move && program.steps[4].id == 1 &&
           !program.steps[4].awaitCompletion);
    assert(program.steps[5].action == QueueAction::Move && program.steps[5].id == 2 &&
           !program.steps[5].awaitCompletion);
    assert(program.steps[6].action == QueueAction::SyncEnd);
    assert(program.steps[7].action == QueueAction::SyncBegin &&
           program.steps[7].groupSize == 2 && program.steps[7].syncTriggerOnly);
    const int32_t closedLift = openLift + program.steps[0].distanceTenths + program.steps[2].distanceTenths +
        program.steps[4].distanceTenths + program.steps[8].distanceTenths;
    assert(closedLift == -122760 && closedLift + returnTravel == 0);
    {
        MotorControl motor; CommandQueue queue(motor);
        assert(motor.begin(4, 5, 500000));
        assert(!queue.syncSettings().valid());
        assert(queue.startDemo(program, 1).code == 202); // Actual embedded close-cap script.
        assert(!queue.syncSettings().valid()); // Product fallback does not configure manual sync.
        queue.cancel("test");
        struct Rotation : QueueRotationSource {
            bool rotationMm(uint8_t, double&) const override { return false; }
        } rotation;
        const char* text = "sync begin trigger\nmove 1 360 deg 1 60 60 800\nmove 2 -180 deg 1 60 60 800\nsync end";
        auto result = queue.start(text, std::strlen(text), 1, rotation, 2);
        assert(result.code == 400 && std::strcmp(result.message, "sync_settings_unconfigured") == 0);
        auto settings = productSyncDefaults(); settings.responseBudgetMs = 500;
        assert(queue.setSyncSettings(settings));
        result = queue.startDemo(program, 3);
        assert(result.code == 400 && std::strcmp(result.message, "sync_feedback_budget_insufficient") == 0);
        assert(queue.syncSettings().responseBudgetMs == 500); // No fallback over an existing valid setting.
        settings.responseBudgetMs = 20; settings.stopTimeoutMs = 2000;
        assert(queue.setSyncSettings(settings) && queue.startDemo(program, 4).code == 202);
        assert(queue.syncSettings().responseBudgetMs == 20 && queue.syncSettings().stopTimeoutMs == 2000);
        queue.cancel("test");
        std::cout << "PASS embedded close-cap sync: blank product defaults, manual isolation and existing-setting preservation\n";
    }
    DemoScript badSync;
    badSync.commands = {"sync begin trigger", "move 1 1 mm 10 20 20 100",
                        "move 2 360 deg 10 20 20 100"};
    assert(!buildDemoProgram(badSync, c, false, zeros, program, error));
    assert(error == "command_1:sync_missing_end");
    badSync.commands.push_back("sync end");
    assert(buildDemoProgram(badSync, c, false, zeros, program, error));
    assert(!buildDemoProgram(badSync, c, true, zeros, program, error));
    assert(error == "sync_not_allowed_during_initialization");
    badSync.commands[2] = "move 1 360 deg 10 20 20 100";
    assert(!buildDemoProgram(badSync, c, false, zeros, program, error));
    assert(error == "command_3:sync_duplicate_id");
    badSync.commands[2] = "move 6 360 deg 10 20 20 100";
    assert(!buildDemoProgram(badSync, c, false, zeros, program, error));
    assert(error == "command_axis_not_declared");
    badSync.commands[2] = "move 2 360 deg 10 20 20 100 await";
    assert(!buildDemoProgram(badSync, c, false, zeros, program, error));
    assert(error == "command_3:sync_only_relative_move");
    assert(!parseDemoConfig("{\"schema_version\":1,\"schema_version\":2}",39,c,error));
    const char* configured = R"({"schema_version":1,"name":"fixture","axes":[{"motor_id":1,"rotation_distance_mm":0}],"display":{"baby_name":"demo","formula_brand":"test","water_ml":180,"temperature_c":45},"initialization":{"timeout_ms":5000,"zero_axes":[{"motor_id":1,"zero_tolerance_deg":1}],"commands":["enable 1","home 1 2 await"]},"stages":[{"id":"open_cap","timeout_ms":5000,"commands":["move 1 1 deg 10 20 20 100 await"]},{"id":"water","timeout_ms":5000,"commands":["wait 100"]},{"id":"powder","timeout_ms":5000,"commands":["wait 100"]},{"id":"close_cap","timeout_ms":5000,"commands":["wait 100"]},{"id":"mix","timeout_ms":5000,"commands":["zero 1 10 20 20 100","wait 100"]}]})";
    assert(parseDemoConfig(configured, std::strlen(configured), c,error)); assert(c.configured);
    const std::string distanceField = "\"rotation_distance_mm\":0";
    std::string millimetres(configured);
    millimetres.replace(millimetres.find("move 1 1 deg"), std::strlen("move 1 1 deg"), "move 1 1 mm");
    for (const char* value : {"0", "-2", "null", "false", "\"2\"", "1e309", "1000001"}) {
        std::string invalid(millimetres);
        invalid.replace(invalid.find(distanceField), distanceField.size(),
                        std::string("\"rotation_distance_mm\":") + value);
        DemoConfig rejected;
        assert(!parseDemoConfig(invalid.c_str(), invalid.size(), rejected, error));
    }
    millimetres.replace(millimetres.find(distanceField), distanceField.size(), "\"rotation_distance_mm\":2");
    DemoConfig metric;
    assert(parseDemoConfig(millimetres.c_str(), millimetres.size(), metric, error) && metric.configured);
    assert(buildDemoProgram(metric.stages[0], metric, false, zeros, program, error));
    assert(program.steps[0].distanceTenths == 1800);
    assert(buildDemoProgram(c.stages[4],c,false,zeros,program,error));
    assert(program.steps[0].absolute && !program.steps[0].absoluteCommand &&
           program.steps[0].distanceTenths == -123);
    assert(program.steps[0].awaitCompletion && program.count == 2);
    DemoScript absoluteStage;
    absoluteStage.commands = {"moveabs 1 0 await"};
    assert(buildDemoProgram(absoluteStage,c,false,zeros,program,error));
    assert(program.count == 1 && program.steps[0].absolute && program.steps[0].absoluteCommand &&
           program.steps[0].distanceTenths == 0 && program.steps[0].awaitCompletion);
    assert(zeros[1] == -123);  // moveabs does not replace the demo software zero.
    DemoScript disableScript;
    disableScript.commands = {"enable 1", "disable 1"};
    assert(buildDemoProgram(disableScript,c,false,zeros,program,error));
    assert(program.count == 2 && program.steps[1].action == QueueAction::Disable);
    assert(buildDemoProgram(disableScript,c,true,zeros,program,error));
    std::string withDisable(configured);
    const std::string homeCommand = "\"home 1 2 await\"";
    withDisable.replace(withDisable.find(homeCommand), homeCommand.size(),
                        "\"home 1 2 await\",\"disable 1\"");
    const std::string openCommand = "\"move 1 1 deg 10 20 20 100 await\"";
    withDisable.replace(withDisable.find(openCommand), openCommand.size(),
                        "\"disable 1\",\"move 1 1 deg 10 20 20 100 await\"");
    assert(parseDemoConfig(withDisable.c_str(), withDisable.size(), c, error) && c.configured);
    DemoScript undeclaredDisable;
    undeclaredDisable.commands = {"disable 2"};
    assert(!buildDemoProgram(undeclaredDisable,c,false,zeros,program,error));
    assert(error == "command_axis_not_declared");
    const char* forbidden[] = {"move 1 1 deg", "home 1 2 await", "velocity 1 10", "hex 01 FE 98 00 6B", "move 2 1 deg await", "zero 2 10 20 20 100"};
    for (auto command : forbidden) {
        DemoScript s; s.commands.push_back(command);
        assert(!buildDemoProgram(s,c,false,zeros,program,error));
    }
    std::string bad(configured); bad += "garbage";
    assert(!parseDemoConfig(bad.c_str(),bad.size(),c,error));
    bad.assign(20,'['); bad.append(20,']'); assert(!parseDemoConfig(bad.c_str(),bad.size(),c,error));
    bad = configured; bad.replace(bad.find("wait 100"),8,"wait 100\\nstop 1");
    assert(!parseDemoConfig(bad.c_str(),bad.size(),c,error));
    std::cout << "PASS demo JSON bounds, required finite mm conversion, sync groups, atomic validation, safe commands, axes and absolute software zero\n";
}
