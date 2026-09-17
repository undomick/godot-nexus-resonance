#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_playback_host_fade_policy.h"

using namespace resonance;

TEST_CASE("host fade-out preserved during soft-stop EOS partial dry", "[playback][host-fade]") {
    REQUIRE_FALSE(playback_host_fade_out_should_clear_stale(true, false, 64, false));
}

TEST_CASE("host fade-out clears on live playback without stop request", "[playback][host-fade]") {
    REQUIRE(playback_host_fade_out_should_clear_stale(false, true, 64, false));
}

TEST_CASE("host fade-out clears on EOS partial dry without stop request", "[playback][host-fade]") {
    REQUIRE(playback_host_fade_out_should_clear_stale(false, false, 64, false));
}
