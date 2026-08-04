#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_static_export_policy.h"

using namespace resonance;

TEST_CASE("export root is never pruned", "[static_export_policy]") {
    REQUIRE_FALSE(should_prune_static_export_subtree(true, true, true, true));
    REQUIRE_FALSE(should_prune_static_export_subtree(true, false, true, true));
    REQUIRE_FALSE(should_prune_static_export_subtree(true, false, false, false));
}

TEST_CASE("nested ResonanceStaticScene always prunes", "[static_export_policy]") {
    REQUIRE(should_prune_static_export_subtree(false, true, false, false));
    REQUIRE(should_prune_static_export_subtree(false, true, true, true));
}

TEST_CASE("instance root with RSS in subtree prunes", "[static_export_policy]") {
    REQUIRE(should_prune_static_export_subtree(false, false, true, true));
}

TEST_CASE("instance root without RSS does not prune", "[static_export_policy]") {
    REQUIRE_FALSE(should_prune_static_export_subtree(false, false, true, false));
}

TEST_CASE("inline folder without scene path does not prune on subtree RSS flag alone", "[static_export_policy]") {
    // Sibling geometry under an inline folder still merges; only nested RSS nodes prune.
    REQUIRE_FALSE(should_prune_static_export_subtree(false, false, false, true));
}

TEST_CASE("plain node without RSS or scene path does not prune", "[static_export_policy]") {
    REQUIRE_FALSE(should_prune_static_export_subtree(false, false, false, false));
}
