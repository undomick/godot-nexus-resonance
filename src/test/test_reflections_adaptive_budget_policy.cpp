#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_reflections_adaptive_budget_policy.h"

using namespace resonance;

namespace {

AdaptiveRayLimits default_ray_limits() {
    AdaptiveRayLimits limits;
    limits.max_rays = 1024;
    limits.min_rays = 128;
    limits.recover_frac = 0.125f;
    limits.recover_cap = 512;
    return limits;
}

AdaptiveIntervalLimits default_interval_limits() {
    AdaptiveIntervalLimits limits;
    limits.step_sec = 0.02f;
    limits.max_extra_sec = 0.2f;
    limits.decay_per_sec = 0.05f;
    return limits;
}

} // namespace

TEST_CASE("Over-budget pass scales rays by budget over cost", "[reflections][adaptive]") {
    REQUIRE(adaptive_next_num_rays(1024, 2000, 1000, default_ray_limits()) == 512);
}

TEST_CASE("Far over-budget pass clamps rays to the minimum", "[reflections][adaptive]") {
    REQUIRE(adaptive_next_num_rays(1024, 32000, 1000, default_ray_limits()) == 128);
}

TEST_CASE("Minimum rays never exceed max rays", "[reflections][adaptive]") {
    AdaptiveRayLimits limits = default_ray_limits();
    limits.max_rays = 64;
    REQUIRE(adaptive_min_num_rays(limits) == 64);
    REQUIRE(adaptive_next_num_rays(64, 32000, 1000, limits) == 64);
}

TEST_CASE("Under-budget pass recovers rays by one capped step", "[reflections][adaptive]") {
    REQUIRE(adaptive_next_num_rays(128, 500, 1000, default_ray_limits()) == 256);
    REQUIRE(adaptive_next_num_rays(1000, 500, 1000, default_ray_limits()) == 1024);

    AdaptiveRayLimits limits = default_ray_limits();
    limits.recover_frac = 1.0f;
    REQUIRE(adaptive_next_num_rays(128, 500, 1000, limits) == 640);
}

TEST_CASE("Interval grows once per slow pass when rays are at the floor", "[reflections][adaptive]") {
    const AdaptiveIntervalLimits limits = default_interval_limits();
    const float grown = adaptive_next_extra_interval(0.0f, true, true, true, 0.016f, limits);
    REQUIRE(grown == Approx(0.02f));
    REQUIRE(adaptive_next_extra_interval(grown, true, false, true, 0.016f, limits) == Approx(0.02f));
}

TEST_CASE("Interval holds while fewer rays can still absorb the cost", "[reflections][adaptive]") {
    REQUIRE(adaptive_next_extra_interval(0.05f, true, true, false, 0.016f, default_interval_limits()) == Approx(0.05f));
}

TEST_CASE("Interval growth stops at the maximum", "[reflections][adaptive]") {
    REQUIRE(adaptive_next_extra_interval(0.19f, true, true, true, 0.016f, default_interval_limits()) == Approx(0.2f));
}

TEST_CASE("Interval decays after an under-budget pass", "[reflections][adaptive]") {
    const AdaptiveIntervalLimits limits = default_interval_limits();
    REQUIRE(adaptive_next_extra_interval(0.1f, false, false, true, 1.0f, limits) == Approx(0.05f));
    REQUIRE(adaptive_next_extra_interval(0.01f, false, true, true, 1.0f, limits) == Approx(0.0f));
}
