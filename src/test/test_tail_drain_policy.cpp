#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_tail_drain_policy.h"

using namespace resonance;

TEST_CASE("reflection EOS tail stops mixing on TAILCOMPLETE", "[playback][eos][parametric][conv][continuity]") {
    REQUIRE(reflection_eos_tail_produced(false));
    REQUIRE_FALSE(reflection_eos_tail_produced(true));
}

TEST_CASE("tail drain completes when rings are drained and grace is spent", "[playback][tail-drain]") {
    REQUIRE(tail_drain_complete(true, true, 0));
}

TEST_CASE("tail drain does not require IPL effect tail sizes (voice-release regression)", "[playback][tail-drain]") {
    REQUIRE(tail_drain_complete(true, true, 0));
}

TEST_CASE("tail drain blocked while output ring still has queued frames", "[playback][tail-drain]") {
    REQUIRE_FALSE(tail_drain_complete(false, true, 0));
}

TEST_CASE("tail drain blocked while split-reverb ring still has data for the reverb child", "[playback][tail-drain]") {
    REQUIRE_FALSE(tail_drain_complete(true, false, 0));
}

TEST_CASE("tail drain blocked while grace budget still counts", "[playback][tail-drain]") {
    REQUIRE_FALSE(tail_drain_complete(true, true, 1));
    REQUIRE_FALSE(tail_drain_complete(true, true, 129));
}

TEST_CASE("tail drain blocked while grace is not armed yet", "[playback][tail-drain]") {
    REQUIRE_FALSE(tail_drain_complete(true, true, -1));
}

TEST_CASE("grace early end: requires at least one successful tail block", "[playback][tail-drain]") {
    REQUIRE_FALSE(tail_grace_end_early(false, true, true, true, 0.0f, 0.0f));
    REQUIRE_FALSE(tail_grace_end_early(false, true, true, true, 3.0e-6f, -4.0e-6f));
}

TEST_CASE("grace early end: after tail production when rings drained and silent", "[playback][tail-drain]") {
    REQUIRE(tail_grace_end_early(true, true, true, true, 0.0f, 0.0f));
    REQUIRE(tail_grace_end_early(true, true, true, true, 3.0e-6f, -4.0e-6f));
}

TEST_CASE("grace early end: blocked by audibly loud last sample after tail production", "[playback][tail-drain]") {
    REQUIRE_FALSE(tail_grace_end_early(true, true, true, true, 0.2f, 0.0f));
    REQUIRE_FALSE(tail_grace_end_early(true, true, true, true, 0.0f, -0.2f));
    REQUIRE_FALSE(tail_grace_end_early(true, true, true, true, kTailDrainSilenceEpsilon, 0.0f));
}

TEST_CASE("grace early end: blocked by queued output or reverb frames", "[playback][tail-drain]") {
    REQUIRE_FALSE(tail_grace_end_early(false, false, true, true, 0.0f, 0.0f));
    REQUIRE_FALSE(tail_grace_end_early(false, true, false, true, 0.0f, 0.0f));
}

TEST_CASE("grace early end: blocked by audibly loud last sample", "[playback][tail-drain]") {
    REQUIRE_FALSE(tail_grace_end_early(false, true, true, true, 0.2f, 0.0f));
    REQUIRE_FALSE(tail_grace_end_early(false, true, true, true, 0.0f, -0.2f));
    REQUIRE_FALSE(tail_grace_end_early(false, true, true, true, kTailDrainSilenceEpsilon, 0.0f));
}

TEST_CASE("grace early end: blocked when last sample not yet valid", "[playback][tail-drain]") {
    REQUIRE_FALSE(tail_grace_end_early(false, true, true, false, 0.0f, 0.0f));
}

TEST_CASE("tail drain IPL gate: uninitialized forces complete without IPL", "[playback][tail-drain][mix-return]") {
    const TailDrainIplGate gate = tail_drain_ipl_gate(false, false, true, true);
    REQUIRE_FALSE(gate.can_run_ipl_tail);
    REQUIRE(gate.force_drain_complete);
    REQUIRE_FALSE(gate.set_grace_to_zero);
}

TEST_CASE("tail drain IPL gate: stale context skips IPL but keeps grace for ring drain", "[playback][tail-drain][mix-return]") {
    const TailDrainIplGate gate = tail_drain_ipl_gate(true, true, true, true);
    REQUIRE_FALSE(gate.can_run_ipl_tail);
    REQUIRE_FALSE(gate.force_drain_complete);
    REQUIRE_FALSE(gate.set_grace_to_zero);
}

TEST_CASE("tail drain IPL gate: server or context mismatch skips IPL", "[playback][tail-drain][mix-return]") {
    const TailDrainIplGate no_srv = tail_drain_ipl_gate(true, false, false, false);
    REQUIRE(no_srv.set_grace_to_zero);
    const TailDrainIplGate bad_ctx = tail_drain_ipl_gate(true, false, true, false);
    REQUIRE(bad_ctx.set_grace_to_zero);
}

TEST_CASE("tail drain IPL gate: healthy playback runs IPL tails", "[playback][tail-drain][mix-return]") {
    const TailDrainIplGate gate = tail_drain_ipl_gate(true, false, true, true);
    REQUIRE(gate.can_run_ipl_tail);
    REQUIRE_FALSE(gate.force_drain_complete);
    REQUIRE_FALSE(gate.set_grace_to_zero);
}

TEST_CASE("tail drain mix return: always reports full frame count to AudioServer", "[playback][tail-drain][mix-return]") {
    REQUIRE(tail_drain_mix_return_frames(512) == 512);
    REQUIRE(tail_drain_mix_return_frames(1) == 1);
    REQUIRE(tail_drain_mix_return_frames(0) == 0);
}

TEST_CASE("tail drain input flush runs even while geometry gate holds decode", "[playback][tail-drain][hold-decode]") {
    REQUIRE(tail_drain_should_flush_input(true));
    REQUIRE(tail_drain_should_flush_input(false));
}

TEST_CASE("tail drain stale path completes once rings are empty and grace is zero", "[playback][tail-drain][mix-return]") {
    const TailDrainIplGate gate = tail_drain_ipl_gate(true, true, true, true);
    REQUIRE_FALSE(gate.set_grace_to_zero);
    REQUIRE(tail_drain_complete(true, true, 0));
}

TEST_CASE("EOS pull plan: actively draining blocks completion while grace remains", "[playback][tail-drain][t01]") {
    const TailDrainEosPullPlan pulling{false, true, false};
    REQUIRE(tail_drain_eos_actively_pullable(pulling));
    REQUIRE_FALSE(tail_drain_complete(true, true, 8, true));
    REQUIRE_FALSE(tail_grace_end_early(true, true, true, true, 0.0f, 0.0f, true, 8));
}

TEST_CASE("EOS pull plan: exhausted grace completes even when still pullable", "[playback][tail-drain][t01]") {
    const TailDrainEosPullPlan pulling{false, true, false};
    REQUIRE(tail_drain_eos_actively_pullable(pulling));
    REQUIRE(tail_drain_complete(true, true, 0, true));
    REQUIRE(tail_grace_end_early(true, true, true, true, 0.0f, 0.0f, true, 0));
}

TEST_CASE("EOS pull plan: frozen GetTailSize without pull path must not block completion", "[playback][tail-drain]") {
    const TailDrainEosPullPlan gate_closed =
        tail_drain_eos_pull_plan(false, 512, true, 4096, true, true, 256, true);
    REQUIRE_FALSE(tail_drain_eos_actively_pullable(gate_closed));
    REQUIRE(tail_drain_complete(true, true, 0, false));

    const bool conv_no_mixer = tail_drain_reflection_eos_will_pull(resonance::kReflectionConvolution, true, 4096, true,
                                                                   false);
    REQUIRE_FALSE(conv_no_mixer);
    const TailDrainEosPullPlan missing_mixer =
        tail_drain_eos_pull_plan(true, 0, true, 4096, conv_no_mixer, false, 0, false);
    REQUIRE_FALSE(tail_drain_eos_actively_pullable(missing_mixer));
    REQUIRE(tail_drain_complete(true, true, 0, false));
}

TEST_CASE("EOS pull plan: conv reflection pullable only with live mixer", "[playback][tail-drain]") {
    REQUIRE(tail_drain_reflection_eos_will_pull(resonance::kReflectionConvolution, true, 512, true, true));
    REQUIRE_FALSE(tail_drain_reflection_eos_will_pull(resonance::kReflectionConvolution, true, 512, true, false));
    REQUIRE(tail_drain_reflection_eos_will_pull(resonance::kReflectionParametric, true, 512, true, false));
}
