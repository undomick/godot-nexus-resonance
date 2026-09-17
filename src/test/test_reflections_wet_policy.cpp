#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_reflections_wet_policy.h"

using namespace resonance;

TEST_CASE("reflections wet disable resets prev mix levels for ramp-from-silence re-enable", "[playback][wet][reverb]") {
    float prev_conv = 0.75f;
    float prev_parametric = 0.5f;
    reflections_wet_reset_prev_levels_on_disable(prev_conv, prev_parametric);
    REQUIRE(prev_conv == 0.0f);
    REQUIRE(prev_parametric == 0.0f);
}
