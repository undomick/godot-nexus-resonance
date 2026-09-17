#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_attenuation_dirty_policy.h"

using namespace resonance;

TEST_CASE("attenuation callback model dirty follows curve change flag", "[sources][attenuation]") {
    REQUIRE(attenuation_callback_model_is_dirty(true));
    REQUIRE_FALSE(attenuation_callback_model_is_dirty(false));
}
