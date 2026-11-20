#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_spatial_output_gate.h"

using namespace resonance;

namespace {
constexpr int kHandles = 8;
using Readiness = SourceDirectReadiness<kHandles>;
} // namespace

TEST_CASE("fresh source handle is not direct ready", "[spatial_output_gate]") {
    Readiness readiness;
    REQUIRE_FALSE(readiness.is_direct_ready(0));
    REQUIRE_FALSE(readiness.is_direct_ready(kHandles - 1));
}

TEST_CASE("source is direct ready after its first published result", "[spatial_output_gate]") {
    Readiness readiness;
    const uint32_t pass = readiness.current_generation();
    readiness.mark_published(3, pass);
    REQUIRE(readiness.is_direct_ready(3));
    REQUIRE_FALSE(readiness.is_direct_ready(2));
}

TEST_CASE("scene reload clears direct ready for every source", "[spatial_output_gate]") {
    Readiness readiness;
    readiness.mark_published(1, readiness.current_generation());
    readiness.mark_published(2, readiness.current_generation());

    readiness.arm();

    REQUIRE_FALSE(readiness.is_direct_ready(1));
    REQUIRE_FALSE(readiness.is_direct_ready(2));
    readiness.mark_published(1, readiness.current_generation());
    REQUIRE(readiness.is_direct_ready(1));
}

TEST_CASE("result from a pass that started before a reload does not count", "[spatial_output_gate]") {
    Readiness readiness;
    const uint32_t pass_before_reload = readiness.current_generation();
    readiness.arm();
    readiness.mark_published(4, pass_before_reload);
    REQUIRE_FALSE(readiness.is_direct_ready(4));
}

TEST_CASE("recycled source handle starts not direct ready", "[spatial_output_gate]") {
    Readiness readiness;
    readiness.mark_published(5, readiness.current_generation());
    readiness.mark_published(6, readiness.current_generation());

    readiness.reset_source(5);

    REQUIRE_FALSE(readiness.is_direct_ready(5));
    REQUIRE(readiness.is_direct_ready(6));
}

TEST_CASE("source stays direct ready while passes skip it", "[spatial_output_gate]") {
    Readiness readiness;
    readiness.mark_published(2, readiness.current_generation());
    for (int pass = 0; pass < 3; pass++)
        readiness.mark_published(3, readiness.current_generation());
    REQUIRE(readiness.is_direct_ready(2));
}

TEST_CASE("empty scene is ready at once", "[spatial_output_gate]") {
    REQUIRE(spatial_scene_ready(SceneTracing::kPhonon, false, 0, false));
}

TEST_CASE("scene with geometry is ready once committed", "[spatial_output_gate]") {
    REQUIRE_FALSE(spatial_scene_ready(SceneTracing::kPhonon, false, 120, false));
    REQUIRE(spatial_scene_ready(SceneTracing::kPhonon, false, 120, true));
}

TEST_CASE("physics traced scene is not ready before its physics world is bound", "[spatial_output_gate]") {
    REQUIRE_FALSE(spatial_scene_ready(SceneTracing::kGodotPhysics, false, 0, true));
    REQUIRE(spatial_scene_ready(SceneTracing::kGodotPhysics, true, 0, true));
    REQUIRE_FALSE(spatial_scene_ready(SceneTracing::kGodotPhysics, true, 0, false));
}

TEST_CASE("simulated source waits for scene ready and its first direct result", "[spatial_output_gate]") {
    REQUIRE_FALSE(spatial_output_gate_allows(false, true, true));
    REQUIRE_FALSE(spatial_output_gate_allows(true, true, false));
    REQUIRE(spatial_output_gate_allows(true, true, true));
}

TEST_CASE("output without a simulated source waits only for scene ready", "[spatial_output_gate]") {
    REQUIRE_FALSE(spatial_output_gate_allows(false, false, false));
    REQUIRE(spatial_output_gate_allows(true, false, false));
}
