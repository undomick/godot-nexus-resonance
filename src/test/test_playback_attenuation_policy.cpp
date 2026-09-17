#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_playback_attenuation_policy.h"

using namespace resonance;

TEST_CASE("inverse distance attenuation matches IPL rolloff", "[playback][attenuation][path-h02]") {
    REQUIRE(inverse_distance_attenuation(0.5f, 1.0f) == Approx(1.0f));
    REQUIRE(inverse_distance_attenuation(1.0f, 1.0f) == Approx(1.0f));
    REQUIRE(inverse_distance_attenuation(2.0f, 1.0f) == Approx(0.5f));
    REQUIRE(inverse_distance_attenuation(10.0f, 2.0f) == Approx(0.2f));
}

TEST_CASE("playback inverse ignores stale sim cache", "[playback][attenuation][path-h02]") {
    const float stale_sim = 1.0f;
    REQUIRE(playback_inverse_distance_attenuation(4.0f, 1.0f, stale_sim) == Approx(0.25f));
    REQUIRE(playback_inverse_distance_attenuation(4.0f, 1.0f, stale_sim) != Approx(stale_sim));
}

// Steam: path SH already carries DA (LOS Euclidean vs occluded probe-path length). Mix level is
// pathingMixLevel ramp only - never Direct playback attenuation.
TEST_CASE("pathing wet does not multiply playback distance attenuation", "[pathing][wet][steam]") {
    REQUIRE_FALSE(pathing_wet_uses_playback_distance_attenuation());
    REQUIRE(pathing_wet_playback_mix_level(0.75f) == Approx(0.75f));
    const float att = 0.2f;
    REQUIRE(pathing_wet_playback_mix_level(1.0f) == Approx(1.0f));
    REQUIRE(pathing_wet_playback_mix_level(1.0f) != Approx(att));
}
