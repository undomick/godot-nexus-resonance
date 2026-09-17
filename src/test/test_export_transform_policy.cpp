#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_export_transform_policy.h"

using namespace resonance;

namespace {

MeshBakeTransform translation(float x, float y, float z) {
    MeshBakeTransform t = identity_mesh_bake_transform();
    t.origin[0] = x;
    t.origin[1] = y;
    t.origin[2] = z;
    return t;
}

} // namespace

TEST_CASE("compose_mesh_bake_xforms_root_first empty is identity", "[export_transform]") {
    MeshBakeTransform out = compose_mesh_bake_xforms_root_first(nullptr, 0);
    REQUIRE(out.origin[0] == Approx(0.0f));
    REQUIRE(out.origin[1] == Approx(0.0f));
    REQUIRE(out.origin[2] == Approx(0.0f));
}

TEST_CASE("compose_mesh_bake_xforms_root_first single equals itself", "[export_transform]") {
    MeshBakeTransform t = translation(1, 2, 3);
    MeshBakeTransform out = compose_mesh_bake_xforms_root_first(&t, 1);
    REQUIRE(out.origin[0] == Approx(1.0f));
    REQUIRE(out.origin[1] == Approx(2.0f));
    REQUIRE(out.origin[2] == Approx(3.0f));
}

TEST_CASE("compose_mesh_bake_xforms_root_first parent then child sums translations", "[export_transform]") {
    MeshBakeTransform chain[2] = {translation(10, 0, 0), translation(0, 5, 0)};
    MeshBakeTransform out = compose_mesh_bake_xforms_root_first(chain, 2);
    REQUIRE(out.origin[0] == Approx(10.0f));
    REQUIRE(out.origin[1] == Approx(5.0f));
    REQUIRE(out.origin[2] == Approx(0.0f));
}

TEST_CASE("multiply_mesh_bake_transform matches parent times local origin", "[export_transform]") {
    MeshBakeTransform parent = translation(1, 0, 0);
    MeshBakeTransform local = translation(0, 2, 0);
    MeshBakeTransform out = multiply_mesh_bake_transform(parent, local);
    REQUIRE(out.origin[0] == Approx(1.0f));
    REQUIRE(out.origin[1] == Approx(2.0f));
    REQUIRE(out.origin[2] == Approx(0.0f));
}
