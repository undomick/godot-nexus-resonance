#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_source_handle_policy.h"
#include <cstdint>

using namespace resonance;

TEST_CASE("source handle requires matching lifecycle epoch", "[source_handle]") {
    REQUIRE(source_handle_matches_lifecycle_epoch(0, 1u, 1u));
    REQUIRE(source_handle_matches_lifecycle_epoch(3, 7u, 7u));
    REQUIRE_FALSE(source_handle_matches_lifecycle_epoch(-1, 1u, 1u));
    REQUIRE_FALSE(source_handle_matches_lifecycle_epoch(0, 1u, 2u));
    REQUIRE_FALSE(source_handle_matches_lifecycle_epoch(0, 0u, 1u));
}

TEST_CASE("probe batch handle requires matching lifecycle epoch", "[source_handle]") {
    // Same recycle rules as sources: after reinit, stale preferred IDs must not remove_batch.
    REQUIRE(probe_batch_handle_matches_lifecycle_epoch(0, 1u, 1u));
    REQUIRE_FALSE(probe_batch_handle_matches_lifecycle_epoch(0, 1u, 2u));
    REQUIRE_FALSE(probe_batch_handle_matches_lifecycle_epoch(-1, 1u, 1u));
    REQUIRE_FALSE(probe_batch_handle_matches_lifecycle_epoch(2, 0u, 1u));
}

TEST_CASE("lifecycle epoch never wraps to zero", "[source_handle]") {
    REQUIRE(next_source_lifecycle_epoch(1u) == 2u);
    REQUIRE(next_source_lifecycle_epoch(UINT32_MAX) == 1u);
}
