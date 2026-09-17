#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_phonon_worker_policy.h"
#include "../resonance_source_handle_policy.h"
#include <vector>

using namespace resonance;

TEST_CASE("phonon worker tick skips when no maintenance or simulation demand", "[phonon_worker][policy]") {
    PhononWorkerTickInputs in{};
    REQUIRE_FALSE(phonon_worker_tick_has_work(in));
}

TEST_CASE("phonon worker tick runs for pending lifecycle or updates", "[phonon_worker][policy]") {
    PhononWorkerTickInputs in{};
    in.pending_lifecycle = true;
    REQUIRE(phonon_worker_tick_has_work(in));

    in.pending_lifecycle = false;
    in.pending_source_updates = true;
    REQUIRE(phonon_worker_tick_has_work(in));

    in.pending_source_updates = false;
    in.scene_dirty = true;
    REQUIRE(phonon_worker_tick_has_work(in));

    in.scene_dirty = false;
    in.pending_dynamic_transforms = true;
    REQUIRE(phonon_worker_tick_has_work(in));
}

TEST_CASE("phonon worker tick runs direct only when scheduled and sources need direct", "[phonon_worker][policy]") {
    PhononWorkerTickInputs in{};
    in.run_direct_scheduled = true;
    REQUIRE_FALSE(phonon_worker_tick_has_work(in));

    in.any_direct_outputs = true;
    REQUIRE(phonon_worker_tick_has_work(in));
}

TEST_CASE("phonon worker tick skips reflection heavy when global reverb output is off", "[phonon_worker][policy]") {
    PhononWorkerTickInputs in{};
    in.run_reflection_heavy = true;
    in.any_reflection_outputs = true;
    in.output_reverb_enabled = false;
    REQUIRE_FALSE(phonon_worker_tick_has_work(in));

    in.output_reverb_enabled = true;
    REQUIRE(phonon_worker_tick_has_work(in));
}

TEST_CASE("phonon worker tick skips pathing heavy without per-source pathing demand", "[phonon_worker][policy]") {
    PhononWorkerTickInputs in{};
    in.run_pathing_heavy = true;
    in.output_reverb_enabled = true;
    in.any_pathing_outputs = false;
    REQUIRE_FALSE(phonon_worker_tick_has_work(in));

    in.any_pathing_outputs = true;
    REQUIRE(phonon_worker_tick_has_work(in));
}

TEST_CASE("phonon worker shared pathing requires global pathing and reverb output", "[phonon_worker][policy]") {
    REQUIRE(phonon_worker_shared_pathing_enabled(true, true, true));
    REQUIRE_FALSE(phonon_worker_shared_pathing_enabled(false, true, true));
    REQUIRE_FALSE(phonon_worker_shared_pathing_enabled(true, false, true));
    REQUIRE_FALSE(phonon_worker_shared_pathing_enabled(true, true, false));
}

TEST_CASE("phonon worker run pathing heavy is gated on shared pathing demand", "[phonon_worker][policy]") {
    REQUIRE(phonon_worker_run_pathing_heavy(true, true, true, true));
    REQUIRE_FALSE(phonon_worker_run_pathing_heavy(true, true, true, false));
    REQUIRE_FALSE(phonon_worker_run_pathing_heavy(true, true, false, true));
}

TEST_CASE("phonon worker forces direct after input drain when direct outputs exist", "[phonon_worker][policy]") {
    REQUIRE_FALSE(phonon_worker_run_direct_after_input_drain(true, false, false));
    REQUIRE(phonon_worker_run_direct_after_input_drain(true, false, true));
    REQUIRE(phonon_worker_run_direct_after_input_drain(false, true, true));
    REQUIRE_FALSE(phonon_worker_run_direct_after_input_drain(false, false, true));
}

TEST_CASE("phonon worker forces direct after scene graph commit when direct outputs exist", "[phonon_worker][policy][geo-05]") {
    REQUIRE_FALSE(phonon_worker_run_direct_after_scene_graph_commit(true, false, false));
    REQUIRE(phonon_worker_run_direct_after_scene_graph_commit(true, false, true));
    REQUIRE(phonon_worker_run_direct_after_scene_graph_commit(false, true, true));
    REQUIRE_FALSE(phonon_worker_run_direct_after_scene_graph_commit(false, false, true));
}

TEST_CASE("phonon worker tick schedules direct for inline input updates", "[phonon_worker][policy]") {
    REQUIRE_FALSE(phonon_worker_run_direct_for_tick(false, false, false));
    REQUIRE(phonon_worker_run_direct_for_tick(false, false, true));
    REQUIRE(phonon_worker_run_direct_for_tick(false, true, false));
    REQUIRE(phonon_worker_run_direct_for_tick(true, false, false));
    REQUIRE(phonon_worker_run_direct_for_tick(true, true, true));
}

TEST_CASE("phonon worker clears stale inline direct pending when no direct outputs", "[phonon_worker][policy]") {
    REQUIRE_FALSE(phonon_worker_should_clear_stale_direct_after_inline_inputs(false, false));
    REQUIRE_FALSE(phonon_worker_should_clear_stale_direct_after_inline_inputs(false, true));
    REQUIRE(phonon_worker_should_clear_stale_direct_after_inline_inputs(true, false));
    REQUIRE_FALSE(phonon_worker_should_clear_stale_direct_after_inline_inputs(true, true));
}

TEST_CASE("phonon worker preserves heavy flags when outputs not yet attached", "[phonon_worker][policy]") {
    PhononWorkerTickInputs in{};
    in.run_reflection_heavy = true;
    in.output_reverb_enabled = true;
    in.any_reflection_outputs = false;
    REQUIRE_FALSE(phonon_worker_tick_has_work(in));
    REQUIRE_FALSE(phonon_worker_should_clear_idle_heavy_flags(in));

    in.run_reflection_heavy = false;
    in.run_pathing_heavy = true;
    in.any_pathing_outputs = false;
    REQUIRE_FALSE(phonon_worker_tick_has_work(in));
    REQUIRE_FALSE(phonon_worker_should_clear_idle_heavy_flags(in));
}

TEST_CASE("phonon worker clears idle heavy when reverb output is disabled", "[phonon_worker][policy]") {
    PhononWorkerTickInputs in{};
    in.run_reflection_heavy = true;
    in.output_reverb_enabled = false;
    in.any_reflection_outputs = false;
    REQUIRE(phonon_worker_should_clear_idle_heavy_flags(in));
}

TEST_CASE("pathing output scan mirrors reflection scan", "[phonon_worker][policy]") {
    std::vector<int32_t> handles = {-1, 5, kMaxSimulationSourcesUserMax};
    const bool active = any_source_has_pathing_outputs(handles, [](int32_t h) -> uint8_t {
        return h == 5 ? 1u : 0u;
    });
    REQUIRE(active);

    const bool inactive = any_source_has_pathing_outputs(handles, [](int32_t h) -> uint8_t {
        return h == 5 ? 0u : 1u;
    });
    REQUIRE_FALSE(inactive);
}

TEST_CASE("phonon worker skips new heavy simulation during shutdown", "[phonon_worker][policy][hang_fix]") {
    REQUIRE(phonon_worker_allow_new_heavy_simulation(false, true));
    REQUIRE_FALSE(phonon_worker_allow_new_heavy_simulation(true, true));
    REQUIRE_FALSE(phonon_worker_allow_new_heavy_simulation(false, false));
    REQUIRE_FALSE(phonon_worker_allow_new_heavy_simulation(true, false));
}

TEST_CASE("phonon worker skips new heavy simulation during cold-start settle", "[phonon_worker][policy][startup]") {
    REQUIRE(phonon_worker_allow_new_heavy_simulation(false, true, false));
    REQUIRE_FALSE(phonon_worker_allow_new_heavy_simulation(false, true, true));
    REQUIRE_FALSE(phonon_worker_allow_new_heavy_simulation(true, true, true));
}
