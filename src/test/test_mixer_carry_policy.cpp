#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_mixer_carry_policy.h"

using namespace resonance;

TEST_CASE("mixer carry compact moves unread count to front", "[mixer][carry]") {
    size_t read_index = 4;
    size_t len = 10;
    mixer_carry_compact_pending(read_index, len);
    REQUIRE(read_index == 0);
    REQUIRE(len == 6);
}

TEST_CASE("mixer carry drop count frees one frame without clearing all", "[mixer][carry]") {
    const size_t cap = 1024;
    const size_t frame_size = 256;
    const size_t len = 900;
    REQUIRE(mixer_carry_drop_count_for_append(len, frame_size, cap) == 132);
    REQUIRE(mixer_carry_len_after_drop(len, frame_size, cap) == 768);
    REQUIRE(mixer_carry_len_after_drop(len, frame_size, cap) + frame_size <= cap);
}

TEST_CASE("mixer carry drop count clears when drop consumes entire buffer", "[mixer][carry]") {
    REQUIRE(mixer_carry_len_after_drop(50, 1024, 1024) == 0);
}

TEST_CASE("mixer carry drop count fails when frame exceeds capacity", "[mixer][carry]") {
    REQUIRE(mixer_carry_drop_count_for_append(0, 2048, 1024) == std::numeric_limits<size_t>::max());
}
