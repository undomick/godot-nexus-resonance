#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_player_debug_hud_policy.h"

TEST_CASE("player debug HUD: shared bus RMS only for convolution reflections", "[debug_hud][hud02]") {
    using resonance::kReflectionConvolution;
    using resonance::kReflectionHybrid;
    using resonance::kReflectionParametric;
    using resonance::kReflectionTan;
    using resonance::player_debug_hud_use_shared_reverb_bus_rms;

    REQUIRE(player_debug_hud_use_shared_reverb_bus_rms(true, kReflectionConvolution));
    REQUIRE_FALSE(player_debug_hud_use_shared_reverb_bus_rms(true, kReflectionParametric));
    REQUIRE_FALSE(player_debug_hud_use_shared_reverb_bus_rms(true, kReflectionHybrid));
    REQUIRE_FALSE(player_debug_hud_use_shared_reverb_bus_rms(true, kReflectionTan));
    REQUIRE_FALSE(player_debug_hud_use_shared_reverb_bus_rms(false, kReflectionConvolution));
}

TEST_CASE("player debug drawer: wants visuals when occ or HUD label enabled", "[debug_hud][qa01]") {
    using resonance::player_debug_drawer_wants_visuals;

    REQUIRE(player_debug_drawer_wants_visuals(true, false));
    REQUIRE(player_debug_drawer_wants_visuals(false, true));
    REQUIRE(player_debug_drawer_wants_visuals(true, true));
    REQUIRE_FALSE(player_debug_drawer_wants_visuals(false, false));
}

TEST_CASE("player debug HUD: distance label line format", "[debug_hud][qa01][dbg04]") {
    using resonance::format_player_debug_hud_distance_line;

    REQUIRE(format_player_debug_hud_distance_line(12.345f) == "Dist: 12.35 m\n");
    REQUIRE(format_player_debug_hud_distance_line(0.0f) == "Dist: 0.00 m\n");
}
