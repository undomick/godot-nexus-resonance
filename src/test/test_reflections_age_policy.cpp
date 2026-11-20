#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "resonance_reflections_age_policy.h"
#include <atomic>
#include <cstdint>

using resonance::ReflectionsAgeTracker;

TEST_CASE("reflections age has no sample before the second pass", "[reflections_age]") {
    ReflectionsAgeTracker tracker;
    REQUIRE_FALSE(tracker.note_pass(1000, true).has_value());
    const auto age = tracker.note_pass(101000, true);
    REQUIRE(age.has_value());
    REQUIRE(*age == 100000);
}

TEST_CASE("reflections age tracks last and max over several passes", "[reflections_age]") {
    ReflectionsAgeTracker tracker;
    std::atomic<uint64_t> max_age{0};
    uint64_t last = 0;
    const uint64_t pass_ends_us[] = {0, 100000, 290000, 400000};
    for (uint64_t now : pass_ends_us) {
        if (const auto age = tracker.note_pass(now, true)) {
            last = *age;
            resonance::store_max(max_age, *age);
        }
    }
    REQUIRE(last == 110000);
    REQUIRE(max_age.load() == 190000);
}

TEST_CASE("reset starts a new chain without a sample", "[reflections_age]") {
    ReflectionsAgeTracker tracker;
    tracker.note_pass(0, true);
    tracker.reset();
    REQUIRE_FALSE(tracker.note_pass(5000000, true).has_value());
    REQUIRE(tracker.note_pass(5100000, true).value() == 100000);
}

TEST_CASE("passes without realtime reflections produce no samples and end the chain", "[reflections_age]") {
    ReflectionsAgeTracker tracker;
    tracker.note_pass(0, true);
    REQUIRE_FALSE(tracker.note_pass(100000, false).has_value());
    REQUIRE_FALSE(tracker.note_pass(200000, false).has_value());
    REQUIRE_FALSE(tracker.note_pass(300000, true).has_value());
    REQUIRE(tracker.note_pass(400000, true).value() == 100000);
}

TEST_CASE("store_max keeps the larger value", "[reflections_age]") {
    std::atomic<uint64_t> target{50};
    resonance::store_max(target, 10);
    REQUIRE(target.load() == 50);
    resonance::store_max(target, 70);
    REQUIRE(target.load() == 70);
}
