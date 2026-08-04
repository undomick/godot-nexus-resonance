#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_probe_exclusion_math.h"

TEST_CASE("point_in_obb_local accepts interior and rejects exterior") {
    REQUIRE(resonance::point_in_obb_local(0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f));
    REQUIRE(resonance::point_in_obb_local(1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f));
    REQUIRE_FALSE(resonance::point_in_obb_local(1.1f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f));
    REQUIRE_FALSE(resonance::point_in_obb_local(0.0f, -2.0f, 0.0f, 1.0f, 1.0f, 1.0f));
}

TEST_CASE("point_in_obb_local respects per-axis half extents") {
    REQUIRE(resonance::point_in_obb_local(2.0f, 0.0f, 0.0f, 2.0f, 0.5f, 0.5f));
    REQUIRE_FALSE(resonance::point_in_obb_local(0.0f, 0.6f, 0.0f, 2.0f, 0.5f, 0.5f));
}
