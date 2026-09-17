#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_reverb_data_point_query_policy.h"

using namespace resonance;

TEST_CASE("reverb data point query once skips editor", "[reverb_data_point][query]") {
    REQUIRE_FALSE(reverb_data_point_should_query_once(true, true, true, false));
}

TEST_CASE("reverb data point query once skips missing probe data", "[reverb_data_point][query]") {
    REQUIRE_FALSE(reverb_data_point_should_query_once(false, false, true, false));
}

TEST_CASE("reverb data point query once skips before server ready", "[reverb_data_point][query]") {
    REQUIRE_FALSE(reverb_data_point_should_query_once(false, true, false, false));
}

TEST_CASE("reverb data point query once skips after already queried", "[reverb_data_point][query]") {
    REQUIRE_FALSE(reverb_data_point_should_query_once(false, true, true, true));
}

TEST_CASE("reverb data point query once fires when ready with data", "[reverb_data_point][query]") {
    REQUIRE(reverb_data_point_should_query_once(false, true, true, false));
}
