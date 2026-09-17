#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_baker_ipl_policy.h"

using namespace resonance;

TEST_CASE("baker progress notes cancel from flag and non-finite progress", "[baker][ipl]") {
    BakerProgressState state{};
    baker_progress_note(state, 0.5f, false);
    REQUIRE(state.saw_progress);
    REQUIRE(state.last_progress == 0.5f);
    REQUIRE_FALSE(state.cancelled);

    baker_progress_note(state, 0.75f, true);
    REQUIRE(state.cancelled);

    BakerProgressState nan_state{};
    baker_progress_note(nan_state, std::nanf(""), false);
    REQUIRE(nan_state.cancelled);
}

TEST_CASE("baker outcome after void bake distinguishes cancel", "[baker][ipl]") {
    BakerProgressState ok{};
    REQUIRE(baker_outcome_after_void_bake(ok, false) == BakerOutcome::Success);
    REQUIRE(baker_outcome_to_ipl_status(BakerOutcome::Success) == IPL_STATUS_SUCCESS);

    BakerProgressState cancelled{};
    cancelled.cancelled = true;
    REQUIRE(baker_outcome_after_void_bake(cancelled, false) == BakerOutcome::Cancelled);
    REQUIRE(baker_outcome_to_ipl_status(BakerOutcome::Cancelled) == IPL_STATUS_FAILURE);

    REQUIRE(baker_outcome_after_void_bake(ok, true) == BakerOutcome::Cancelled);
}
