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

namespace godot {

/// Probe placement grids and IPL bakes (reflection IRs, pathing, static endpoint variations) into `ResonanceProbeData`.
/// Optional `progress_callback` args are invoked synchronously on the calling thread (blocking bake).
class ResonanceBaker {
  public:
    ResonanceBaker() = default;
    ~ResonanceBaker() = default;
    ResonanceBaker(const ResonanceBaker&) = delete;
    ResonanceBaker& operator=(const ResonanceBaker&) = delete;
    ResonanceBaker(ResonanceBaker&&) = delete;
    ResonanceBaker& operator=(ResonanceBaker&&) = delete;

    // Generation type for probe placement
    enum ProbeGenType { GEN_CENTROID = 0,
                        GEN_UNIFORM_FLOOR = 1,
                        GEN_VOLUME = 2 };

    // Generate grid points in world space based on volume transform, extents, spacing, and generation type.
    PackedVector3Array generate_manual_grid(
        const Transform3D& volume_transform,
        Vector3 extents,
        float spacing,
        int generation_type,
        float height_above_floor);

    /// Scene-aware probe placement (`iplProbeArrayGenerateProbes`); only GEN_CENTROID / GEN_UNIFORM_FLOOR - use `bake_manual_grid` for GEN_VOLUME.
    /// `progress_callback`: optional synchronous progress; `pathing_scheduled`: defer disk save until pathing bake finishes.
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

    // Fixed probe positions → bake IRs. reflection_type selects convolution/parametric/hybrid bake flags; GPU devices needed for Radeon scene type.
    bool bake_manual_grid(
        IPLContext context,
        IPLScene scene,
        IPLSceneType scene_type,
        IPLOpenCLDevice opencl_device,
        IPLRadeonRaysDevice radeon_rays_device,
        const PackedVector3Array& points,
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

    /// Baked IRs for `IPL_BAKEDDATAVARIATION_STATICSOURCE` (probes must exist). `influence_radius` limits which probes get data.
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

    /// Same for `IPL_BAKEDDATAVARIATION_STATICLISTENER` (static listener endpoint).
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

    /// Probe count in serialized probe data, or -1 if load fails.
    int32_t probe_data_get_num_probes(IPLContext context, Ref<ResonanceProbeData> probe_data_res) const;

    /// `iplProbeBatchRemoveProbe` + reserialize; drop matching `probe_positions` entry; clears pathing hash - reload batch in runtime if live.
    bool probe_data_remove_probe_at_index(IPLContext context, Ref<ResonanceProbeData> probe_data_res, int32_t index) const;

    /// `iplProbeBatchRemoveData`: type 0 = reflections, 1 = pathing; variation 0–3 = reverb / static source / static listener / dynamic.
    /// Endpoint sphere must match the original bake for static variations.
    bool probe_data_remove_baked_data_layer(IPLContext context, Ref<ResonanceProbeData> probe_data_res, int baked_data_type,
                                            int variation, Vector3 endpoint, float influence_radius) const;

    /// Weighted sum of STATICSOURCE energy fields from probes near [listener_position] (audit / cliff diagnosis).
    float probe_data_static_source_interpolated_energy(IPLContext context, Ref<ResonanceProbeData> probe_data_res, Vector3 endpoint_position,
                                                       float influence_radius, Vector3 listener_position, float neighbor_radius_m,
                                                       int* out_probes_with_data = nullptr, int* out_probes_missing = nullptr) const;

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