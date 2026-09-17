#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_listener_attenuation_policy.h"

using namespace resonance;

TEST_CASE("playback syncs viewport listener before attenuation when server ready", "[listener][attenuation]") {
    REQUIRE(playback_should_sync_viewport_listener_before_attenuation(true));
    REQUIRE_FALSE(playback_should_sync_viewport_listener_before_attenuation(false));
}
