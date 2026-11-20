#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_listener_reverb_policy.h"
#include "../resonance_player_reflections_policy.h"

using namespace resonance;

TEST_CASE("Player reflections types map to their baked data variation", "[reflections][player]") {
    REQUIRE(player_reflections_baked_data_variation(kPlayerReflectionsRealtime) == -1);
    REQUIRE(player_reflections_baked_data_variation(kPlayerReflectionsBakedStaticSource) == 1);
    REQUIRE(player_reflections_baked_data_variation(kPlayerReflectionsBakedStaticListener) == 2);
}

TEST_CASE("Unknown player reflections type falls back to realtime", "[reflections][player]") {
    REQUIRE(player_reflections_baked_data_variation(7) == -1);
    REQUIRE(player_reflections_baked_data_variation(-1) == -1);
}

TEST_CASE("Static player reflections types keep their 1.1 values", "[reflections][player]") {
    REQUIRE(kPlayerReflectionsBakedStaticSource == 2);
    REQUIRE(kPlayerReflectionsBakedStaticListener == 3);
    // 1 was the per-player Baked Reverb, which no longer exists.
    REQUIRE(player_reflections_baked_data_variation(1) == -1);
}

TEST_CASE("Only the listener uses baked reverb", "[reflections][listener]") {
    REQUIRE(listener_reverb_baked_data_variation(kListenerReverbBaked) == 0);
    REQUIRE(listener_reverb_baked_data_variation(kListenerReverbRealtime) == -1);
}

TEST_CASE("Listener reverb source lives while the listener or FMOD needs it", "[reflections][listener]") {
    REQUIRE(listener_reverb_source_needed(true, false));
    REQUIRE(listener_reverb_source_needed(false, true));
    REQUIRE_FALSE(listener_reverb_source_needed(false, false));
}
