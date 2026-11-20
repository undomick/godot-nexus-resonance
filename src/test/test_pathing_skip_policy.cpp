#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_pathing_skip_policy.h"

using namespace resonance;

namespace {

PathingPassInputs pass_inputs() {
    PathingPassInputs p;
    p.source_inputs_generation = 7;
    p.scene_generation = 3;
    p.probe_batch_generation = 1;
    p.deviation_generation = 2;
    p.listener.origin = {1.0f, 2.0f, 3.0f};
    p.listener.ahead = {0.0f, 0.0f, -1.0f};
    p.listener.up = {0.0f, 1.0f, 0.0f};
    p.listener.right = {1.0f, 0.0f, 0.0f};
    return p;
}

} // namespace

TEST_CASE("Unchanged inputs skip the pathing pass", "[pathing][skip]") {
    REQUIRE(pathing_pass_can_skip(true, pass_inputs(), pass_inputs(), false));
}

TEST_CASE("First pathing pass always runs", "[pathing][skip]") {
    REQUIRE_FALSE(pathing_pass_can_skip(false, pass_inputs(), pass_inputs(), false));
}

TEST_CASE("Custom scene always runs pathing", "[pathing][skip]") {
    REQUIRE_FALSE(pathing_pass_can_skip(true, pass_inputs(), pass_inputs(), true));
}

TEST_CASE("Any changed pathing input runs the pass", "[pathing][skip]") {
    const PathingPassInputs last = pass_inputs();
    PathingPassInputs next = last;

    next.source_inputs_generation++;
    REQUIRE_FALSE(pathing_pass_can_skip(true, last, next, false));

    next = last;
    next.scene_generation++;
    REQUIRE_FALSE(pathing_pass_can_skip(true, last, next, false));

    next = last;
    next.probe_batch_generation++;
    REQUIRE_FALSE(pathing_pass_can_skip(true, last, next, false));

    next = last;
    next.deviation_generation++;
    REQUIRE_FALSE(pathing_pass_can_skip(true, last, next, false));

    next = last;
    next.listener.origin.x += 0.01f;
    REQUIRE_FALSE(pathing_pass_can_skip(true, last, next, false));

    next = last;
    next.listener.ahead = {1.0f, 0.0f, 0.0f};
    REQUIRE_FALSE(pathing_pass_can_skip(true, last, next, false));

    next = last;
    next.listener.up = {0.0f, 0.0f, 1.0f};
    REQUIRE_FALSE(pathing_pass_can_skip(true, last, next, false));

    next = last;
    next.listener.right = {0.0f, 0.0f, 1.0f};
    REQUIRE_FALSE(pathing_pass_can_skip(true, last, next, false));

    next = last;
    next.debug_vis = true;
    REQUIRE_FALSE(pathing_pass_can_skip(true, last, next, false));
}
