#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_reflection_cache_publish_policy.h"

TEST_CASE("entry epoch fresh only when non-zero and matches slot", "[reflection_cache_publish]") {
    REQUIRE(resonance::reflection_cache_entry_epoch_fresh(5u, 5u));
    REQUIRE_FALSE(resonance::reflection_cache_entry_epoch_fresh(5u, 4u));
    REQUIRE_FALSE(resonance::reflection_cache_entry_epoch_fresh(5u, 0u));
    REQUIRE_FALSE(resonance::reflection_cache_entry_epoch_fresh(0u, 0u));
}
