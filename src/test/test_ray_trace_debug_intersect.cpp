#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../ray_trace_debug_intersect.h"

using namespace resonance;

namespace {

RayDebugTriangle unit_xy_triangle() {
    RayDebugTriangle tri{};
    tri.v0 = {0.0f, 0.0f, 0.0f};
    tri.v1 = {1.0f, 0.0f, 0.0f};
    tri.v2 = {0.0f, 1.0f, 0.0f};
    tri.normal = {0.0f, 0.0f, 1.0f};
    return tri;
}

} // namespace

TEST_CASE("ray_debug_ray_triangle_intersect hits triangle interior", "[ray_debug][intersect]") {
    const RayDebugTriangle tri = unit_xy_triangle();
    IPLRay ray{};
    ray.origin = {0.25f, 0.25f, 1.0f};
    ray.direction = {0.0f, 0.0f, -1.0f};

    float t = 0.0f;
    IPLVector3 n{};
    REQUIRE(ray_debug_ray_triangle_intersect(ray, 0.0f, 100.0f, tri, kRayDebugTriangleEpsilon, t, n));
    REQUIRE(t == Approx(1.0f));
    REQUIRE(n.x == Approx(0.0f));
    REQUIRE(n.y == Approx(0.0f));
    REQUIRE(n.z == Approx(1.0f));
}

TEST_CASE("ray_debug_ray_triangle_intersect miss outside triangle on plane", "[ray_debug][intersect]") {
    const RayDebugTriangle tri = unit_xy_triangle();
    IPLRay ray{};
    ray.origin = {2.0f, 2.0f, 1.0f};
    ray.direction = {0.0f, 0.0f, -1.0f};

    float t = 0.0f;
    IPLVector3 n{};
    REQUIRE_FALSE(ray_debug_ray_triangle_intersect(ray, 0.0f, 100.0f, tri, kRayDebugTriangleEpsilon, t, n));
}

TEST_CASE("ray_debug_ray_triangle_intersect parallel to triangle plane", "[ray_debug][intersect]") {
    const RayDebugTriangle tri = unit_xy_triangle();
    IPLRay ray{};
    ray.origin = {0.5f, 0.5f, 1.0f};
    ray.direction = {1.0f, 0.0f, 0.0f};

    float t = 0.0f;
    IPLVector3 n{};
    REQUIRE_FALSE(ray_debug_ray_triangle_intersect(ray, 0.0f, 100.0f, tri, kRayDebugTriangleEpsilon, t, n));
}

TEST_CASE("ray_debug_ray_triangle_intersect respects t_min t_max", "[ray_debug][intersect]") {
    const RayDebugTriangle tri = unit_xy_triangle();
    IPLRay ray{};
    ray.origin = {0.25f, 0.25f, 1.0f};
    ray.direction = {0.0f, 0.0f, -1.0f};

    float t = 0.0f;
    IPLVector3 n{};
    REQUIRE_FALSE(ray_debug_ray_triangle_intersect(ray, 1.5f, 10.0f, tri, kRayDebugTriangleEpsilon, t, n));
    REQUIRE_FALSE(ray_debug_ray_triangle_intersect(ray, 0.0f, 0.5f, tri, kRayDebugTriangleEpsilon, t, n));
}
