#include "../resonance_constants.h"

#include "../lib/catch2/single_include/catch2/catch.hpp"

// Worker fetch last-good-IR policy for convolution (see resonance_server_fetch.cpp).

namespace {

constexpr int kReflectionConvolution = 0;

bool should_use_last_good_conv(bool has_ir, int reflection_type, bool last_good_valid) {
    return !has_ir && reflection_type == kReflectionConvolution && last_good_valid;
}

} // namespace

TEST_CASE("last-good IR: used when convolution fetch returns null ir", "[reflection_fetch]") {
    REQUIRE(should_use_last_good_conv(false, kReflectionConvolution, true) == true);
}

TEST_CASE("last-good IR: not used when fresh ir is present", "[reflection_fetch]") {
    REQUIRE(should_use_last_good_conv(true, kReflectionConvolution, true) == false);
}

TEST_CASE("last-good IR: not used without stored fallback", "[reflection_fetch]") {
    REQUIRE(should_use_last_good_conv(false, kReflectionConvolution, false) == false);
}
