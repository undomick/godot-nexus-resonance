#include "ray_trace_debug_context.h"
#include "ray_trace_debug_intersect.h"
#include <cmath>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace godot {

namespace {

static inline IPLVector3 transform_point(const IPLMatrix4x4& m, const IPLVector3& p) {
    IPLVector3 out;
    out.x = m.elements[0][0] * p.x + m.elements[0][1] * p.y + m.elements[0][2] * p.z + m.elements[0][3];
    out.y = m.elements[1][0] * p.x + m.elements[1][1] * p.y + m.elements[1][2] * p.z + m.elements[1][3];
    out.z = m.elements[2][0] * p.x + m.elements[2][1] * p.y + m.elements[2][2] * p.z + m.elements[2][3];
    return out;
}

static IPLMatrix4x4 identity_matrix() {
    IPLMatrix4x4 m{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            m.elements[i][j] = (i == j) ? 1.0f : 0.0f;
    return m;
}

void append_ray_debug_segment(Array& out, const Vector3& from, const Vector3& to, int bounce) {
    Dictionary d;
    d[StringName("from")] = from;
    d[StringName("to")] = to;
    d[StringName("bounce")] = bounce;
    out.push_back(d);
}

} // namespace

RayTraceDebugContext::RayTraceDebugContext() {}

RayTraceDebugContext::~RayTraceDebugContext() {
    clear();
}

void RayTraceDebugContext::clear() {
    std::lock_guard<std::mutex> lock(geometry_mutex_);
    triangles_.clear();
    materials_.clear();
    mesh_id_to_mat_offset_.clear();
    next_mesh_id_ = 1;
}

int RayTraceDebugContext::register_mesh(const std::vector<IPLVector3>& vertices,
                                        const std::vector<IPLTriangle>& triangles,
                                        const IPLint32* material_indices,
                                        const IPLMatrix4x4* transform,
                                        const IPLMaterial* material) {
    (void)material_indices; // Per-triangle materials ignored; debug context uses one material per mesh.
    std::lock_guard<std::mutex> lock(geometry_mutex_);

    IPLMatrix4x4 xform = transform ? *transform : identity_matrix();
    int mat_offset = (int)materials_.size();
    if (material) {
        materials_.push_back(*material);
    } else {
        IPLMaterial def{};
        def.absorption[0] = def.absorption[1] = def.absorption[2] = 0.1f;
        def.scattering = 0.5f;
        def.transmission[0] = def.transmission[1] = def.transmission[2] = 0.1f;
        materials_.push_back(def);
    }

    const int mesh_id = next_mesh_id_++;
    mesh_id_to_mat_offset_[mesh_id] = mat_offset;

    triangles_.reserve(triangles_.size() + triangles.size());
    for (size_t i = 0; i < triangles.size(); i++) {
        const IPLTriangle& t = triangles[i];
        TriangleData td;
        td.v0 = transform_point(xform, vertices[t.indices[0]]);
        td.v1 = transform_point(xform, vertices[t.indices[1]]);
        td.v2 = transform_point(xform, vertices[t.indices[2]]);

        IPLVector3 e1 = {td.v1.x - td.v0.x, td.v1.y - td.v0.y, td.v1.z - td.v0.z};
        IPLVector3 e2 = {td.v2.x - td.v0.x, td.v2.y - td.v0.y, td.v2.z - td.v0.z};
        float nx = e1.y * e2.z - e1.z * e2.y;
        float ny = e1.z * e2.x - e1.x * e2.z;
        float nz = e1.x * e2.y - e1.y * e2.x;
        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > kNormalLengthEpsilon) {
            td.normal.x = nx / len;
            td.normal.y = ny / len;
            td.normal.z = nz / len;
        } else {
            td.normal.x = td.normal.y = 0.0f;
            td.normal.z = 1.0f;
        }
        td.material_index = mat_offset;
        triangles_.push_back(td);
    }
    return mesh_id;
}

void RayTraceDebugContext::unregister_mesh(int mesh_id) {
    std::lock_guard<std::mutex> lock(geometry_mutex_);
    auto it = mesh_id_to_mat_offset_.find(mesh_id);
    if (it == mesh_id_to_mat_offset_.end())
        return;
    mesh_id_to_mat_offset_.erase(it);
    // Triangles are not removed per mesh; call clear() when the debug scene is rebuilt.
}

bool RayTraceDebugContext::ray_triangle_intersect(const IPLRay& ray, float t_min, float t_max,
                                                  const TriangleData& tri, float& out_t, IPLVector3& out_normal) const {
    const resonance::RayDebugTriangle rt{tri.v0, tri.v1, tri.v2, tri.normal};
    return resonance::ray_debug_ray_triangle_intersect(ray, t_min, t_max, rt, resonance::kRayDebugTriangleEpsilon, out_t,
                                                       out_normal);
}

bool RayTraceDebugContext::closest_hit_along_ray(const IPLRay& ray, float t_min, float t_max, float& out_t) const {
    float best_t = t_max + 1.0f;
    for (const TriangleData& tri : triangles_) {
        float t;
        IPLVector3 n;
        if (ray_triangle_intersect(ray, t_min, t_max, tri, t, n) && t < best_t)
            best_t = t;
    }
    if (best_t > t_max)
        return false;
    out_t = best_t;
    return true;
}

void RayTraceDebugContext::trace_reflection_rays_for_viz(const IPLVector3& origin, int num_rays,
                                                         float max_distance, Array& out_segments) {
    out_segments.clear();
    if (num_rays <= 0 || max_distance <= 0.0f)
        return;

    std::lock_guard<std::mutex> lock(geometry_mutex_);
    if (triangles_.empty())
        return;

    static constexpr float kPi = 3.14159265f;
    const float n = static_cast<float>(num_rays);
    const Vector3 from(origin.x, origin.y, origin.z);

    for (int i = 0; i < num_rays && (int)out_segments.size() < resonance::kRayDebugMaxSegments; i++) {
        // Uniform on sphere: z uniform in [-1,1], azimuth phi uniform in [0, 2pi).
        const float z = 1.0f - (2.0f * (static_cast<float>(i) + 0.5f) / n);
        const float phi = 2.0f * kPi * static_cast<float>(i) / n;
        const float r_xy = std::sqrt(std::max(0.0f, 1.0f - z * z));
        IPLVector3 dir{r_xy * std::cos(phi), r_xy * std::sin(phi), z};
        const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        if (len > 1e-8f) {
            dir.x /= len;
            dir.y /= len;
            dir.z /= len;
        } else {
            dir = IPLVector3{0.0f, 0.0f, 1.0f};
        }

        IPLRay ray = {origin, dir};
        float hit_t = 0.0f;
        if (!closest_hit_along_ray(ray, kMinRayT, max_distance, hit_t))
            continue;

        const float to_x = origin.x + dir.x * hit_t;
        const float to_y = origin.y + dir.y * hit_t;
        const float to_z = origin.z + dir.z * hit_t;
        if (!std::isfinite(to_x) || !std::isfinite(to_y) || !std::isfinite(to_z))
            continue;

        append_ray_debug_segment(out_segments, from, Vector3(to_x, to_y, to_z), 0);
    }
}

} // namespace godot
