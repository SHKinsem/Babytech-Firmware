#include "ProductCommandDispatcher.h"

#include <cassert>
#include <cstring>
#include <limits>

using namespace motion;
using babytech::display::DisplayStage;

struct FakeExecutor : DemoExecutor {
    int starts = 0;
    int stops = 0;
    bool stopAccepted = true;
    DemoExecution state = DemoExecution::Done;
    bool healthy() const override { return true; }
    bool available() const override { return true; }
    DemoEvidence evidence(uint8_t) const override { return {true, true, false, 100}; }
    bool start(const DemoScript&, bool, const std::array<int32_t, 256>&, uint32_t) override {
        ++starts;
        state = DemoExecution::Running;
        return true;
    }
    DemoExecution execution() const override { return state; }
    bool stop() override { ++stops; return stopAccepted; }
    bool reset() override { return true; }
};

struct CountingGuard : ProductStartGuard {
    int journals = 0;
    bool ready() const override { return true; }
    bool prepare(ProductRun&, uint32_t) override { ++journals; return true; }
};

ProductCommandOutcome dispatch(const char* json, ProductSession& product,
                               bool otaActive = false, uint32_t now = 20) {
    DynamicJsonDocument document(768);
    assert(!deserializeJson(document, json));
    return executeProductCommand(document.as<JsonVariantConst>(), product, otaActive, now);
}

void expect(const ProductCommandOutcome& outcome, bool accepted,
            const char* status, const char* reason) {
    assert(outcome.accepted == accepted);
    assert(std::strcmp(outcome.status, status) == 0);
    assert((!outcome.reason && !reason) ||
           (outcome.reason && reason && std::strcmp(outcome.reason, reason) == 0));
}

void ready(DemoFlowController& flow, FakeExecutor& executor) {
    DemoConfig config;
    config.configured = true;
    config.axes.push_back({1, 10, 0, true});
    assert(flow.apply(config));
    assert(flow.initialize(10));
    flow.tick(10);
    executor.state = DemoExecution::Done;
    flow.tick(11);
    assert(flow.stage() == DisplayStage::Ready);
}

int main() {
    FakeExecutor executor;
    DemoFlowController flow(executor);
    ready(flow, executor);
    ProductSession product(flow);
    CountingGuard guard;
    product.setStartGuard(&guard);
    product.setExecutionAuthorized(true);
    product.resources(true, false, true, 350, 12);

    const int startsBeforeOta = executor.starts;
    expect(dispatch(R"({"command":"prepare","command_id":"cmd-1","water_ml":180,"temp":45,"powder_g_per_100ml":25})",
                    product, true), false, "rejected", "ota_active");
    expect(dispatch(R"({"command":"clean"})", product, true),
           false, "rejected", "ota_active");
    assert(executor.starts == startsBeforeOta && !product.active());

    expect(dispatch(R"({"command":"set_target_temp","temp":47})", product),
           true, "accepted", nullptr);
    assert(product.targetTemp() == 47);
    // Optional means absent, not a supplied value of the wrong JSON type.
    for (const char* field : {"water_ml", "temp", "powder_g_per_100ml"}) {
        for (const char* value : {"null", "true", "\"45\"", "[]", "{}", "1e99"}) {
            DynamicJsonDocument invalid(768), parsedValue(128);
            invalid["command"] = "prepare";
            invalid["command_id"] = "malformed-recipe";
            invalid["baby_id"] = "baby-1";
            invalid["water_ml"] = 180;
            invalid["temp"] = 45;
            invalid["powder_g_per_100ml"] = 25.0f;
            assert(!deserializeJson(parsedValue, value));
            invalid[field] = parsedValue.as<JsonVariantConst>();
            const std::string reason = std::string("invalid_") + field;
            const int starts = executor.starts;
            expect(executeProductCommand(invalid.as<JsonVariantConst>(), product, false, 20),
                   false, "rejected", reason.c_str());
            assert(executor.starts == starts && !product.active() && !flow.busy());
            assert(product.targetTemp() == 47);
            assert(guard.journals == 0);
        }
    }
    for (const char* field : {"water_ml", "temp", "powder_g_per_100ml"}) {
        const double minimum = std::strcmp(field, "water_ml") == 0 ? 30 :
            (std::strcmp(field, "temp") == 0 ? 35 : 1);
        const double maximum = std::strcmp(field, "water_ml") == 0 ? 500 :
            (std::strcmp(field, "temp") == 0 ? 60 : 50);
        for (double value : {std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity(),
                             -std::numeric_limits<double>::infinity(), minimum - 0.1, maximum + 0.1}) {
            DynamicJsonDocument invalid(768);
            invalid["command"] = "prepare";
            invalid["command_id"] = "numeric-bounds";
            invalid["baby_id"] = "baby-1";
            invalid["water_ml"] = 180;
            invalid["temp"] = 45;
            invalid["powder_g_per_100ml"] = 25;
            invalid[field] = value;
            const std::string reason = std::string("invalid_") + field;
            expect(executeProductCommand(invalid.as<JsonVariantConst>(), product, false, 20),
                   false, "rejected", reason.c_str());
            assert(guard.journals == 0 && !product.active() && !flow.busy());
            assert(product.targetTemp() == 47);
        }
    }
    expect(dispatch(R"({"command":"prepare","command_id":"legacy-ratio","baby_id":"baby-1","ratio":1.5,"powder_g_per_100ml":25})",
                    product), false, "rejected", "invalid_powder_g_per_100ml");
    assert(!product.active() && !flow.busy());
    expect(dispatch(R"({"command":"prepare","command_id":"legacy-null","baby_id":"baby-1","ratio":null,"powder_g_per_100ml":25})",
                    product), false, "rejected", "invalid_powder_g_per_100ml");
    expect(dispatch(R"({"command":"prepare","command_id":"missing-powder","baby_id":"baby-1"})",
                    product), false, "rejected", "invalid_powder_g_per_100ml");
    assert(guard.journals == 0 && !product.active() && !flow.busy());
    expect(dispatch(R"({"command":"set_target_temp","temp":90})", product),
           false, "rejected", "invalid_temp");
    for (const char* value : {"null", "true", "\"45\"", "[]", "{}", "1e99", "34.9", "60.1"}) {
        const std::string json = std::string("{\"command\":\"set_target_temp\",\"temp\":") + value + "}";
        expect(dispatch(json.c_str(), product), false, "rejected", "invalid_temp");
        assert(product.targetTemp() == 47 && !product.active());
    }
    expect(dispatch(R"({"command":"set_target_temp","temp":48.0})", product),
           true, "accepted", nullptr);
    assert(product.targetTemp() == 48);
    expect(dispatch(R"({"command":"prepare","command_id":"cmd-invalid","water_ml":180,"temp":45,"powder_g_per_100ml":0})",
                    product), false, "rejected", "invalid_powder_g_per_100ml");

    expect(dispatch(R"({"command":"prepare","command_id":"cmd-1","baby_id":"baby-1","water_ml":180,"temp":45,"powder_g_per_100ml":25})",
                    product), true, "accepted", nullptr);
    assert(product.active() && product.activeRun().commandId == "cmd-1");
    assert(product.activeRun().babyId == "baby-1");
    assert(product.activeRun().targetPowderG == 45.0f);
    assert(product.targetTemp() == 45);
    expect(dispatch(R"({"command":"set_target_temp","temp":48})", product),
           false, "rejected", "busy");
    expect(dispatch(R"({"command":"prepare","command_id":"cmd-2","water_ml":180,"temp":45,"powder_g_per_100ml":25})",
                    product), false, "rejected", "busy");
    expect(dispatch(R"({"command":"stop"})", product, true, 21),
           true, "accepted", nullptr);
    assert(executor.stops == 1);
    expect(dispatch(R"({"command":"check_firmware_update"})", product),
           false, "rejected", "cloud_ota_not_supported");
    expect(dispatch(R"({"command":"unknown"})", product),
           false, "rejected", "unknown_command");

    FakeExecutor failedExecutor;
    DemoFlowController failedFlow(failedExecutor);
    ready(failedFlow, failedExecutor);
    ProductSession failedProduct(failedFlow);
    failedProduct.setExecutionAuthorized(true);
    failedProduct.resources(true, false, true, 350, 12);
    expect(dispatch(R"({"command":"prepare","command_id":"cmd-3","baby_id":"baby-1","water_ml":180,"temp":45,"powder_g_per_100ml":25})",
                    failedProduct), true, "accepted", nullptr);
    failedExecutor.stopAccepted = false;
    expect(dispatch(R"({"command":"stop"})", failedProduct),
           false, "failed", "stop_unconfirmed");
    ProductTerminal terminal;
    assert(failedProduct.takeTerminal(terminal));
    assert(!terminal.completed && terminal.errorCode == "E_CAN_FAULT");

    FakeExecutor defaultExecutor;
    DemoFlowController defaultFlow(defaultExecutor);
    ready(defaultFlow, defaultExecutor);
    ProductSession defaultProduct(defaultFlow);
    defaultProduct.setExecutionAuthorized(true);
    defaultProduct.resources(true, false, true, 350, 12);
    expect(dispatch(R"({"command":"prepare","command_id":"defaults","baby_id":"baby-1","powder_g_per_100ml":25})",
                    defaultProduct), true, "accepted", nullptr);
    assert(defaultProduct.activeRun().recipe.waterMl == 180);
    assert(defaultProduct.activeRun().recipe.temperatureC == 45);

    FakeExecutor numericExecutor;
    DemoFlowController numericFlow(numericExecutor);
    ready(numericFlow, numericExecutor);
    ProductSession numericProduct(numericFlow);
    numericProduct.setExecutionAuthorized(true);
    numericProduct.resources(true, false, true, 350, 12);
    expect(dispatch(R"({"command":"prepare","command_id":"numeric","baby_id":"baby-1","water_ml":150.0,"temp":48.0,"powder_g_per_100ml":25.5})",
                    numericProduct), true, "accepted", nullptr);
    assert(numericProduct.activeRun().recipe.waterMl == 150);
    assert(numericProduct.activeRun().recipe.temperatureC == 48);
    assert(numericProduct.activeRun().recipe.powderGPer100Ml == 25.5f);
    assert(numericProduct.targetTemp() == 48);
    for (bool upper : {false, true}) {
        FakeExecutor boundaryExecutor;
        DemoFlowController boundaryFlow(boundaryExecutor);
        ready(boundaryFlow, boundaryExecutor);
        ProductSession boundaryProduct(boundaryFlow);
        boundaryProduct.setExecutionAuthorized(true);
        boundaryProduct.resources(true, false, true, 350, 12);
        const char* json = upper ?
            R"({"command":"prepare","command_id":"maximum","baby_id":"baby-1","water_ml":500,"temp":60,"powder_g_per_100ml":50})" :
            R"({"command":"prepare","command_id":"minimum","baby_id":"baby-1","water_ml":30,"temp":35,"powder_g_per_100ml":1})";
        expect(dispatch(json, boundaryProduct), true, "accepted", nullptr);
    }
}
