#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_material_update_policy.h"

using namespace resonance;

TEST_CASE("geometry material update skips when unchanged", "[material_update_policy]") {
    REQUIRE(geometry_material_update_path(true, true, true) == GeometryMaterialUpdatePath::None);
    REQUIRE(geometry_material_update_path(true, true, false) == GeometryMaterialUpdatePath::None);
    REQUIRE(geometry_material_update_path(true, false, false) == GeometryMaterialUpdatePath::None);
}

TEST_CASE("geometry material update skips when not in tree", "[material_update_policy]") {
    REQUIRE(geometry_material_update_path(false, false, true) == GeometryMaterialUpdatePath::None);
    REQUIRE(geometry_material_update_path(false, false, false) == GeometryMaterialUpdatePath::None);
}

TEST_CASE("geometry material update uses in-place when live meshes exist", "[material_update_policy]") {
    REQUIRE(geometry_material_update_path(false, true, true) == GeometryMaterialUpdatePath::InPlace);
}

TEST_CASE("geometry material update rebuilds when no live meshes", "[material_update_policy]") {
    REQUIRE(geometry_material_update_path(false, true, false) == GeometryMaterialUpdatePath::Rebuild);
}
