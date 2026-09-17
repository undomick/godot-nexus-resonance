#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_playback_input_started_policy.h"

using namespace resonance;

TEST_CASE("playback steam path begins on first ingested block", "[playback][input-started]") {
    REQUIRE(playback_steam_input_path_should_run(false, true));
    REQUIRE(playback_steam_input_path_should_run(true, false));
    REQUIRE_FALSE(playback_steam_input_path_should_run(false, false));
}
