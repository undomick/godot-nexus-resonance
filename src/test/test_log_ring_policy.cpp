#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_log_ring_policy.h"

using namespace resonance;

TEST_CASE("log ring overwrite drops undrained slot ticket", "[log_ring][policy]") {
    REQUIRE(log_ring_slot_overwrite_drops_message(10, 5));
    REQUIRE_FALSE(log_ring_slot_overwrite_drops_message(10, 10));
    REQUIRE_FALSE(log_ring_slot_overwrite_drops_message(10, 11));
}

TEST_CASE("log ring empty slot ticket does not count as drop", "[log_ring][policy]") {
    REQUIRE_FALSE(log_ring_slot_overwrite_drops_message(0, 0));
    REQUIRE_FALSE(log_ring_slot_overwrite_drops_message(0, 99));
}
