#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_pathing_batch_lookup_policy.h"

using namespace resonance;

TEST_CASE("pathing batch lookup prefers explicit volume", "[pathing_batch_lookup]") {
    const auto hit = resolve_pathing_batch_lookup(2, true, true, 2, 1);
    REQUIRE(hit.outcome == PathingBatchLookupOutcome::PreferredHit);
    REQUIRE(hit.handle == 2);

    const auto invalid = resolve_pathing_batch_lookup(2, true, false, 5, 2);
    REQUIRE(invalid.outcome == PathingBatchLookupOutcome::PreferredInvalid);
    REQUIRE(invalid.handle == -1);
}

TEST_CASE("pathing batch lookup without explicit volume", "[pathing_batch_lookup]") {
    const auto none = resolve_pathing_batch_lookup(-1, false, false, -1, 0);
    REQUIRE(none.outcome == PathingBatchLookupOutcome::NoPathingBatch);

    const auto single = resolve_pathing_batch_lookup(-1, false, false, 3, 1);
    REQUIRE(single.outcome == PathingBatchLookupOutcome::SingleVolumeFallback);
    REQUIRE(single.handle == 3);

    const auto ambiguous = resolve_pathing_batch_lookup(-1, false, false, 3, 2);
    REQUIRE(ambiguous.outcome == PathingBatchLookupOutcome::AmbiguousMultiVolume);
    REQUIRE(ambiguous.handle == -1);
}
