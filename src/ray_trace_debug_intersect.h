#ifndef RAY_TRACE_DEBUG_INTERSECT_H
#define RAY_TRACE_DEBUG_INTERSECT_H

#include <phonon.h>

namespace resonance {

constexpr float kRayDebugTriangleEpsilon = 1e-7f;

/// Vertices and shading normal for debug ray-triangle tests (matches RayTraceDebugContext triangle layout).
struct RayDebugTriangle {
    IPLVector3 v0;
    IPLVector3 v1;
    IPLVector3 v2;
    IPLVector3 normal;
};

/// Moeller-Trumbore-style ray vs triangle. `eps` is the parallel-ray threshold (default kRayDebugTriangleEpsilon).
inline bool ray_debug_ray_triangle_intersect(const IPLRay& ray, float t_min, float t_max,
                                             const RayDebugTriangle& tri, float eps,
                                             float& out_t, IPLVector3& out_normal) {
    IPLVector3 edge1 = {tri.v1.x - tri.v0.x, tri.v1.y - tri.v0.y, tri.v1.z - tri.v0.z};
    IPLVector3 edge2 = {tri.v2.x - tri.v0.x, tri.v2.y - tri.v0.y, tri.v2.z - tri.v0.z};
    IPLVector3 h = {
        ray.direction.y * edge2.z - ray.direction.z * edge2.y,
        ray.direction.z * edge2.x - ray.direction.x * edge2.z,
        ray.direction.x * edge2.y - ray.direction.y * edge2.x};
    float a = edge1.x * h.x + edge1.y * h.y + edge1.z * h.z;
    if (a > -eps && a < eps)
        return false;

    float f = 1.0f / a;
    IPLVector3 s = {ray.origin.x - tri.v0.x, ray.origin.y - tri.v0.y, ray.origin.z - tri.v0.z};
    float u = f * (s.x * h.x + s.y * h.y + s.z * h.z);
    if (u < 0.0f || u > 1.0f)
        return false;

    IPLVector3 q = {
        s.y * edge1.z - s.z * edge1.y,
        s.z * edge1.x - s.x * edge1.z,
        s.x * edge1.y - s.y * edge1.x};
    float v = f * (ray.direction.x * q.x + ray.direction.y * q.y + ray.direction.z * q.z);
    if (v < 0.0f || u + v > 1.0f)
        return false;

    float t = f * (edge2.x * q.x + edge2.y * q.y + edge2.z * q.z);
    if (t < t_min || t > t_max)
        return false;

    out_t = t;
    out_normal = tri.normal;
    return true;
}

} // namespace resonance

#endif
