#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_source_update_policy.h"

using namespace resonance;

namespace {

struct TinyParams {
    float position[3];
    float radius;
    int baked_data_variation;
};

} // namespace

TEST_CASE("source update skip blocked for baked reverb listener probe", "[source_update][conv][parametric]") {
    REQUIRE(source_update_skip_unchanged_allowed(true, true, true, 0, true, false) == false);
    REQUIRE(source_update_skip_unchanged_allowed(true, true, true, 0, false, false));
    REQUIRE(source_update_skip_unchanged_allowed(true, true, false, 0, true, false));
    REQUIRE(source_update_skip_unchanged_allowed(true, false, true, 0, false, false) == false);
}

TEST_CASE("source update skip blocked while attenuation callback curve is dirty", "[source_update][attenuation]") {
    REQUIRE(source_update_skip_unchanged_allowed(true, true, false, -1, false, true) == false);
    REQUIRE(source_update_skip_unchanged_allowed(true, true, false, -1, false, false));
}

TEST_CASE("source update params memcmp equality", "[source_update]") {
    TinyParams a{};
    TinyParams b{};
    a.position[0] = 1.0f;
    b.position[0] = 1.0f;
    REQUIRE(source_update_params_equal(a, b));
    b.radius = 2.0f;
    REQUIRE_FALSE(source_update_params_equal(a, b));
}

TEST_CASE("source sim reflections ignores mix; honors override and output_reverb", "[source_update][sim][steam]") {
    REQUIRE(source_sim_reflections_enabled(-1, true));
    REQUIRE(source_sim_reflections_enabled(1, true));
    REQUIRE_FALSE(source_sim_reflections_enabled(0, true));
    REQUIRE_FALSE(source_sim_reflections_enabled(-1, false));
    REQUIRE_FALSE(source_sim_reflections_enabled(1, false));
}

TEST_CASE("source sim pathing ignores mix; honors override, global, and output_reverb", "[source_update][sim][steam]") {
    REQUIRE(source_sim_pathing_enabled(-1, true, true));
    REQUIRE_FALSE(source_sim_pathing_enabled(-1, false, true));
    REQUIRE(source_sim_pathing_enabled(1, false, true));
    REQUIRE_FALSE(source_sim_pathing_enabled(0, true, true));
    REQUIRE_FALSE(source_sim_pathing_enabled(-1, true, false));
    REQUIRE_FALSE(source_sim_pathing_enabled(1, true, false));
}

TEST_CASE("playback enable ignores mix; honors output gates", "[source_update][playback][steam]") {
    REQUIRE(playback_enable_direct(true));
    REQUIRE_FALSE(playback_enable_direct(false));
    REQUIRE(playback_enable_reverb(true));
    REQUIRE_FALSE(playback_enable_reverb(false));
}

TEST_CASE("playback wants wet effects from enable overrides", "[source_update][playback][steam]") {
    REQUIRE(playback_wants_wet_effects(-1, -1, true));
    REQUIRE(playback_wants_wet_effects(-1, -1, false)); // reflections default on
    REQUIRE_FALSE(playback_wants_wet_effects(0, 0, true));
    REQUIRE_FALSE(playback_wants_wet_effects(0, -1, false));
    REQUIRE(playback_wants_wet_effects(0, 1, false));
    REQUIRE(playback_wants_wet_effects(1, 0, false));
}
