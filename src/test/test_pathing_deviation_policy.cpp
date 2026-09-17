#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_pathing_deviation_policy.h"

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

TEST_CASE("pathing deviation LUT index at bounds") {
    REQUIRE(resonance::pathing_deviation_angle_to_lut_index(0.0f, 64) == 0);
    REQUIRE(resonance::pathing_deviation_angle_to_lut_index(kPi, 64) == 63);
}

TEST_CASE("pathing deviation LUT lookup clamps output") {
    const float lut[] = {0.0f, 0.5f, 2.0f};
    REQUIRE(resonance::pathing_deviation_lut_lookup(lut, 3, 0.0f) == 0.0f);
    REQUIRE(resonance::pathing_deviation_lut_lookup(lut, 3, kPi) == 1.0f);
}

TEST_CASE("pathing deviation band clamp") {
    REQUIRE(resonance::pathing_deviation_band_clamped(-1) == 0);
    REQUIRE(resonance::pathing_deviation_band_clamped(99) == 2);
}
