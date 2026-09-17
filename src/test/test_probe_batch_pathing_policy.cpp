#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_probe_batch_pathing_policy.h"

using namespace resonance;

TEST_CASE("handle pathing follows IPL layer not hash", "[probe_batch][pathing]") {
    REQUIRE(resolve_handle_has_pathing_from_load(true, 0));
    REQUIRE(resolve_handle_has_pathing_from_load(true, 42));
    REQUIRE_FALSE(resolve_handle_has_pathing_from_load(false, 0));
    REQUIRE_FALSE(resolve_handle_has_pathing_from_load(false, 42));
}

TEST_CASE("warn when hash claims pathing but IPL layer missing", "[probe_batch][pathing]") {
    REQUIRE_FALSE(should_warn_pathing_hash_without_layer(true, 10));
    REQUIRE_FALSE(should_warn_pathing_hash_without_layer(false, 0));
    REQUIRE(should_warn_pathing_hash_without_layer(false, 10));
}
