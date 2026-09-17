#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_pathing_inputs_policy.h"
#include <cstdint>

using namespace resonance;

// Mirrors IPL simulation flags without pulling phonon.h into this TU.
constexpr int kDirectFlag = 1 << 0;
constexpr int kReflectionsFlag = 1 << 1;
constexpr int kPathingFlag = 1 << 2;

TEST_CASE("direct SetInputs selector is always DIRECT alone", "[pathing_inputs][direct]") {
    REQUIRE(source_direct_set_inputs_selector_flags(kDirectFlag) == kDirectFlag);
    REQUIRE(source_direct_sim_flags_mask(kDirectFlag) == kDirectFlag);
    // Direct selector must never OR PATHING or REFLECTIONS.
    REQUIRE((source_direct_set_inputs_selector_flags(kDirectFlag) & kPathingFlag) == 0);
    REQUIRE((source_direct_set_inputs_selector_flags(kDirectFlag) & kReflectionsFlag) == 0);
}

TEST_CASE("wet SetInputs selector includes PATHING when globally enabled", "[pathing_inputs]") {
    REQUIRE(source_set_inputs_selector_flags(0, kPathingFlag, true) == kPathingFlag);
    REQUIRE(source_set_inputs_selector_flags(kReflectionsFlag, kPathingFlag, true) == (kReflectionsFlag | kPathingFlag));
    REQUIRE(source_set_inputs_selector_flags(kPathingFlag, kPathingFlag, true) == kPathingFlag);
}

TEST_CASE("wet SetInputs selector unchanged when pathing globally disabled", "[pathing_inputs]") {
    REQUIRE(source_set_inputs_selector_flags(0, kPathingFlag, false) == 0);
    REQUIRE(source_set_inputs_selector_flags(kReflectionsFlag, kPathingFlag, false) == kReflectionsFlag);
}

TEST_CASE("wet SetInputs needed when wet bits set or pathing globally on", "[pathing_inputs]") {
    REQUIRE(source_wet_set_inputs_needed(kReflectionsFlag, false));
    REQUIRE(source_wet_set_inputs_needed(kPathingFlag, false));
    REQUIRE(source_wet_set_inputs_needed(0, true));
    REQUIRE_FALSE(source_wet_set_inputs_needed(0, false));
}

TEST_CASE("batch remove clears explicit and fallback pathing assignments", "[pathing_inputs]") {
    // Preferred matches removing batch.
    REQUIRE(source_should_clear_pathing_on_batch_remove(3, 3, true));
    // Explicit "any batch" preference always clears.
    REQUIRE(source_should_clear_pathing_on_batch_remove(-1, 3, false));
    // Preferred still usable: bound to preferred, not the unrelated removing batch.
    REQUIRE_FALSE(source_should_clear_pathing_on_batch_remove(2, 3, true));
    // Preferred stale/non-pathing: get_pathing_batch may have bound removing via fallback.
    REQUIRE(source_should_clear_pathing_on_batch_remove(2, 3, false));
    REQUIRE_FALSE(source_should_clear_pathing_on_batch_remove(3, -1, true));
}

TEST_CASE("pathing run requires simulator created with PATHING", "[pathing_inputs]") {
    REQUIRE(simulator_supports_pathing_run(true, true));
    REQUIRE_FALSE(simulator_supports_pathing_run(true, false));
    REQUIRE_FALSE(simulator_supports_pathing_run(false, true));
    REQUIRE_FALSE(simulator_supports_pathing_run(false, false));
}

TEST_CASE("enabling pathing without PATHING create requires recreate", "[pathing_inputs]") {
    // Legacy simulator built without PATHING still needs recreate to enable.
    REQUIRE(pathing_enable_requires_simulator_recreate(true, false));
    // Init always creates with PATHING: live enable never reinits.
    REQUIRE_FALSE(pathing_enable_requires_simulator_recreate(true, true));
    REQUIRE_FALSE(pathing_enable_requires_simulator_recreate(false, false));
    REQUIRE_FALSE(pathing_enable_requires_simulator_recreate(false, true));
}

TEST_CASE("create with PATHING allows live enable without recreate", "[pathing_inputs]") {
    constexpr bool kCreatedWithPathing = true;
    REQUIRE(simulator_supports_pathing_run(true, kCreatedWithPathing));
    REQUIRE_FALSE(pathing_enable_requires_simulator_recreate(true, kCreatedWithPathing));
    // Disable stays live either way.
    REQUIRE_FALSE(pathing_enable_requires_simulator_recreate(false, kCreatedWithPathing));
}

TEST_CASE("pathing apply order clamps to Ambisonic 0..3", "[pathing_inputs]") {
    REQUIRE(pathing_apply_order(2) == 2);
    REQUIRE(pathing_apply_order(0) == 0);
    REQUIRE(pathing_apply_order(99) == 3);
    REQUIRE(pathing_apply_order(-1) == 0);
    // Configured order 2 must win over zero-init GetOutputs order (never written by Steam Audio).
    REQUIRE(pathing_apply_order(2) != 0);
    REQUIRE(pathing_sh_coeff_count(pathing_apply_order(2)) == 9);
}

TEST_CASE("pathing SH coeff count is (order+1)^2", "[pathing_inputs]") {
    REQUIRE(pathing_sh_coeff_count(0) == 1);
    REQUIRE(pathing_sh_coeff_count(1) == 4);
    REQUIRE(pathing_sh_coeff_count(2) == 9);
    REQUIRE(pathing_sh_coeff_count(3) == 16);
    REQUIRE(pathing_sh_coeff_count(-1) == 0);
}
