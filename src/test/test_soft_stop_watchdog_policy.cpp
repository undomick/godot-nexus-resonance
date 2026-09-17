#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_soft_stop_watchdog_policy.h"

using namespace resonance;

TEST_CASE("soft-stop watchdog does not force-stop active drain before absolute cap", "[playback][soft-stop]") {
    REQUIRE_FALSE(soft_stop_watchdog_should_force_stop(2.0f, 2.4, 0.0));
    REQUIRE_FALSE(soft_stop_watchdog_should_force_stop(2.0f, 7.9, 0.1));
}

TEST_CASE("soft-stop watchdog forces stop on stalled mix after legacy cap", "[playback][soft-stop]") {
    REQUIRE(soft_stop_watchdog_should_force_stop(2.0f, 2.5, kSoftStopMixStallThresholdSec));
    REQUIRE_FALSE(soft_stop_watchdog_should_force_stop(2.0f, 2.4, kSoftStopMixStallThresholdSec));
}

TEST_CASE("soft-stop watchdog absolute cap applies even when mix is active", "[playback][soft-stop]") {
    const double active_cap = 2.0 * kSoftStopActiveDrainCapMultiplier + kSoftStopActiveDrainCapMarginSec;
    REQUIRE(soft_stop_watchdog_should_force_stop(2.0f, active_cap, 0.0));
    REQUIRE_FALSE(soft_stop_watchdog_should_force_stop(2.0f, active_cap - 0.01, 0.0));
}
