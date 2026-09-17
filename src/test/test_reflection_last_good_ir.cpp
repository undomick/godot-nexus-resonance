#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_reflection_fetch_policy.h"

TEST_CASE("last-good IR: used when convolution fetch returns null ir between RunReflections", "[reflection_fetch][r02]") {
    REQUIRE(resonance::reflection_worker_use_last_good_conv(false, resonance::kReflectionConvolution, true, false));
}

TEST_CASE("last-good IR: not used when fresh ir is present", "[reflection_fetch][r02]") {
    REQUIRE_FALSE(resonance::reflection_worker_use_last_good_conv(true, resonance::kReflectionConvolution, true, false));
}

TEST_CASE("last-good IR: not used without stored fallback", "[reflection_fetch][r02]") {
    REQUIRE_FALSE(resonance::reflection_worker_use_last_good_conv(false, resonance::kReflectionConvolution, false, false));
}

TEST_CASE("last-good IR: not used after RunReflections sync (invalidated IR)", "[reflection_fetch][r02]") {
    REQUIRE_FALSE(resonance::reflection_worker_use_last_good_conv(false, resonance::kReflectionConvolution, true, true));
}

TEST_CASE("last-good IR: not used for non-convolution modes", "[reflection_fetch][r02]") {
    REQUIRE_FALSE(resonance::reflection_worker_use_last_good_conv(false, resonance::kReflectionParametric, true, false));
}
