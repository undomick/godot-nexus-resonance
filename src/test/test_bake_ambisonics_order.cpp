#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_constants.h"

TEST_CASE("clamp_bake_ambisonics_order clamps to 1-3") {
    REQUIRE(resonance::clamp_bake_ambisonics_order(0) == 1);
    REQUIRE(resonance::clamp_bake_ambisonics_order(1) == 1);
    REQUIRE(resonance::clamp_bake_ambisonics_order(2) == 2);
    REQUIRE(resonance::clamp_bake_ambisonics_order(3) == 3);
    REQUIRE(resonance::clamp_bake_ambisonics_order(99) == 3);
    REQUIRE(resonance::clamp_bake_ambisonics_order(-100) == 1);
}

TEST_CASE("resolve_volume_bake_ambisonic_order Use Global vs BakeConfig override") {
    REQUIRE(resonance::resolve_volume_bake_ambisonic_order(0, 2) == 2);
    REQUIRE(resonance::resolve_volume_bake_ambisonic_order(0, 1) == 1);
    REQUIRE(resonance::resolve_volume_bake_ambisonic_order(3, 1) == 3);
    REQUIRE(resonance::resolve_volume_bake_ambisonic_order(1, 3) == 1);
    REQUIRE(resonance::resolve_volume_bake_ambisonic_order(-1, 2) == 2);
    REQUIRE(resonance::resolve_volume_bake_ambisonic_order(99, 1) == 3);
}

TEST_CASE("effective_ambisonics_order uses min of baked and runtime", "[amb-02]") {
    REQUIRE(resonance::effective_ambisonics_order(-1, 1) == 1);
    REQUIRE(resonance::effective_ambisonics_order(1, 1) == 1);
    REQUIRE(resonance::effective_ambisonics_order(2, 2) == 2);
    REQUIRE(resonance::effective_ambisonics_order(2, 3) == 2);
    REQUIRE(resonance::effective_ambisonics_order(3, 1) == 1);
    REQUIRE(resonance::effective_ambisonics_order(3, 2) == 2);
    REQUIRE(resonance::effective_ambisonics_order(1, 3) == 1);
}

TEST_CASE("ambisonics_order_mismatches_bake_setting detects inequality", "[amb-02]") {
    REQUIRE_FALSE(resonance::ambisonics_order_mismatches_bake_setting(2, 2));
    REQUIRE(resonance::ambisonics_order_mismatches_bake_setting(3, 2));
    REQUIRE(resonance::ambisonics_order_mismatches_bake_setting(1, 3));
    REQUIRE_FALSE(resonance::ambisonics_order_mismatches_bake_setting(-1, 1));
    REQUIRE(resonance::ambisonics_order_mismatches_bake_setting(-1, 2));
}

TEST_CASE("ambisonics_order_mismatches_runtime detects inequality", "[amb-02]") {
    REQUIRE_FALSE(resonance::ambisonics_order_mismatches_runtime(2, 2));
    REQUIRE(resonance::ambisonics_order_mismatches_runtime(3, 2));
    REQUIRE(resonance::ambisonics_order_mismatches_runtime(1, 3));
    REQUIRE_FALSE(resonance::ambisonics_order_mismatches_runtime(-1, 1));
}

TEST_CASE("RuntimeConfig SSOT defaults for missing dict keys", "[runtime_config][cfg-02][cfg-03]") {
    REQUIRE(resonance::kDefaultRealtimeRays == 0);
    REQUIRE(resonance::kBakePathingDefaultNumSamples == 4);
    REQUIRE(resonance::kRuntimePathingDefaultNumVisSamples == 4);
    REQUIRE(resonance::kBakePathingDefaultVisRange == Approx(1000.0f));
    REQUIRE(resonance::kBakePathingDefaultPathRange == Approx(1000.0f));
    REQUIRE(resonance::kBakePathingDefaultRadius == Approx(1.0f));
    REQUIRE(resonance::kBakePathingDefaultThreshold == Approx(0.1f));
}
