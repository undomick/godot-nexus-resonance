#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_shutdown_playback_policy.h"

using namespace resonance;

TEST_CASE("shutdown blocks audio mix when shutting down", "[shutdown][playback]") {
    REQUIRE(ipl_audio_teardown_blocks_mix(true, false));
    REQUIRE(ipl_audio_teardown_blocks_mix(true, true));
}

TEST_CASE("shutdown blocks audio mix when IPL teardown active", "[shutdown][playback]") {
    REQUIRE(ipl_audio_teardown_blocks_mix(false, true));
    REQUIRE_FALSE(ipl_audio_teardown_blocks_mix(false, false));
}

TEST_CASE("shutdown phase order: worker before simulation teardown", "[shutdown][playback]") {
    REQUIRE(shutdown_phase_transition_allowed(ShutdownPhase::Idle, ShutdownPhase::TeardownFlagsSet));
    REQUIRE(shutdown_phase_transition_allowed(ShutdownPhase::TeardownFlagsSet, ShutdownPhase::WorkerJoined));
    REQUIRE(shutdown_phase_transition_allowed(ShutdownPhase::WorkerJoined, ShutdownPhase::CachesInvalidated));
    REQUIRE(shutdown_phase_transition_allowed(ShutdownPhase::CachesInvalidated, ShutdownPhase::ClientsDrained));
    REQUIRE(shutdown_phase_transition_allowed(ShutdownPhase::ClientsDrained, ShutdownPhase::SimulationTeardownDone));

    REQUIRE_FALSE(shutdown_phase_transition_allowed(ShutdownPhase::Idle, ShutdownPhase::SimulationTeardownDone));
    REQUIRE_FALSE(shutdown_phase_transition_allowed(ShutdownPhase::TeardownFlagsSet, ShutdownPhase::ClientsDrained));
    REQUIRE_FALSE(shutdown_phase_transition_allowed(ShutdownPhase::WorkerJoined, ShutdownPhase::SimulationTeardownDone));
}

TEST_CASE("shutdown during playback: simulation teardown only after clients drained", "[shutdown][playback]") {
    ShutdownPhase phase = ShutdownPhase::Idle;
    auto advance = [&](ShutdownPhase next) {
        REQUIRE(shutdown_phase_transition_allowed(phase, next));
        phase = next;
    };

    advance(ShutdownPhase::TeardownFlagsSet);
    advance(ShutdownPhase::WorkerJoined);
    advance(ShutdownPhase::CachesInvalidated);
    advance(ShutdownPhase::ClientsDrained);
    advance(ShutdownPhase::SimulationTeardownDone);
    REQUIRE(phase == ShutdownPhase::SimulationTeardownDone);
}
