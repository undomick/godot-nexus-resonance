#ifndef RESONANCE_BAKER_H
#define RESONANCE_BAKER_H

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <phonon.h>

#include "resonance_constants.h"
#include "resonance_probe_data.h"
#include "resonance_utils.h"

#include <godot_cpp/variant/dictionary.hpp>

namespace godot {

/// Bakes block the calling thread; progress_callback runs synchronously on it.
class ResonanceBaker {
  public:
    ResonanceBaker() = default;
    ~ResonanceBaker() = default;
    ResonanceBaker(const ResonanceBaker&) = delete;
    ResonanceBaker& operator=(const ResonanceBaker&) = delete;
    ResonanceBaker(ResonanceBaker&&) = delete;
    ResonanceBaker& operator=(ResonanceBaker&&) = delete;

    enum ProbeGenType { GEN_CENTROID = 0,
                        GEN_UNIFORM_FLOOR = 1,
                        GEN_VOLUME = 2 };

    PackedVector3Array generate_manual_grid(
        const Transform3D& volume_transform,
        Vector3 extents,
        float spacing,
        int generation_type,
        float height_above_floor);

    /// Supports GEN_CENTROID and GEN_UNIFORM_FLOOR only; GEN_VOLUME goes through bake_manual_grid.
    /// pathing_scheduled defers the save to disk until the pathing bake finishes.
    bool bake_with_probe_array(
        IPLContext context,
        IPLScene scene,
        IPLSceneType scene_type,
        IPLOpenCLDevice opencl_device,
        IPLRadeonRaysDevice radeon_rays_device,
        const Transform3D& volume_transform,
        Vector3 extents,
        float spacing,
        int generation_type,
        float height_above_floor,
        int num_bounces,
        int num_rays,
        int reflection_type,
        Ref<ResonanceProbeData> probe_data_res,
        void (*progress_callback)(float, void*) = nullptr,
        void* progress_user_data = nullptr,
        bool pathing_scheduled = false,
        int num_threads = 2,
        int ambisonics_order = resonance::kBakeDefaultAmbisonicsOrder);

    // spacing (clamped) becomes each probe's influence radius, as in Steam Audio's uniform floor placement.
    // The GPU devices are needed only for the Radeon Rays scene type.
    bool bake_manual_grid(
        IPLContext context,
        IPLScene scene,
        IPLSceneType scene_type,
        IPLOpenCLDevice opencl_device,
        IPLRadeonRaysDevice radeon_rays_device,
        const PackedVector3Array& points,
        float spacing,
        int num_bounces,
        int num_rays,
        int reflection_type,
        Ref<ResonanceProbeData> probe_data_res,
        void (*progress_callback)(float, void*) = nullptr,
        void* progress_user_data = nullptr,
        bool pathing_scheduled = false,
        int num_threads = 2,
        int ambisonics_order = resonance::kBakeDefaultAmbisonicsOrder);

    bool bake_pathing(
        IPLContext context,
        IPLScene scene,
        Ref<ResonanceProbeData> probe_data_res,
        float vis_range = 500.0f,
        float path_range = 100.0f,
        int num_samples = 16,
        float radius = 0.5f,
        float threshold = 0.1f,
        void (*progress_callback)(float, void*) = nullptr,
        void* progress_user_data = nullptr,
        int num_threads = 2);

    /// Probes must already exist. influence_radius limits which probes get data.
    bool bake_static_source(
        IPLContext context,
        IPLScene scene,
        IPLSceneType scene_type,
        IPLOpenCLDevice opencl_device,
        IPLRadeonRaysDevice radeon_rays_device,
        Ref<ResonanceProbeData> probe_data_res,
        Vector3 endpoint_position,
        float influence_radius,
        int num_bounces = 4,
        int num_rays = 4096,
        void (*progress_callback)(float, void*) = nullptr,
        void* progress_user_data = nullptr,
        int num_threads = 2,
        int ambisonics_order = resonance::kBakeDefaultAmbisonicsOrder);

    bool bake_static_listener(
        IPLContext context,
        IPLScene scene,
        IPLSceneType scene_type,
        IPLOpenCLDevice opencl_device,
        IPLRadeonRaysDevice radeon_rays_device,
        Ref<ResonanceProbeData> probe_data_res,
        Vector3 endpoint_position,
        float influence_radius,
        int num_bounces = 4,
        int num_rays = 4096,
        void (*progress_callback)(float, void*) = nullptr,
        void* progress_user_data = nullptr,
        int num_threads = 2,
        int ambisonics_order = resonance::kBakeDefaultAmbisonicsOrder);

    /// -1 if the probe data fails to load.
    int32_t probe_data_get_num_probes(IPLContext context, Ref<ResonanceProbeData> probe_data_res) const;

    /// Clears the pathing hash. Volumes already using the resource need reload_probe_batch.
    bool probe_data_remove_probe_at_index(IPLContext context, Ref<ResonanceProbeData> probe_data_res, int32_t index) const;

    /// baked_data_type: 0 = reflections, 1 = pathing. variation: 0 reverb, 1 static source, 2 static listener, 3 dynamic.
    /// For static variations the endpoint sphere must match the original bake.
    bool probe_data_remove_baked_data_layer(IPLContext context, Ref<ResonanceProbeData> probe_data_res, int baked_data_type,
                                            int variation, Vector3 endpoint, float influence_radius) const;

    /// Inverse-distance-squared blend of the static source energy of probes near listener_position.
    float probe_data_static_source_interpolated_energy(IPLContext context, Ref<ResonanceProbeData> probe_data_res, Vector3 endpoint_position,
                                                       float influence_radius, Vector3 listener_position, float neighbor_radius_m,
                                                       int* out_probes_with_data = nullptr, int* out_probes_missing = nullptr) const;

    /// baked_variation: 0 reverb, 1 static source, 2 static listener.
    Dictionary probe_data_query_baked_at_probe_index(IPLContext context, Ref<ResonanceProbeData> probe_data_res, int32_t probe_index,
                                                     int baked_variation, Vector3 endpoint_position, float influence_radius,
                                                     bool reconstruct_ir, int sampling_rate) const;

    /// Inverse-distance-squared blend of the probes near world_position.
    Dictionary probe_data_query_baked_at_point(IPLContext context, Ref<ResonanceProbeData> probe_data_res, Vector3 world_position,
                                               int baked_variation, Vector3 endpoint_position, float influence_radius, float neighbor_radius_m,
                                               bool reconstruct_ir, int sampling_rate) const;

  private:
    bool _bake_static_endpoint(
        IPLContext context,
        IPLScene scene,
        IPLSceneType scene_type,
        IPLOpenCLDevice opencl_device,
        IPLRadeonRaysDevice radeon_rays_device,
        Ref<ResonanceProbeData> probe_data_res,
        Vector3 endpoint_position,
        float influence_radius,
        IPLBakedDataVariation variation,
        const char* error_prefix,
        const char* success_msg,
        int num_bounces = 4,
        int num_rays = 4096,
        void (*progress_callback)(float, void*) = nullptr,
        void* progress_user_data = nullptr,
        int num_threads = 2,
        int ambisonics_order = resonance::kBakeDefaultAmbisonicsOrder);
};

} // namespace godot

#endif