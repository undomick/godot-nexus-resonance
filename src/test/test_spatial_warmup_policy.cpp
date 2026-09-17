#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_spatial_warmup_policy.h"

using namespace resonance;

TEST_CASE("spatial warmup decrements on RunDirect or scene commit", "[constants][warmup]") {
    REQUIRE(spatial_warmup_should_decrement(true, false));
    REQUIRE(spatial_warmup_should_decrement(false, true));
    REQUIRE(spatial_warmup_should_decrement(true, true));
    REQUIRE_FALSE(spatial_warmup_should_decrement(false, false));
}
