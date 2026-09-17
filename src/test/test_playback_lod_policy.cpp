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
