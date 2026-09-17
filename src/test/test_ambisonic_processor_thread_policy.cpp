#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_ambisonic_processor_thread_policy.h"

using namespace resonance;

TEST_CASE("ambisonic audio passthrough when IPL not ready or config stale", "[ambisonic][policy][thread]") {
    REQUIRE(ambisonic_audio_should_passthrough(false, false, false, true));
    REQUIRE(ambisonic_audio_should_passthrough(true, true, false, true));
    REQUIRE(ambisonic_audio_should_passthrough(true, false, true, true));
    REQUIRE(ambisonic_audio_should_passthrough(true, false, false, false));
    REQUIRE_FALSE(ambisonic_audio_should_passthrough(true, false, false, true));
}

TEST_CASE("ambisonic main reinit when initialized and config drifted", "[ambisonic][policy][thread]") {
    REQUIRE_FALSE(ambisonic_main_should_reinit_processor(false, false, true));
    REQUIRE(ambisonic_main_should_reinit_processor(true, true, true));
    REQUIRE(ambisonic_main_should_reinit_processor(true, false, false));
    REQUIRE_FALSE(ambisonic_main_should_reinit_processor(true, false, true));
}

TEST_CASE("ambisonic pump breaks when decode cannot consume input", "[ambisonic][policy][pump]") {
    REQUIRE(ambisonic_pump_should_break(false, true));
    REQUIRE(ambisonic_pump_should_break(true, false));
    REQUIRE_FALSE(ambisonic_pump_should_break(true, true));
}
