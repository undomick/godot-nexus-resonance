#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_polyphonic_voice_policy.h"
#include "../resonance_reflection_fetch_policy.h"

using namespace resonance;

TEST_CASE("two reflection apply readers on one source are rejected", "[polyphonic_voice]") {
    int32_t reader = -1;
    REQUIRE(claim_reflection_apply(&reader, kEmitterReflectionReader));
    REQUIRE_FALSE(claim_reflection_apply(&reader, 2));
    REQUIRE(reader == kEmitterReflectionReader);
    REQUIRE(claim_reflection_apply(&reader, kEmitterReflectionReader));
    reader = -1;
    REQUIRE(claim_reflection_apply(&reader, kEmitterReflectionReader));
}

TEST_CASE("a voice bound to the emitter may convolve that source", "[polyphonic_voice]") {
    REQUIRE(voice_may_convolve_source(4, 4));
    REQUIRE_FALSE(voice_may_convolve_source(-1, 4));
    REQUIRE_FALSE(voice_may_convolve_source(4, 8));
    REQUIRE(voice_tail_ir_matches_source(4, 4));
    REQUIRE_FALSE(voice_tail_ir_matches_source(4, 8));
    REQUIRE(voice_reflection_ir_generation_live(2u, 2u));
    REQUIRE_FALSE(voice_reflection_ir_generation_live(2u, 3u));
    REQUIRE_FALSE(voice_reflection_ir_generation_live(0u, 3u));
}

TEST_CASE("a fetch miss after the emitter applied once keeps the decoder running", "[polyphonic_voice]") {
    const bool tail = emitter_tail_applyable(true, 4, 4, 2u, 2u);
    REQUIRE(tail);
    REQUIRE_FALSE(reflection_decoder_should_wait_for_first_params(true, true, true, false, tail));
}

TEST_CASE("an emitter tail from a recycled or foreign source does not count", "[polyphonic_voice]") {
    REQUIRE_FALSE(emitter_tail_applyable(false, 4, 4, 2u, 2u));
    REQUIRE_FALSE(emitter_tail_applyable(true, 4, 8, 2u, 2u));
    REQUIRE_FALSE(emitter_tail_applyable(true, 4, 4, 2u, 3u));
}
