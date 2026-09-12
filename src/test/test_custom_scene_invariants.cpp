#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_constants.h"
#include "../resonance_physics_ray_math.h"
#include <cmath>

namespace {

// Mirrors Steam Audio direct_effect.cpp: overallGain *= occlusion + (1 - occlusion) * averageTransmissionFactor
float steam_direct_occlusion_transmission_gain(float occlusion, float trans_l, float trans_m, float trans_h) {
    const float t_avg = (trans_l + trans_m + trans_h) / 3.0f;
    return occlusion + (1.0f - occlusion) * t_avg;
}

} // namespace

TEST_CASE("Occlusion fetch default is visible (Steam LOS = 1)", "[custom_scene][occlusion]") {
    REQUIRE(resonance::kOcclusionFetchDefaultVisible == 1.0f);
}

TEST_CASE("Custom scene occlusion ray epsilon matches constant", "[custom_scene][ray]") {
    REQUIRE(resonance::kCustomSceneOcclusionRayStartEpsilon == 1e-2f);
}

TEST_CASE("custom_scene_occlusion_ray_start_t nudges open segment", "[custom_scene][ray]") {
    using resonance::custom_scene_occlusion_ray_start_t;
    const float eps = resonance::kCustomSceneOcclusionRayStartEpsilon;
    REQUIRE(custom_scene_occlusion_ray_start_t(0.0f, 1.0f) == Approx(eps).margin(1e-7f));
    REQUIRE(custom_scene_occlusion_ray_start_t(0.0f, 0.5f * eps) == Approx(0.0f).margin(1e-7f));
    REQUIRE(custom_scene_occlusion_ray_start_t(0.5f, 0.52f) == Approx(0.5f + eps).margin(1e-7f));
    REQUIRE(custom_scene_occlusion_ray_start_t(1.0f, 1.0f) == Approx(1.0f).margin(1e-7f));
    REQUIRE(custom_scene_occlusion_ray_start_t(2.0f, 1.0f) == Approx(2.0f).margin(1e-7f));
}

TEST_CASE("custom_scene closest-hit hit_from_inside stays false (transmission march)", "[custom_scene][ray]") {
    // If this fails, do not flip it back to true without reading
    // resonance_physics_ray_math.h: Steam Audio's DirectSimulator::transmission
    // advances min_distance past each hit (~1 cm), so closest-hit segments
    // start inside the surface just crossed; inside-start hits at ~0 distance
    // make the march re-accumulate the same collider until transmission
    // collapses to ~0 (all occluders fully opaque; the original bug 1).
    REQUIRE_FALSE(resonance::custom_scene_closest_hit_from_inside());
}

TEST_CASE("custom_scene any-hit hit_from_inside stays false (occlusion)", "[custom_scene][ray]") {
    // Any-hit occlusion rays start at the listener (or volumetric source
    // samples); a listener inside a collider must not occlude every ray.
    REQUIRE_FALSE(resonance::custom_scene_any_hit_from_inside());
}

TEST_CASE("Steam direct gain: full occlusion with low transmission damps strongly", "[custom_scene][steam_ref]") {
    const float g = steam_direct_occlusion_transmission_gain(0.0f, 0.1f, 0.05f, 0.03f);
    REQUIRE(g == Approx((0.1f + 0.05f + 0.03f) / 3.0f).margin(1e-6f));
    REQUIRE(g < 0.11f);
}

TEST_CASE("Steam direct gain: line of sight ignores transmission", "[custom_scene][steam_ref]") {
    const float g = steam_direct_occlusion_transmission_gain(1.0f, 0.1f, 0.05f, 0.03f);
    REQUIRE(g == Approx(1.0f).margin(1e-6f));
}

TEST_CASE("Steam direct gain: partial occlusion blends", "[custom_scene][steam_ref]") {
    const float g = steam_direct_occlusion_transmission_gain(0.5f, 1.0f, 1.0f, 1.0f);
    REQUIRE(g == Approx(1.0f).margin(1e-6f));
}
