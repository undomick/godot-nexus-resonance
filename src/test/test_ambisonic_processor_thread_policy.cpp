#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_ambisonic_processor_thread_policy.h"

using namespace resonance;

TEST_CASE("ambisonic audio owns the processor only when initialized and no flag is set", "[ambisonic][policy][thread]") {
    REQUIRE(ambisonic_audio_owns_processor(true, false, false));
    REQUIRE_FALSE(ambisonic_audio_owns_processor(false, false, false));
    REQUIRE_FALSE(ambisonic_audio_owns_processor(true, true, false));
    REQUIRE_FALSE(ambisonic_audio_owns_processor(true, false, true));
}

TEST_CASE("ambisonic main reinits only after audio flagged the config stale", "[ambisonic][policy][thread]") {
    REQUIRE(ambisonic_main_should_reinit_processor(true, true));
    REQUIRE_FALSE(ambisonic_main_should_reinit_processor(true, false));
    REQUIRE_FALSE(ambisonic_main_should_reinit_processor(false, true));
}

TEST_CASE("ambisonic main and audio never own the processor at the same time", "[ambisonic][policy][thread]") {
    for (int bits = 0; bits < 8; ++bits) {
        const bool initialized = bits & 1;
        const bool steam_context_stale = bits & 2;
        const bool processor_config_stale = bits & 4;
        const bool audio = ambisonic_audio_owns_processor(initialized, steam_context_stale, processor_config_stale);
        const bool main_reinits = ambisonic_main_should_reinit_processor(initialized, processor_config_stale) || steam_context_stale;
        REQUIRE_FALSE((audio && main_reinits));
    }
}
