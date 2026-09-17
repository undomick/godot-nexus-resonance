#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_physics_material_lookup_policy.h"

using namespace resonance;

TEST_CASE("physics material lookup prefers direct ResonanceMaterial resource", "[physics_material_lookup]") {
    REQUIRE(physics_material_lookup_path(true, true, true) == PhysicsMaterialLookupResult::DirectResource);
    REQUIRE(physics_material_lookup_path(true, true, false) == PhysicsMaterialLookupResult::DirectResource);
    REQUIRE(physics_material_lookup_path(true, false, false) == PhysicsMaterialLookupResult::DirectResource);
}

TEST_CASE("physics material lookup uses name map when preset is loaded", "[physics_material_lookup]") {
    REQUIRE(physics_material_lookup_path(false, true, true) == PhysicsMaterialLookupResult::NameMap);
}

TEST_CASE("physics material lookup falls back when preset missing or unknown", "[physics_material_lookup]") {
    REQUIRE(physics_material_lookup_path(false, true, false) == PhysicsMaterialLookupResult::Fallback);
    REQUIRE(physics_material_lookup_path(false, false, false) == PhysicsMaterialLookupResult::Fallback);
    REQUIRE(physics_material_lookup_path(false, false, true) == PhysicsMaterialLookupResult::Fallback);
}
