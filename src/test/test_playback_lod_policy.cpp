#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_playback_lod_policy.h"

using namespace resonance;

TEST_CASE("playback LOD pushes coeff refresh when full params skipped", "[playback][lod]") {
    REQUIRE(playback_lod_should_push_coeff_refresh(false));
    REQUIRE_FALSE(playback_lod_should_push_coeff_refresh(true));
}

TEST_CASE("playback LOD coeff refresh includes position and attenuation", "[playback][lod]") {
    REQUIRE(playback_lod_coeff_refresh_includes_position_attenuation());
}

TEST_CASE("playback LOD throttles the full push while nothing forces it", "[playback][lod]") {
    REQUIRE_FALSE(playback_should_apply_full_params(false, false, false));
    REQUIRE(playback_should_apply_full_params(true, false, false));
    REQUIRE(playback_should_apply_full_params(false, true, false));
}

TEST_CASE("playback LOD forces a full push when the voice wakes or goes virtual", "[playback][lod][virtualization]") {
    REQUIRE(playback_should_apply_full_params(false, false, true));
}
