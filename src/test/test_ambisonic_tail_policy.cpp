#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_ambisonic_tail_policy.h"
#include <phonon.h>

using namespace resonance;

TEST_CASE("ambisonic tail: effect tail produced until TAILCOMPLETE", "[ambisonic][eos][tail]") {
    REQUIRE(ambisonic_effect_tail_produced(IPL_AUDIOEFFECTSTATE_TAILREMAINING));
    REQUIRE_FALSE(ambisonic_effect_tail_produced(IPL_AUDIOEFFECTSTATE_TAILCOMPLETE));
    REQUIRE(ambisonic_decode_tail_produced(IPL_AUDIOEFFECTSTATE_TAILREMAINING));
    REQUIRE_FALSE(ambisonic_decode_tail_produced(IPL_AUDIOEFFECTSTATE_TAILCOMPLETE));
}

TEST_CASE("ambisonic tail: rotation residual must pass through decode Apply before stereo EOS", "[ambisonic][eos][tail]") {
    REQUIRE(ambisonic_rotation_tail_needs_decode_apply(IPL_AUDIOEFFECTSTATE_TAILREMAINING));
    REQUIRE_FALSE(ambisonic_rotation_tail_needs_decode_apply(IPL_AUDIOEFFECTSTATE_TAILCOMPLETE));
}

TEST_CASE("ambisonic tail: stop holds while rings or IPL tails remain", "[ambisonic][eos][tail]") {
    REQUIRE(ambisonic_stop_has_pending_output(true, false, 0, 0));
    REQUIRE(ambisonic_stop_has_pending_output(false, true, 0, 0));
    REQUIRE(ambisonic_stop_has_pending_output(false, false, 0, 256));
    REQUIRE_FALSE(ambisonic_stop_has_pending_output(false, false, 0, 0));
}
