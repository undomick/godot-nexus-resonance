#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_simulator_commit_policy.h"

using namespace resonance;

TEST_CASE("simulator commit required after lifecycle or scene graph changes", "[simulator_commit][f-14]") {
    PhononSimulatorCommitInputs in{};
    REQUIRE_FALSE(phonon_simulator_commit_required(in));

    in.lifecycle_graph_changed = true;
    REQUIRE(phonon_simulator_commit_required(in));

    in.lifecycle_graph_changed = false;
    in.simulator_scene_graph_committed = true;
    REQUIRE(phonon_simulator_commit_required(in));
}

TEST_CASE("first RunReflections is not deferred after heavy scene commit", "[simulator_commit][hang_fix]") {
    REQUIRE_FALSE(phonon_worker_should_defer_reflections_after_heavy_scene_commit(true, true, false));
    REQUIRE(phonon_worker_should_defer_reflections_after_heavy_scene_commit(true, true, true));
    REQUIRE_FALSE(phonon_worker_should_defer_reflections_after_heavy_scene_commit(false, true, true));
    REQUIRE_FALSE(phonon_worker_should_defer_reflections_after_heavy_scene_commit(true, false, true));
}

TEST_CASE("lifecycle commit is separate from post-shared-inputs commit", "[simulator_commit][f-14]") {
    PhononSimulatorCommitInputs lifecycle_only{};
    lifecycle_only.lifecycle_graph_changed = true;
    REQUIRE(phonon_simulator_commit_required(lifecycle_only));
    REQUIRE_FALSE(lifecycle_only.simulator_scene_graph_committed);

    PhononSimulatorCommitInputs scene_only{};
    scene_only.simulator_scene_graph_committed = true;
    REQUIRE(phonon_simulator_commit_required(scene_only));
}

TEST_CASE("F-14 default commit inputs do not imply pathing graph mutation", "[simulator_commit][f-14][pathing]") {
    PhononSimulatorCommitInputs in{};
    REQUIRE_FALSE(phonon_simulator_commit_required(in));
}
