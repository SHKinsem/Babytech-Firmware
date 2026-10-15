#include "ProductContextCodec.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

void verifyContext(const std::string& json) {
    DynamicJsonDocument document(2048);
    assert(!deserializeJson(document, json));
    motion::FeedingContext context;
    assert(motion::decodeFeedingContext(document.as<JsonVariantConst>(), context));
    assert(context.babyId == "fixture-baby-1");
    assert(context.babyName == "Mia");
    assert(context.formulaBrand == "Friso");
    assert(context.recipe.waterMl == 180);
    assert(context.recipe.temperatureC == 45);
    assert(context.recipe.powderGPer100Ml == 25.0f);
    assert(context.profileVersion == 2);
}

int main(int argc, char** argv) {
    verifyContext(R"({"baby_id":"fixture-baby-1","baby_name":"Mia","formula_brand":"Friso","water_ml":180,"temp":45,"powder_g_per_100ml":25,"profile_version":2})");
    if (argc > 1) {
        std::ifstream input(argv[1]);
        assert(input.good());
        verifyContext(std::string(std::istreambuf_iterator<char>(input),
                                  std::istreambuf_iterator<char>()));
    }
    DynamicJsonDocument longDocument(512);
    const std::string tooLong(97, 'x');
    longDocument["baby_id"] = tooLong;
    motion::FeedingContext rejected;
    assert(!motion::decodeFeedingContext(longDocument.as<JsonVariantConst>(), rejected));
    for (const char* field : {"baby_id", "water_ml", "temp", "powder_g_per_100ml",
                              "profile_version"}) {
        DynamicJsonDocument missing(512);
        assert(!deserializeJson(missing, R"({"baby_id":"baby-1","water_ml":180,"temp":45,"powder_g_per_100ml":25,"profile_version":1})"));
        missing.remove(field);
        assert(!motion::decodeFeedingContext(missing.as<JsonVariantConst>(), rejected));
    }
    DynamicJsonDocument wrongType(512);
    assert(!deserializeJson(wrongType, R"({"baby_id":"baby-1","water_ml":"180","temp":45,"powder_g_per_100ml":25,"profile_version":1})"));
    assert(!motion::decodeFeedingContext(wrongType.as<JsonVariantConst>(), rejected));
    wrongType["water_ml"] = 180;
    wrongType["powder_g_per_100ml"] = "25";
    assert(!motion::decodeFeedingContext(wrongType.as<JsonVariantConst>(), rejected));
    wrongType["powder_g_per_100ml"] = 25;
    wrongType["profile_version"] = -1;
    assert(!motion::decodeFeedingContext(wrongType.as<JsonVariantConst>(), rejected));
    wrongType["profile_version"] = 1;
    wrongType["water_ml"] = 0;
    assert(!motion::decodeFeedingContext(wrongType.as<JsonVariantConst>(), rejected));
    DynamicJsonDocument cleared(256);
    assert(!deserializeJson(cleared, R"({"cleared":true,"profile_version":3})"));
    uint32_t version = 0;
    assert(motion::decodeFeedingContextClear(cleared.as<JsonVariantConst>(), version));
    assert(version == 3);
    cleared["profile_version"] = 0;
    assert(!motion::decodeFeedingContextClear(cleared.as<JsonVariantConst>(), version));
    cleared["profile_version"] = "3";
    assert(!motion::decodeFeedingContextClear(cleared.as<JsonVariantConst>(), version));
    cleared["profile_version"] = 3;
    cleared["cleared"] = false;
    assert(!motion::decodeFeedingContextClear(cleared.as<JsonVariantConst>(), version));
    const std::string emoji = "\xF0\x9F\x8D\xBC";
    std::string longName;
    std::string longBrand;
    for (int i = 0; i < 80; ++i) longName += emoji;
    for (int i = 0; i < 120; ++i) longBrand += emoji;
    DynamicJsonDocument unicode(2048);
    unicode["baby_id"] = "baby-unicode";
    unicode["baby_name"] = longName;
    unicode["formula_brand"] = longBrand;
    unicode["water_ml"] = 180;
    unicode["temp"] = 45;
    unicode["powder_g_per_100ml"] = 25;
    unicode["profile_version"] = 4;
    motion::FeedingContext unicodeContext;
    assert(motion::decodeFeedingContext(unicode.as<JsonVariantConst>(), unicodeContext));
    assert(unicodeContext.babyName.size() == 320);
    assert(unicodeContext.formulaBrand.size() == 480);
    std::string stored;
    serializeJson(unicode, stored);
    DynamicJsonDocument reloaded(2048);
    assert(!deserializeJson(reloaded, stored));
    assert(motion::decodeFeedingContext(reloaded.as<JsonVariantConst>(), unicodeContext));
    std::puts("PASS product feeding context decoder");
}
