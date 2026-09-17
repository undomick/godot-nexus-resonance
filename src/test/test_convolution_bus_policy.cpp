#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_convolution_bus_policy.h"

using namespace resonance;

TEST_CASE("convolution bus skips IPL apply when mixer never fed and no hold-last", "[convolution_bus][conv]") {
    REQUIRE(convolution_bus_skip_empty_mixer_apply(0, false));
    REQUIRE_FALSE(convolution_bus_skip_empty_mixer_apply(0, true));
    REQUIRE_FALSE(convolution_bus_skip_empty_mixer_apply(3, false));
}
