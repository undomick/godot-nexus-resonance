#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_attenuation_callback_policy.h"

using namespace resonance;

TEST_CASE("attenuation callback data equality", "[sources][attenuation]") {
    const float curve_a[3] = {1.0f, 0.5f, 0.0f};
    const float curve_b[3] = {1.0f, 0.5f, 0.0f};
    REQUIRE(attenuation_callback_data_equal(2, 1.0f, 10.0f, curve_a, 3, 2, 1.0f, 10.0f, curve_b, 3));
    REQUIRE_FALSE(attenuation_callback_data_equal(2, 1.0f, 10.0f, curve_a, 3, 1, 1.0f, 10.0f, curve_b, 3));
}
