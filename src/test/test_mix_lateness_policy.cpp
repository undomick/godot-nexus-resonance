#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "resonance_mix_lateness_policy.h"
#include <cstdint>

using resonance::MixLatenessTracker;

namespace {
constexpr uint64_t kBlockUs = 11610; // 512 frames at 44.1 kHz

int count_late(MixLatenessTracker& tracker, const uint64_t* gaps, int n) {
    int late = 0;
    for (int i = 0; i < n; i++) {
        if (tracker.note_mix(gaps[i], kBlockUs))
            late++;
    }
    return late;
}
} // namespace

TEST_CASE("steady calls one block apart are never late", "[mix_lateness]") {
    MixLatenessTracker tracker;
    const uint64_t gaps[] = {kBlockUs, kBlockUs + 900, kBlockUs - 900, kBlockUs, kBlockUs + 2000, kBlockUs - 2000};
    REQUIRE(count_late(tracker, gaps, 6) == 0);
}

TEST_CASE("two blocks back to back then a double gap are not late", "[mix_lateness]") {
    MixLatenessTracker tracker;
    for (int burst = 0; burst < 100; burst++) {
        REQUIRE_FALSE(tracker.note_mix(100, kBlockUs));
        REQUIRE_FALSE(tracker.note_mix(2 * kBlockUs - 100, kBlockUs));
    }
}

TEST_CASE("a single gap above one and a half blocks is late", "[mix_lateness]") {
    MixLatenessTracker tracker;
    REQUIRE_FALSE(tracker.note_mix(kBlockUs + kBlockUs / 2, kBlockUs));
    REQUIRE(tracker.note_mix(kBlockUs + kBlockUs / 2 + 1, kBlockUs));
}

TEST_CASE("a tolerated deficit does not carry over to the next call", "[mix_lateness]") {
    MixLatenessTracker tracker;
    REQUIRE_FALSE(tracker.note_mix(kBlockUs + kBlockUs / 2, kBlockUs));
    REQUIRE_FALSE(tracker.note_mix(kBlockUs + kBlockUs / 4, kBlockUs));
}

TEST_CASE("a stall after a burst is late once the buffered lead is used up", "[mix_lateness]") {
    MixLatenessTracker tracker;
    REQUIRE_FALSE(tracker.note_mix(100, kBlockUs));
    REQUIRE(tracker.note_mix(4 * kBlockUs, kBlockUs));
}

TEST_CASE("the lead is capped so a long stall after a long burst still counts", "[mix_lateness]") {
    MixLatenessTracker tracker;
    for (int i = 0; i < 50; i++)
        tracker.note_mix(0, kBlockUs);
    REQUIRE(tracker.note_mix(150000, kBlockUs));
}

TEST_CASE("reset drops the lead", "[mix_lateness]") {
    MixLatenessTracker tracker;
    tracker.note_mix(0, kBlockUs);
    tracker.reset();
    REQUIRE(tracker.note_mix(2 * kBlockUs, kBlockUs));
}
