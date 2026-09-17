#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_param_cache_invalidate_policy.h"

using namespace resonance;

TEST_CASE("param cache invalidation touches both double-buffer slots", "[cache][invalidate][prb-03]") {
    REQUIRE(kParamCacheSlotCount == 2);
    REQUIRE(param_cache_slot_is_valid(0));
    REQUIRE(param_cache_slot_is_valid(1));
    REQUIRE_FALSE(param_cache_slot_is_valid(-1));
    REQUIRE_FALSE(param_cache_slot_is_valid(2));
}
