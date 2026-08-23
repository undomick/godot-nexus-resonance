#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_constants.h"
#include "../resonance_reflection_type_policy.h"

#include <phonon.h>

using namespace resonance;

TEST_CASE("hybrid simulator type stays HYBRID", "[reflection][hybrid][parametric]") {
    REQUIRE(reflection_type_for_simulator(kReflectionHybrid) == IPL_REFLECTIONEFFECTTYPE_HYBRID);
}

TEST_CASE("hybrid effect params fall back to parametric without conv+parametric", "[reflection][hybrid][parametric]") {
    REQUIRE(reflection_effect_type_for_mode(kReflectionHybrid, false) == IPL_REFLECTIONEFFECTTYPE_PARAMETRIC);
    REQUIRE(reflection_effect_type_for_mode(kReflectionHybrid, true) == IPL_REFLECTIONEFFECTTYPE_HYBRID);
}

TEST_CASE("parametric mode maps to parametric for simulator and effect", "[reflection][parametric]") {
    REQUIRE(reflection_type_for_simulator(kReflectionParametric) == IPL_REFLECTIONEFFECTTYPE_PARAMETRIC);
    REQUIRE(reflection_effect_type_for_mode(kReflectionParametric, false) == IPL_REFLECTIONEFFECTTYPE_PARAMETRIC);
}

TEST_CASE("convolution and TAN mapping unchanged", "[reflection]") {
    REQUIRE(reflection_type_for_simulator(kReflectionConvolution) == IPL_REFLECTIONEFFECTTYPE_CONVOLUTION);
    REQUIRE(reflection_type_for_simulator(kReflectionTan) == IPL_REFLECTIONEFFECTTYPE_TAN);
    REQUIRE(reflection_effect_type_for_mode(kReflectionConvolution, false) == IPL_REFLECTIONEFFECTTYPE_CONVOLUTION);
    REQUIRE(reflection_effect_type_for_mode(kReflectionTan, false) == IPL_REFLECTIONEFFECTTYPE_TAN);
}

TEST_CASE("hybrid simulator vs effect divergence is intentional", "[reflection][hybrid]") {
    // Simulator always HYBRID; effect may be PARAMETRIC when IR or bands missing on fetch.
    REQUIRE(reflection_type_for_simulator(kReflectionHybrid) != reflection_effect_type_for_mode(kReflectionHybrid, false));
    REQUIRE(reflection_type_for_simulator(kReflectionHybrid) == reflection_effect_type_for_mode(kReflectionHybrid, true));
}
