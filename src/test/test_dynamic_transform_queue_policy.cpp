#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_dynamic_transform_queue_policy.h"

using namespace resonance;

TEST_CASE("dynamic transform queue preserves force_apply on merge", "[geometry][dynamic]") {
    REQUIRE(dynamic_instanced_transform_force_apply_merge(false, false) == false);
    REQUIRE(dynamic_instanced_transform_force_apply_merge(true, false) == true);
    REQUIRE(dynamic_instanced_transform_force_apply_merge(false, true) == true);
    REQUIRE(dynamic_instanced_transform_force_apply_merge(true, true) == true);
}
