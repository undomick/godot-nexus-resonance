#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_spawn_timing.h"

using namespace resonance;

TEST_CASE("spawn timing charges a single phase", "[spawn_timing]") {
    SpawnTiming t;
    t.enter(SpawnPhase::VoiceDsp, 100);
    t.leave(350);
    REQUIRE(t.total_us(SpawnPhase::VoiceDsp) == 250);
    REQUIRE(t.total_us(SpawnPhase::Source) == 0);
}

TEST_CASE("spawn timing keeps nested phases exclusive", "[spawn_timing]") {
    SpawnTiming t;
    t.enter(SpawnPhase::Source, 0);
    t.enter(SpawnPhase::ReflectionEffect, 10);
    t.leave(110);
    t.leave(130);
    REQUIRE(t.total_us(SpawnPhase::Source) == 30);
    REQUIRE(t.total_us(SpawnPhase::ReflectionEffect) == 100);
}

TEST_CASE("spawn timing accumulates repeated phases", "[spawn_timing]") {
    SpawnTiming t;
    t.enter(SpawnPhase::ParamsPush, 0);
    t.leave(5);
    t.enter(SpawnPhase::ParamsPush, 20);
    t.leave(27);
    REQUIRE(t.total_us(SpawnPhase::ParamsPush) == 12);
}

TEST_CASE("spawn timing keeps the outer phase intact past the nesting limit", "[spawn_timing]") {
    SpawnTiming t;
    t.enter(SpawnPhase::Source, 0);
    for (int i = 0; i < 20; i++)
        t.enter(SpawnPhase::ParamsPush, 10);
    for (int i = 0; i < 20; i++)
        t.leave(10);
    t.leave(30);
    REQUIRE(t.total_us(SpawnPhase::Source) == 30);
}

TEST_CASE("spawn timing ignores an unmatched leave", "[spawn_timing]") {
    SpawnTiming t;
    t.leave(50);
    t.enter(SpawnPhase::VoiceDsp, 60);
    t.leave(70);
    REQUIRE(t.total_us(SpawnPhase::VoiceDsp) == 10);
}
