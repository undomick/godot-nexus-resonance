#ifndef RAY_TRACE_DEBUG_CONTEXT_H
#define RAY_TRACE_DEBUG_CONTEXT_H

#include "resonance_constants.h"
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <mutex>
#include <phonon.h>
#include <unordered_map>
#include <vector>

namespace godot {

class RayTraceDebugContext {
  public:
    static constexpr float kNormalLengthEpsilon = 1e-8f;
    static constexpr float kMinRayT = 0.001f;

    RayTraceDebugContext();
    ~RayTraceDebugContext();

    RayTraceDebugContext(const RayTraceDebugContext&) = delete;
    RayTraceDebugContext& operator=(const RayTraceDebugContext&) = delete;
    RayTraceDebugContext(RayTraceDebugContext&&) = delete;
    RayTraceDebugContext& operator=(RayTraceDebugContext&&) = delete;

    void clear();

    int register_mesh(const std::vector<IPLVector3>& vertices,
                      const std::vector<IPLTriangle>& triangles,
                      const IPLint32* material_indices,
                      const IPLMatrix4x4* transform,
                      const IPLMaterial* material);
    void unregister_mesh(int mesh_id);

    /// Cast rays from origin in uniform directions, trace against geometry, return segments for viz.
    /// Main-thread only. Used when Embree scene + debug_reflections (no CUSTOM callbacks).
    void trace_reflection_rays_for_viz(const IPLVector3& origin, int num_rays,
                                       float max_distance, Array& out_segments);

  private:
    struct TriangleData {
        IPLVector3 v0, v1, v2;
        IPLVector3 normal;
        int material_index;
    };
    std::vector<TriangleData> triangles_;
    std::vector<IPLMaterial> materials_;
    std::unordered_map<int, int> mesh_id_to_mat_offset_;
    int next_mesh_id_ = 1;
    std::mutex geometry_mutex_;

    bool ray_triangle_intersect(const IPLRay& ray, float t_min, float t_max,
                                const TriangleData& tri, float& out_t, IPLVector3& out_normal) const;
    bool closest_hit_along_ray(const IPLRay& ray, float t_min, float t_max, float& out_t) const;
};

} // namespace godot

#endif // RAY_TRACE_DEBUG_CONTEXT_H
