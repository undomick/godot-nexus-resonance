#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_geometry_transform_coalesce_policy.h"

using namespace resonance;

TEST_CASE("geometry transform coalesce due every Nth tick per instance", "[geometry][coalesce]") {
    REQUIRE_FALSE(geometry_transform_coalesce_tick_due(1, 4));
    REQUIRE_FALSE(geometry_transform_coalesce_tick_due(2, 4));
    REQUIRE_FALSE(geometry_transform_coalesce_tick_due(3, 4));
    REQUIRE(geometry_transform_coalesce_tick_due(4, 4));
    REQUIRE(geometry_transform_coalesce_tick_due(8, 4));
}

TEST_CASE("geometry transform coalesce interval one always due", "[geometry][coalesce]") {
    REQUIRE(geometry_transform_coalesce_tick_due(1, 1));
    REQUIRE(geometry_transform_coalesce_tick_due(99, 0));
}
