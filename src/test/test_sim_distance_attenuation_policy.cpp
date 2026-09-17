#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_playback_attenuation_policy.h"
#include "../resonance_sim_distance_attenuation_policy.h"

using namespace resonance;

TEST_CASE("direct sim DA only for Inverse when distance_attenuation on", "[sim][attenuation]") {
    REQUIRE(sim_distance_model_for_direct(true, kAttenuationModeInverse) == SimDistanceModelKind::InverseDistance);
    REQUIRE(sim_direct_distance_attenuation_flag(true, kAttenuationModeInverse));
    REQUIRE(sim_distance_model_for_direct(true, kAttenuationModeLinear) == SimDistanceModelKind::Default);
    REQUIRE_FALSE(sim_direct_distance_attenuation_flag(true, kAttenuationModeLinear));
    REQUIRE(sim_distance_model_for_direct(false, kAttenuationModeInverse) == SimDistanceModelKind::Default);
    REQUIRE(sim_distance_model_for_direct(true, kAttenuationModeDisabled) == SimDistanceModelKind::Default);
}

TEST_CASE("pathing Linear/Curve uses Callback; Inverse uses InverseDistance", "[sim][attenuation][pathing]") {
    REQUIRE(sim_distance_model_for_pathing(true, kAttenuationModeInverse) == SimDistanceModelKind::InverseDistance);
    REQUIRE(sim_distance_model_for_pathing(true, kAttenuationModeLinear) == SimDistanceModelKind::Callback);
    REQUIRE(sim_distance_model_for_pathing(true, kAttenuationModeCurve) == SimDistanceModelKind::Callback);
    REQUIRE(sim_distance_model_for_pathing(true, kAttenuationModeDisabled) == SimDistanceModelKind::Default);
    REQUIRE(sim_distance_model_for_pathing(false, kAttenuationModeLinear) == SimDistanceModelKind::Default);
}

TEST_CASE("reflections Callback only when use_distance_curve_for_reflections", "[sim][attenuation][reflections]") {
    REQUIRE(sim_distance_model_for_reflections(true, kAttenuationModeLinear, false) == SimDistanceModelKind::Default);
    REQUIRE(sim_distance_model_for_reflections(true, kAttenuationModeLinear, true) == SimDistanceModelKind::Callback);
    REQUIRE(sim_distance_model_for_reflections(true, kAttenuationModeCurve, true) == SimDistanceModelKind::Callback);
    REQUIRE(sim_distance_model_for_reflections(true, kAttenuationModeInverse, true) ==
            SimDistanceModelKind::InverseDistance);
    REQUIRE(sim_distance_model_for_reflections(true, kAttenuationModeInverse, false) ==
            SimDistanceModelKind::InverseDistance);
}

TEST_CASE("wet SetInputs split when reflections and pathing models differ", "[sim][attenuation]") {
    const auto path = sim_distance_model_for_pathing(true, kAttenuationModeLinear);
    const auto refl_off = sim_distance_model_for_reflections(true, kAttenuationModeLinear, false);
    const auto refl_on = sim_distance_model_for_reflections(true, kAttenuationModeLinear, true);
    REQUIRE(sim_distance_models_need_split_wet_set_inputs(refl_off, path));
    REQUIRE_FALSE(sim_distance_models_need_split_wet_set_inputs(refl_on, path));
}

TEST_CASE("Inverse ignores max_distance; Linear uses it", "[sim][attenuation][inspector]") {
    REQUIRE_FALSE(attenuation_mode_uses_max_distance(true, kAttenuationModeInverse));
    REQUIRE(attenuation_mode_uses_max_distance(true, kAttenuationModeLinear));
    REQUIRE(attenuation_mode_uses_max_distance(true, kAttenuationModeCurve));
    REQUIRE_FALSE(attenuation_mode_uses_max_distance(true, kAttenuationModeDisabled));
    REQUIRE_FALSE(use_distance_curve_for_reflections_editable(true, kAttenuationModeInverse));
    REQUIRE(use_distance_curve_for_reflections_editable(true, kAttenuationModeLinear));
}

TEST_CASE("pathing wet never multiplies Direct playback attenuation", "[pathing][wet][steam]") {
    REQUIRE_FALSE(pathing_wet_uses_playback_distance_attenuation());
}

TEST_CASE("Inverse playback ignores max (Physics Based)", "[playback][attenuation]") {
    // min/d with no max cutoff: far sources stay quiet but non-zero.
    REQUIRE(inverse_distance_attenuation(500.0f, 1.0f) == Approx(0.002f));
    REQUIRE(inverse_distance_attenuation(50.0f, 1.0f) == Approx(0.02f));
    REQUIRE(inverse_distance_attenuation(500.0f, 1.0f) != Approx(0.0f));
}
