#include "../resonance_reflection_ir_fingerprint.h"

#include "../lib/catch2/single_include/catch2/catch.hpp"

using namespace godot;

TEST_CASE("baked energy q16: zero for invalid input", "[reflection_ir_fingerprint]") {
    REQUIRE(reflection_baked_energy_to_q16(0.0f) == 0);
    REQUIRE(reflection_baked_energy_to_q16(-1.0f) == 0);
}

TEST_CASE("baked energy q16: monotonic for positive energy", "[reflection_ir_fingerprint]") {
    const uint16_t low = reflection_baked_energy_to_q16(0.01f);
    const uint16_t high = reflection_baked_energy_to_q16(10.0f);
    REQUIRE(high > low);
}

TEST_CASE("conv ir fir energy: null ir returns zero", "[reflection_ir_fingerprint]") {
    REQUIRE(reflection_conv_ir_fir_energy_q16(nullptr, 96000, 9) == 0);
}
