#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_source_update_policy.h"
#include <atomic>
#include <cstdint>
#include <thread>

using namespace resonance;

namespace {

struct TinyParams {
    float position[3];
    float radius;
    int baked_data_variation;
};

} // namespace

TEST_CASE("source update params memcmp equality", "[source_update]") {
    TinyParams a{};
    TinyParams b{};
    a.position[0] = 1.0f;
    b.position[0] = 1.0f;
    REQUIRE(source_update_params_equal(a, b));
    b.radius = 2.0f;
    REQUIRE_FALSE(source_update_params_equal(a, b));
}

TEST_CASE("source sim reflections ignores mix; honors override and output_reverb", "[source_update][sim][steam]") {
    REQUIRE(source_sim_reflections_enabled(-1, true));
    REQUIRE(source_sim_reflections_enabled(1, true));
    REQUIRE_FALSE(source_sim_reflections_enabled(0, true));
    REQUIRE_FALSE(source_sim_reflections_enabled(-1, false));
    REQUIRE_FALSE(source_sim_reflections_enabled(1, false));
}

TEST_CASE("source sim pathing ignores mix; honors override, global, and output_reverb", "[source_update][sim][steam]") {
    REQUIRE(source_sim_pathing_enabled(-1, true, true));
    REQUIRE_FALSE(source_sim_pathing_enabled(-1, false, true));
    REQUIRE(source_sim_pathing_enabled(1, false, true));
    REQUIRE_FALSE(source_sim_pathing_enabled(0, true, true));
    REQUIRE_FALSE(source_sim_pathing_enabled(-1, true, false));
    REQUIRE_FALSE(source_sim_pathing_enabled(1, true, false));
}

TEST_CASE("playback enable ignores mix; honors output gates", "[source_update][playback][steam]") {
    REQUIRE(playback_enable_direct(true));
    REQUIRE_FALSE(playback_enable_direct(false));
    REQUIRE(playback_enable_reverb(true));
    REQUIRE_FALSE(playback_enable_reverb(false));
}

TEST_CASE("playback wants wet effects from enable overrides", "[source_update][playback][steam]") {
    REQUIRE(playback_wants_wet_effects(-1, -1, true));
    REQUIRE(playback_wants_wet_effects(-1, -1, false)); // reflections default on
    REQUIRE_FALSE(playback_wants_wet_effects(0, 0, true));
    REQUIRE_FALSE(playback_wants_wet_effects(0, -1, false));
    REQUIRE(playback_wants_wet_effects(0, 1, false));
    REQUIRE(playback_wants_wet_effects(1, 0, false));
}

TEST_CASE("voice decodes reverb itself only for hybrid reflections", "[source_update][playback][steam]") {
    REQUIRE(voice_needs_reverb_decoder(true, kReflectionHybrid));
    REQUIRE_FALSE(voice_needs_reverb_decoder(false, kReflectionHybrid));
    REQUIRE_FALSE(voice_needs_reverb_decoder(true, kReflectionConvolution));
    REQUIRE_FALSE(voice_needs_reverb_decoder(true, kReflectionTan));
    REQUIRE_FALSE(voice_needs_reverb_decoder(true, kReflectionParametric));
}

TEST_CASE("snapshot invalidation is consumed once per mark", "[source_update][snapshot]") {
    resonance::SourceSnapshotInvalidation<4> invalidation;
    uint32_t seen_a = 0;
    uint32_t seen_b = 0;
    REQUIRE_FALSE(invalidation.consume(1, seen_a));

    invalidation.invalidate(1);
    REQUIRE(invalidation.consume(1, seen_a));
    REQUIRE_FALSE(invalidation.consume(1, seen_a));
    REQUIRE_FALSE(invalidation.consume(2, seen_b));
}

TEST_CASE("invalidate_all marks every snapshot once", "[source_update][snapshot]") {
    resonance::SourceSnapshotInvalidation<4> invalidation;
    uint32_t seen_a = 0;
    uint32_t seen_b = 0;
    invalidation.invalidate_all();
    REQUIRE(invalidation.consume(0, seen_a));
    REQUIRE(invalidation.consume(3, seen_b));
    REQUIRE_FALSE(invalidation.consume(0, seen_a));
    REQUIRE_FALSE(invalidation.consume(3, seen_b));
}

TEST_CASE("an out-of-range handle invalidates every snapshot", "[source_update][snapshot]") {
    resonance::SourceSnapshotInvalidation<4> invalidation;
    uint32_t seen_in_range = 0;
    uint32_t seen_out_of_range = 0;
    invalidation.invalidate(7);
    REQUIRE(invalidation.consume(2, seen_in_range));
    REQUIRE(invalidation.consume(7, seen_out_of_range));
    REQUIRE_FALSE(invalidation.consume(7, seen_out_of_range));
}

TEST_CASE("marks from another thread reach the consumer", "[source_update][snapshot]") {
    resonance::SourceSnapshotInvalidation<4> invalidation;
    constexpr int kMarks = 1000;
    std::atomic<int> consumed{0};
    std::atomic<bool> marking_done{false};
    std::thread marker([&] {
        for (int i = 0; i < kMarks; ++i) {
            invalidation.invalidate(1);
            while (!marking_done.load() && consumed.load() <= i)
                std::this_thread::yield();
        }
    });
    uint32_t seen = 0;
    while (consumed.load() < kMarks) {
        if (invalidation.consume(1, seen))
            consumed.fetch_add(1);
    }
    marking_done.store(true);
    marker.join();
    REQUIRE(consumed.load() == kMarks);
}
