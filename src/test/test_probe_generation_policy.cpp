#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_probe_generation_policy.h"

using namespace resonance;

TEST_CASE("uniform floor finds no floor in a volume lower than the probe height", "[probe][generation]") {
    REQUIRE_FALSE(uniform_floor_fits_volume(1.3f, 1.5f));
    REQUIRE_FALSE(uniform_floor_fits_volume(1.5f, 1.5f));
}

TEST_CASE("uniform floor fits a volume taller than the probe height", "[probe][generation]") {
    REQUIRE(uniform_floor_fits_volume(4.0f, 1.5f));
    REQUIRE(uniform_floor_fits_volume(1.3f, 0.5f));
}
