#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_pathing_inputs_policy.h"

// Baked REVERB is always listener-centric. Gate mirrors
// ResonanceServer::_maybe_apply_baked_reverb_listener_reflection_inputs.

namespace {

constexpr int kVariationRealtime = -1;
constexpr int kVariationBakedReverb = 0;
constexpr int kVariationStaticSource = 1;
constexpr int kVariationStaticListener = 2;

constexpr int kDirectFlag = 1 << 0;
constexpr int kReflectionsFlag = 1 << 1;

inline bool gate_listener_probe_setinputs(int baked_variation, bool enable_reflections, bool reflections_in_sim_flags,
                                          bool listener_valid) {
    return baked_variation == kVariationBakedReverb && enable_reflections && reflections_in_sim_flags && listener_valid;
}

} // namespace

TEST_CASE("baked reverb listener probe: only IPL_BAKEDDATAVARIATION_REVERB triggers the second SetInputs",
          "[baked_reverb_listener_probe]") {
    REQUIRE(gate_listener_probe_setinputs(kVariationRealtime, true, true, true) == false);
    REQUIRE(gate_listener_probe_setinputs(kVariationStaticSource, true, true, true) == false);
    REQUIRE(gate_listener_probe_setinputs(kVariationStaticListener, true, true, true) == false);
    REQUIRE(gate_listener_probe_setinputs(kVariationBakedReverb, true, true, true) == true);
}

TEST_CASE("baked reverb listener probe: skipped when reflections are disabled", "[baked_reverb_listener_probe]") {
    REQUIRE(gate_listener_probe_setinputs(kVariationBakedReverb, /*enable_reflections*/ false, true, true) == false);
    REQUIRE(gate_listener_probe_setinputs(kVariationBakedReverb, true, /*reflections_in_sim_flags*/ false, true) == false);
}

TEST_CASE("baked reverb listener probe: skipped when listener is invalid", "[baked_reverb_listener_probe]") {
    REQUIRE(gate_listener_probe_setinputs(kVariationBakedReverb, true, true, /*listener_valid*/ false) == false);
}

TEST_CASE("baked reverb: Direct SetInputs selector never includes REFLECTIONS", "[baked_reverb_listener_probe][direct]") {
    // Game-source Direct slot stays DIRECT-only even when baked REVERB is active;
    // listener-centric Reflections use a separate SetInputs (helper).
    (void)kVariationBakedReverb;
    const int direct_selector = resonance::source_direct_set_inputs_selector_flags(kDirectFlag);
    REQUIRE(direct_selector == kDirectFlag);
    REQUIRE((direct_selector & kReflectionsFlag) == 0);
}
