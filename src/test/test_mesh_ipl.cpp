#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_mesh_ipl.h"
#include <vector>

using namespace resonance;

namespace {

constexpr float kVertsIndexed[] = {
    0.0f, 0.0f, 0.0f, //
    1.0f, 0.0f, 0.0f, //
    0.0f, 1.0f, 0.0f, //
    1.0f, 1.0f, 0.0f, //
};

constexpr int32_t kIndicesTwoTris[] = {0, 1, 2, 1, 3, 2};

constexpr float kVertsNonIndexed[] = {
    0.0f, 0.0f, 0.0f, //
    1.0f, 0.0f, 0.0f, //
    0.0f, 1.0f, 0.0f, //
};

MeshBakeTransform translation_transform(float tx, float ty, float tz) {
    MeshBakeTransform t = identity_mesh_bake_transform();
    t.origin[0] = tx;
    t.origin[1] = ty;
    t.origin[2] = tz;
    return t;
}

} // namespace

TEST_CASE("append_mesh_surface_to_ipl skips too few vertices", "[mesh_ipl]") {
    std::vector<IPLVector3> vertices;
    std::vector<IPLTriangle> triangles;
    std::vector<IPLint32> mat_indices;

    const int added = append_mesh_surface_to_ipl(kVertsNonIndexed, 2, nullptr, 0, identity_mesh_bake_transform(), vertices,
                                                 triangles, &mat_indices);
    REQUIRE(added == 0);
    REQUIRE(vertices.empty());
    REQUIRE(triangles.empty());
    REQUIRE(mat_indices.empty());
}

TEST_CASE("append_mesh_surface_to_ipl indexed surface appends triangles and material indices", "[mesh_ipl]") {
    std::vector<IPLVector3> vertices;
    std::vector<IPLTriangle> triangles;
    std::vector<IPLint32> mat_indices;

    const int added = append_mesh_surface_to_ipl(kVertsIndexed, 4, kIndicesTwoTris, 6, identity_mesh_bake_transform(),
                                                 vertices, triangles, &mat_indices, 7);
    REQUIRE(added == 2);
    REQUIRE(vertices.size() == 4);
    REQUIRE(triangles.size() == 2);
    REQUIRE(mat_indices.size() == 2);
    REQUIRE(mat_indices[0] == 7);
    REQUIRE(mat_indices[1] == 7);
    REQUIRE(triangles[0].indices[0] == 0);
    REQUIRE(triangles[0].indices[1] == 1);
    REQUIRE(triangles[0].indices[2] == 2);
    REQUIRE(triangles[1].indices[0] == 1);
    REQUIRE(triangles[1].indices[1] == 3);
    REQUIRE(triangles[1].indices[2] == 2);
}

TEST_CASE("append_mesh_surface_to_ipl non-indexed surface appends one triangle", "[mesh_ipl]") {
    std::vector<IPLVector3> vertices;
    std::vector<IPLTriangle> triangles;

    const int added =
        append_mesh_surface_to_ipl(kVertsNonIndexed, 3, nullptr, 0, identity_mesh_bake_transform(), vertices, triangles, nullptr);
    REQUIRE(added == 1);
    REQUIRE(vertices.size() == 3);
    REQUIRE(triangles.size() == 1);
    REQUIRE(triangles[0].indices[0] == 0);
    REQUIRE(triangles[0].indices[1] == 1);
    REQUIRE(triangles[0].indices[2] == 2);
}

TEST_CASE("append_mesh_surface_to_ipl applies bake transform to vertices", "[mesh_ipl]") {
    std::vector<IPLVector3> vertices;
    std::vector<IPLTriangle> triangles;

    const MeshBakeTransform xform = translation_transform(2.0f, -1.0f, 0.5f);
    append_mesh_surface_to_ipl(kVertsNonIndexed, 3, nullptr, 0, xform, vertices, triangles, nullptr);

    REQUIRE(vertices.size() == 3);
    REQUIRE(vertices[0].x == Approx(2.0f));
    REQUIRE(vertices[0].y == Approx(-1.0f));
    REQUIRE(vertices[0].z == Approx(0.5f));
    REQUIRE(vertices[1].x == Approx(3.0f));
    REQUIRE(vertices[1].y == Approx(-1.0f));
    REQUIRE(vertices[1].z == Approx(0.5f));
}

TEST_CASE("append_mesh_surface_to_ipl offsets indices when appending second surface", "[mesh_ipl]") {
    std::vector<IPLVector3> vertices;
    std::vector<IPLTriangle> triangles;

    append_mesh_surface_to_ipl(kVertsNonIndexed, 3, nullptr, 0, identity_mesh_bake_transform(), vertices, triangles, nullptr);
    append_mesh_surface_to_ipl(kVertsNonIndexed, 3, nullptr, 0, identity_mesh_bake_transform(), vertices, triangles, nullptr);

    REQUIRE(vertices.size() == 6);
    REQUIRE(triangles.size() == 2);
    REQUIRE(triangles[1].indices[0] == 3);
    REQUIRE(triangles[1].indices[1] == 4);
    REQUIRE(triangles[1].indices[2] == 5);
}
