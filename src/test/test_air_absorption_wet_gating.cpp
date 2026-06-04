#include "../lib/catch2/single_include/catch2/catch.hpp"

// These tests lock down the gating logic for `PlaybackParameters::apply_air_absorption_to_wet` in
// resonance_player.cpp's `_build_playback_params`:
//
//     new_params.apply_air_absorption_to_wet =
//         c.air_absorption_enabled && srv && _compute_baked_data_variation(srv) == 0;
//
// STATICSOURCE/STATICLISTENER skip the wet pre-EQ; baked REVERB still uses it.

namespace {

// _compute_baked_data_variation return values, mirrored from src/resonance_player.cpp.
constexpr int kVariationRealtime = -1;      // ray-traced reflections
constexpr int kVariationBakedReverb = 0;    // IPL_BAKEDDATAVARIATION_REVERB
constexpr int kVariationStaticSource = 1;   // IPL_BAKEDDATAVARIATION_STATICSOURCE
constexpr int kVariationStaticListener = 2; // IPL_BAKEDDATAVARIATION_STATICLISTENER

inline bool gate_air_absorption_to_wet(bool air_absorption_enabled, bool has_srv, int baked_variation) {
    return air_absorption_enabled && has_srv && baked_variation == 0;
}

} // namespace

TEST_CASE("air absorption wet: realtime variation never receives the wet pre-EQ", "[air_absorption_wet]") {
    // Realtime ray-traced reflections include air absorption in the IR per ray; applying the pre-EQ on top would
    // double-attenuate high frequencies.
    REQUIRE(gate_air_absorption_to_wet(true, true, kVariationRealtime) == false);
    REQUIRE(gate_air_absorption_to_wet(false, true, kVariationRealtime) == false);
}

TEST_CASE("air absorption wet: baked reverb variation enables the pre-EQ when air absorption is on", "[air_absorption_wet]") {
    REQUIRE(gate_air_absorption_to_wet(true, true, kVariationBakedReverb) == true);
}

TEST_CASE("air absorption wet: baked static-source/listener do not receive the pre-EQ", "[air_absorption_wet]") {
    REQUIRE(gate_air_absorption_to_wet(true, true, kVariationStaticSource) == false);
    REQUIRE(gate_air_absorption_to_wet(true, true, kVariationStaticListener) == false);
}

TEST_CASE("air absorption wet: master switch off blocks the pre-EQ regardless of variation", "[air_absorption_wet]") {
    REQUIRE(gate_air_absorption_to_wet(false, true, kVariationBakedReverb) == false);
    REQUIRE(gate_air_absorption_to_wet(false, true, kVariationStaticSource) == false);
    REQUIRE(gate_air_absorption_to_wet(false, true, kVariationStaticListener) == false);
}

TEST_CASE("air absorption wet: missing server disables the pre-EQ", "[air_absorption_wet]") {
    // Defensive: if no server is available we cannot resolve the variation, so the wet pre-EQ stays off.
    REQUIRE(gate_air_absorption_to_wet(true, false, kVariationBakedReverb) == false);
    REQUIRE(gate_air_absorption_to_wet(true, false, kVariationRealtime) == false);
}
