#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_probe_interpolation_policy.h"
#include <vector>

TEST_CASE("probe interpolation: inverse distance squared weights") {
    const float xyz[] = {0.0f, 0.0f, 0.0f, 3.0f, 0.0f, 0.0f};
    std::vector<resonance::ProbeNeighborWeight> neighbors;
    float sum = 0.0f;
    resonance::probe_neighbors_inverse_distance_squared(xyz, 2, 0.0f, 0.0f, 0.0f, 5.0f, neighbors, sum);
    REQUIRE(neighbors.size() == 2);
    REQUIRE(sum > 0.0f);
    REQUIRE(neighbors[0].index == 0);
    REQUIRE(neighbors[0].weight > neighbors[1].weight);
}

TEST_CASE("probe interpolation: empty when radius too small") {
    const float xyz[] = {10.0f, 0.0f, 0.0f};
    std::vector<resonance::ProbeNeighborWeight> neighbors;
    float sum = 1.0f;
    resonance::probe_neighbors_inverse_distance_squared(xyz, 1, 0.0f, 0.0f, 0.0f, 1.0f, neighbors, sum);
    REQUIRE(neighbors.empty());
    REQUIRE(sum == 0.0f);
}
