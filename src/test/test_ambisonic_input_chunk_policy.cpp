#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_ambisonic_input_chunk_policy.h"

using namespace resonance;

TEST_CASE("ambisonic input chunk: mix larger than scratch buffer is split", "[ambisonic][input][chunk]") {
    const int channels = 4;
    const size_t scratch_floats = 256 * channels;
    int32_t frames_left = 1000;
    int chunks = 0;
    while (frames_left > 0) {
        const int32_t chunk = ambisonic_input_chunk_frames(scratch_floats, channels, frames_left);
        REQUIRE(chunk > 0);
        REQUIRE(static_cast<size_t>(chunk) * channels <= scratch_floats);
        frames_left -= chunk;
        chunks++;
    }
    REQUIRE(frames_left == 0);
    REQUIRE(chunks == 4);
}

TEST_CASE("ambisonic input chunk: mix that fits is one chunk", "[ambisonic][input][chunk]") {
    REQUIRE(ambisonic_input_chunk_frames(512 * 9, 9, 300) == 300);
    REQUIRE(ambisonic_input_chunk_frames(512 * 9, 9, 512) == 512);
}

TEST_CASE("ambisonic input chunk: only whole frames fit", "[ambisonic][input][chunk]") {
    REQUIRE(ambisonic_input_chunk_frames(17, 4, 100) == 4);
    REQUIRE(ambisonic_input_chunk_frames(3, 4, 100) == 0);
}

TEST_CASE("ambisonic input chunk: nothing to do returns zero", "[ambisonic][input][chunk]") {
    REQUIRE(ambisonic_input_chunk_frames(0, 4, 100) == 0);
    REQUIRE(ambisonic_input_chunk_frames(1024, 4, 0) == 0);
    REQUIRE(ambisonic_input_chunk_frames(1024, 0, 100) == 0);
}
