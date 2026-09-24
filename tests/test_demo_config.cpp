#include "DemoFlowConfig.h"
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
    assert(parseDemoConfig(source.c_str(), source.size(), c, error));
    assert(!c.configured && c.axes.empty());
    assert(!parseDemoConfig("{\"schema_version\":1,\"schema_version\":2}",39,c,error));
    const char* configured = R"({"schema_version":1,"name":"fixture","axes":[{"motor_id":1,"rotation_distance_mm":0}],"display":{"baby_name":"demo","formula_brand":"test","water_ml":180,"temperature_c":45},"initialization":{"timeout_ms":5000,"zero_axes":[{"motor_id":1,"zero_tolerance_deg":1}],"commands":["enable 1","home 1 2 await"]},"stages":[{"id":"open_cap","timeout_ms":5000,"commands":["move 1 1 deg 10 20 20 100 await"]},{"id":"water","timeout_ms":5000,"commands":["wait 100"]},{"id":"powder","timeout_ms":5000,"commands":["wait 100"]},{"id":"close_cap","timeout_ms":5000,"commands":["wait 100"]},{"id":"mix","timeout_ms":5000,"commands":["zero 1 10 20 20 100","wait 100"]}]})";
    assert(parseDemoConfig(configured, std::strlen(configured), c,error)); assert(c.configured);
    QueueProgram program; std::array<int32_t,256> zeros{}; zeros[1] = -123;
    assert(buildDemoProgram(c.stages[4],c,false,zeros,program,error));
    assert(program.steps[0].absolute && program.steps[0].distanceTenths == -123);
    assert(program.steps[0].awaitCompletion && program.count == 2);
    const char* forbidden[] = {"move 1 1 deg", "home 1 2 await", "velocity 1 10", "disable 1", "hex 01 FE 98 00 6B", "move 2 1 deg await", "zero 2 10 20 20 100"};
    for (auto command : forbidden) {
        DemoScript s; s.commands.push_back(command);
        assert(!buildDemoProgram(s,c,false,zeros,program,error));
    }
    std::string bad(configured); bad += "garbage";
    assert(!parseDemoConfig(bad.c_str(),bad.size(),c,error));
    bad.assign(20,'['); bad.append(20,']'); assert(!parseDemoConfig(bad.c_str(),bad.size(),c,error));
    bad = configured; bad.replace(bad.find("wait 100"),8,"wait 100\\nstop 1");
    assert(!parseDemoConfig(bad.c_str(),bad.size(),c,error));
    std::cout << "PASS demo JSON bounds, atomic validation, safe commands, axes and absolute software zero\n";
}
