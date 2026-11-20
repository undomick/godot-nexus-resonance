#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_reflection_skip_policy.h"

using namespace resonance;

namespace {

ReflectionPassInputs pass_inputs() {
    ReflectionPassInputs p;
    p.source_inputs_generation = 7;
    p.scene_generation = 3;
    p.probe_batch_generation = 1;
    p.listener.origin = {1.0f, 2.0f, 3.0f};
    p.listener.ahead = {0.0f, 0.0f, -1.0f};
    p.listener.up = {0.0f, 1.0f, 0.0f};
    p.listener.right = {1.0f, 0.0f, 0.0f};
    p.num_rays = 1024;
    p.num_bounces = 4;
    p.duration = 2.0f;
    p.order = 1;
    p.irradiance_min_distance = 1.0f;
    return p;
}

} // namespace

TEST_CASE("Unchanged inputs skip the reflections pass", "[reflections][skip]") {
    REQUIRE(reflection_pass_can_skip(true, pass_inputs(), pass_inputs(), false));
}

TEST_CASE("First pass always runs", "[reflections][skip]") {
    REQUIRE_FALSE(reflection_pass_can_skip(false, pass_inputs(), pass_inputs(), false));
}

TEST_CASE("Custom scene always runs", "[reflections][skip]") {
    REQUIRE_FALSE(reflection_pass_can_skip(true, pass_inputs(), pass_inputs(), true));
}

TEST_CASE("Any changed input runs the pass", "[reflections][skip]") {
    const ReflectionPassInputs last = pass_inputs();
    ReflectionPassInputs next = last;

    next.source_inputs_generation++;
    REQUIRE_FALSE(reflection_pass_can_skip(true, last, next, false));

    next = last;
    next.scene_generation++;
    REQUIRE_FALSE(reflection_pass_can_skip(true, last, next, false));

    next = last;
    next.probe_batch_generation++;
    REQUIRE_FALSE(reflection_pass_can_skip(true, last, next, false));

    next = last;
    next.listener.origin.x += 0.01f;
    REQUIRE_FALSE(reflection_pass_can_skip(true, last, next, false));

    next = last;
    next.listener.ahead = {1.0f, 0.0f, 0.0f};
    REQUIRE_FALSE(reflection_pass_can_skip(true, last, next, false));

    next = last;
    next.num_rays = 512;
    REQUIRE_FALSE(reflection_pass_can_skip(true, last, next, false));

    next = last;
    next.num_bounces = 2;
    REQUIRE_FALSE(reflection_pass_can_skip(true, last, next, false));

    next = last;
    next.duration = 1.0f;
    REQUIRE_FALSE(reflection_pass_can_skip(true, last, next, false));

    next = last;
    next.order = 2;
    REQUIRE_FALSE(reflection_pass_can_skip(true, last, next, false));

    next = last;
    next.irradiance_min_distance = 0.5f;
    REQUIRE_FALSE(reflection_pass_can_skip(true, last, next, false));
}
