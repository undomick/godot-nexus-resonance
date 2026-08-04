#include "resonance_baker.h"
#include "resonance_constants.h"
#include "resonance_ipl_guard.h"
#include "resonance_log.h"
#include "resonance_reflection_ir_fingerprint.h"
#include <atomic>
#include <cmath>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/resource_uid.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <vector>

using namespace godot;

// Counter for unique `probe_batch_fallback_*` filenames when `probe_data` has no saved path.
static std::atomic<int> s_fallback_counter{0};

static bool _baker_on_main_thread() {
    OS* os = OS::get_singleton();
    return !os || os->get_thread_caller_id() == os->get_main_thread_id();
}

// Bake runs on a worker Thread; Godot print/push_* are main-thread only.
static void _baker_print_rich(const String& msg) {
    if (!_baker_on_main_thread())
        return;
    UtilityFunctions::print_rich(msg);
}

static void _baker_push_error(const String& msg) {
    if (!_baker_on_main_thread()) {
        ResonanceLog::error(msg);
        return;
    }
    UtilityFunctions::push_error(msg);
}

static void _baker_push_warning(const String& msg) {
    if (!_baker_on_main_thread()) {
        ResonanceLog::warn(msg);
        return;
    }
    UtilityFunctions::push_warning(msg);
}

/// Invalidate `_get_bake_params_hash` match so editor gizmos show "needs bake" (fixed sentinel; avoids XOR false positives).
static void invalidate_probe_data_bake_params_hash(const Ref<ResonanceProbeData>& probe_data_res) {
    if (probe_data_res.is_null())
        return;
    constexpr uint32_t kInvalidBakeParamsStamp = 0xDEADBEEFu;
    probe_data_res->set_bake_params_hash(static_cast<int64_t>(kInvalidBakeParamsStamp));
}

/// Maps addon reflection_type to IPL_REFLECTIONSBAKEFLAGS (conv / param / both).
static IPLReflectionsBakeFlags _bake_flags_from_reflection_type(int reflection_type) {
    if (reflection_type == resonance::kReflectionConvolution)
        return static_cast<IPLReflectionsBakeFlags>(IPL_REFLECTIONSBAKEFLAGS_BAKECONVOLUTION);
    if (reflection_type == resonance::kReflectionParametric)
        return static_cast<IPLReflectionsBakeFlags>(IPL_REFLECTIONSBAKEFLAGS_BAKEPARAMETRIC);
    return static_cast<IPLReflectionsBakeFlags>(IPL_REFLECTIONSBAKEFLAGS_BAKECONVOLUTION | IPL_REFLECTIONSBAKEFLAGS_BAKEPARAMETRIC);
}

static void _fill_reflections_bake_params(IPLReflectionsBakeParams& out,
                                          IPLScene scene, IPLProbeBatch probe_batch, IPLSceneType scene_type,
                                          IPLOpenCLDevice opencl_device, IPLRadeonRaysDevice radeon_rays_device,
                                          IPLBakedDataVariation variation, int num_rays, int num_bounces, int reflection_type, int num_threads,
                                          int ambisonics_order,
                                          const Vector3* endpoint_position = nullptr, float influence_radius = 0.0f) {
    out.scene = scene;
    out.probeBatch = probe_batch;
    out.sceneType = scene_type;
    out.openCLDevice = opencl_device;
    out.radeonRaysDevice = radeon_rays_device;
    out.identifier.type = IPL_BAKEDDATATYPE_REFLECTIONS;
    out.identifier.variation = variation;
    if (endpoint_position && influence_radius > 0.0f) {
        out.identifier.endpointInfluence.center = ResonanceUtils::to_ipl_vector3(*endpoint_position);
        out.identifier.endpointInfluence.radius = influence_radius;
    }
    out.bakeFlags = _bake_flags_from_reflection_type(reflection_type);
    out.numRays = num_rays;
    out.numBounces = num_bounces;
    out.numDiffuseSamples = resonance::kBakerNumDiffuseSamples;
    out.order = resonance::clamp_bake_ambisonics_order(ambisonics_order);
    out.simulatedDuration = resonance::kBakerSimulatedDuration;
    out.savedDuration = resonance::kBakerSimulatedDuration;
    out.numThreads = (num_threads < 1) ? 1 : num_threads;
    out.irradianceMinDistance = resonance::kBakerIrradianceMinDistance;
}

/// Deserialize `ResonanceProbeData` → `IPLProbeBatch` (caller must `iplProbeBatchRelease`).
static IPLProbeBatch _load_probe_batch_from_resource(IPLContext context, Ref<ResonanceProbeData> probe_data_res) {
    if (probe_data_res.is_null() || probe_data_res->get_data().is_empty())
        return nullptr;
    PackedByteArray pba = probe_data_res->get_data();
    IPLSerializedObjectSettings sSettings{};
    sSettings.data = reinterpret_cast<IPLbyte*>(pba.ptrw());
    sSettings.size = pba.size();
    IPLSerializedObject sObj = nullptr;
    if (iplSerializedObjectCreate(context, &sSettings, &sObj) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceBaker: iplSerializedObjectCreate failed (_load_probe_batch_from_resource).");
        return nullptr;
    }
    IPLScopedRelease<IPLSerializedObject> sObjGuard(sObj, iplSerializedObjectRelease);
    IPLProbeBatch batch = nullptr;
    if (iplProbeBatchLoad(context, sObj, &batch) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceBaker: iplProbeBatchLoad failed (_load_probe_batch_from_resource).");
        return nullptr;
    }
    iplProbeBatchCommit(batch);
    return batch;
}

static bool _save_probe_batch_to_probe_data(IPLContext context, IPLProbeBatch batch, Ref<ResonanceProbeData> probe_data_res) {
    IPLSerializedObjectSettings serial_settings{};
    IPLSerializedObject serialized_object = nullptr;
    if (iplSerializedObjectCreate(context, &serial_settings, &serialized_object) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceBaker: iplSerializedObjectCreate failed (_save_probe_batch_to_probe_data).");
        return false;
    }
    IPLScopedRelease<IPLSerializedObject> serial_guard(serialized_object, iplSerializedObjectRelease);
    iplProbeBatchSave(batch, serialized_object);
    IPLsize size = iplSerializedObjectGetSize(serialized_object);
    IPLbyte* data = iplSerializedObjectGetData(serialized_object);
    if (size == 0 || !data) {
        _baker_push_error("Nexus Resonance: Serialized probe batch is empty after edit.");
        return false;
    }
    PackedByteArray new_pba;
    new_pba.resize((int64_t)size);
    memcpy(new_pba.ptrw(), data, size);
    probe_data_res->set_data(new_pba);
    return true;
}

static String _build_tres_content(const PackedByteArray& pba, Ref<ResonanceProbeData> probe_data_res, int reflection_type) {
    int64_t bph = probe_data_res->get_bake_params_hash();
    int64_t pph = probe_data_res->get_pathing_params_hash();
    int64_t ssp = probe_data_res->get_static_source_params_hash();
    int64_t slp = probe_data_res->get_static_listener_params_hash();
    int64_t ssc = probe_data_res->get_static_scene_params_hash();
    String data_str = UtilityFunctions::var_to_str(pba);
    String probe_pos_str = UtilityFunctions::var_to_str(probe_data_res->get_probe_positions());
    return "[gd_resource type=\"ResonanceProbeData\" format=3]\n\n[resource]\ndata = " + data_str +
           "\nprobe_positions = " + probe_pos_str +
           "\nbake_params_hash = " + String::num_int64(bph) +
           "\nbaked_reflection_type = " + String::num_int64(reflection_type) +
           "\npathing_params_hash = " + String::num_int64(pph) +
           "\nstatic_source_params_hash = " + String::num_int64(ssp) +
           "\nstatic_listener_params_hash = " + String::num_int64(slp) +
           "\nstatic_scene_params_hash = " + String::num_int64(ssc) + "\n";
}

/// Disk I/O belongs on the main thread (ResonanceBakeRunner). Bake thread only updates in-memory probe_data.
static bool _save_probe_data_to_disk(Ref<ResonanceProbeData> probe_data_res, const String& path,
                                     const PackedByteArray& pba, int reflection_type, IPLsize size, bool pathing_scheduled) {
    if (path.is_empty() || pathing_scheduled)
        return true;
    OS* os = OS::get_singleton();
    if (os && os->get_thread_caller_id() != os->get_main_thread_id())
        return true;
    Error err = ResourceSaver::get_singleton()->save(probe_data_res, path, ResourceSaver::FLAG_CHANGE_PATH);
    if (err == OK)
        return true;
    const String fallback_ext = path.get_extension().to_lower() == String("res") ? String("res") : String("tres");
    const String text_fallback_path = path.get_basename() + "." + fallback_ext;
    if (fallback_ext == String("res")) {
        _baker_push_error("ResonanceBaker: ResourceSaver failed for .res path (" + String::num_int64((int64_t)err) + "); cannot fall back to hand-written binary.");
        return false;
    }
    Ref<FileAccess> f = FileAccess::open(text_fallback_path, FileAccess::WRITE);
    if (!f.is_valid()) {
        _baker_push_error("ResonanceBaker: Could not save file. ResourceSaver failed (" + String::num_int64((int64_t)err) + ") and fallback open failed.");
        return false;
    }
    String content = _build_tres_content(pba, probe_data_res, reflection_type);
    f->store_string(content);
    f->close();
    probe_data_res->take_over_path(text_fallback_path);
    return true;
}

/// UID → path; empty → ProjectSettings bake dir + numbered fallback (creates directories).
static String _resolve_save_path(Ref<ResonanceProbeData> probe_data_res) {
    String path = probe_data_res->get_path();
    if (!path.is_empty() && path.begins_with("uid://")) {
        path = ResourceUID::get_singleton()->uid_to_path(path);
    }
    if (path.is_empty()) {
        const String base_dir = resonance_bake_batches_dir_from_settings();
        int n = s_fallback_counter.fetch_add(1) + 1;
        const String ext = resonance_probe_data_save_extension_from_settings();
        path = base_dir + String("probe_batch_fallback_") + String::num_int64(n) + String(".") + ext;
        _baker_push_warning("Nexus Resonance Bake: probe_data has no path. Using fallback: " + path);
        String dir = path.get_base_dir();
        if (!dir.is_empty()) {
            ProjectSettings* ps2 = ProjectSettings::get_singleton();
            String abs_dir = ps2 ? ps2->globalize_path(dir) : dir;
            if (!abs_dir.is_empty()) {
                DirAccess::make_dir_recursive_absolute(abs_dir);
            }
        }
    }
    return path;
}

struct AdapterData {
    void (*cb)(float, void*);
    void* ud;
};

PackedVector3Array ResonanceBaker::generate_manual_grid(const Transform3D& volume_transform, Vector3 extents, float spacing,
                                                        int generation_type, float height_above_floor) {
    PackedVector3Array points;

    if (spacing <= resonance::kBakerMinSpacing)
        spacing = resonance::kBakerMinSpacing;
    Vector3 size = extents * 2.0f;

    if (generation_type == GEN_CENTROID) {
        // Centroid: one sample at the volume origin (IPL_PROBEGENERATIONTYPE_CENTROID).
        Vector3 local_center(0, 0, 0);
        Vector3 world_pos = volume_transform.xform(local_center);
        points.push_back(world_pos);
        return points;
    }

    if (generation_type == GEN_UNIFORM_FLOOR) {
        // Uniform floor: 2D grid on local y = -extents.y + height_above_floor.
        float plane_y = -extents.y + height_above_floor;
        int count_x = static_cast<int>(std::floor(size.x / spacing));
        int count_z = static_cast<int>(std::floor(size.z / spacing));
        if (count_x <= 0)
            count_x = 1;
        if (count_z <= 0)
            count_z = 1;

        float offset_x = (size.x < spacing) ? extents.x : spacing * 0.5f;
        float offset_z = (size.z < spacing) ? extents.z : spacing * 0.5f;

        for (int ix = 0; ix < count_x; ix++) {
            for (int iz = 0; iz < count_z; iz++) {
                Vector3 local_pos(-extents.x + (ix * spacing) + offset_x, plane_y, -extents.z + (iz * spacing) + offset_z);
                points.push_back(volume_transform.xform(local_pos));
            }
        }
        return points;
    }

    // Volume: full 3D grid (GEN_VOLUME).
    int count_x = static_cast<int>(std::floor(size.x / spacing));
    int count_y = static_cast<int>(std::floor(size.y / spacing));
    int count_z = static_cast<int>(std::floor(size.z / spacing));
    if (count_x <= 0)
        count_x = 1;
    if (count_y <= 0)
        count_y = 1;
    if (count_z <= 0)
        count_z = 1;

    Vector3 local_start = -extents;
    Vector3 offset(spacing * 0.5f, spacing * 0.5f, spacing * 0.5f);
    if (size.x < spacing)
        offset.x = extents.x;
    if (size.y < spacing)
        offset.y = extents.y;
    if (size.z < spacing)
        offset.z = extents.z;

    for (int ix = 0; ix < count_x; ix++) {
        for (int iy = 0; iy < count_y; iy++) {
            for (int iz = 0; iz < count_z; iz++) {
                Vector3 local_pos = local_start + Vector3(
                                                      (ix * spacing) + offset.x,
                                                      (iy * spacing) + offset.y,
                                                      (iz * spacing) + offset.z);
                points.push_back(volume_transform.xform(local_pos));
            }
        }
    }
    return points;
}

static void IPLCALL _ipl_progress_adapter(IPLfloat32 progress, void* userData) {
    AdapterData* ad = static_cast<AdapterData*>(userData);
    if (ad && ad->cb)
        ad->cb(static_cast<float>(progress), ad->ud);
}

bool ResonanceBaker::bake_with_probe_array(IPLContext context, IPLScene scene, IPLSceneType scene_type,
                                           IPLOpenCLDevice opencl_device, IPLRadeonRaysDevice radeon_rays_device,
                                           const Transform3D& volume_transform, Vector3 extents, float spacing,
                                           int generation_type, float height_above_floor,
                                           int num_bounces, int num_rays, int reflection_type,
                                           Ref<ResonanceProbeData> probe_data_res,
                                           void (*progress_callback)(float, void*), void* progress_user_data, bool pathing_scheduled, int num_threads,
                                           int ambisonics_order) {
    if (generation_type != GEN_CENTROID && generation_type != GEN_UNIFORM_FLOOR) {
        _baker_push_error("ResonanceBaker: bake_with_probe_array only supports Centroid (0) and UniformFloor (1). Use bake_manual_grid for Volume.");
        return false;
    }
    if (probe_data_res.is_null() || !context || !scene) {
        _baker_push_error("ResonanceBaker: bake_with_probe_array requires valid context, scene, and probe_data.");
        return false;
    }
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint()) {
        _baker_print_rich("[color=cyan]Nexus Resonance:[/color] Using Steam Audio Probe Array API (scene-aware placement)...");
    }
    IPLProbeArray probeArray = nullptr;
    if (iplProbeArrayCreate(context, &probeArray) != IPL_STATUS_SUCCESS) {
        _baker_push_error("ResonanceBaker: iplProbeArrayCreate failed.");
        return false;
    }
    IPLProbeGenerationParams genParams{};
    genParams.type = (generation_type == GEN_CENTROID) ? IPL_PROBEGENERATIONTYPE_CENTROID : IPL_PROBEGENERATIONTYPE_UNIFORMFLOOR;
    genParams.spacing = spacing;
    genParams.height = height_above_floor;
    genParams.transform = ResonanceUtils::create_volume_transform_rotated(volume_transform, extents);
    iplProbeArrayGenerateProbes(probeArray, scene, &genParams);
    int num_probes = iplProbeArrayGetNumProbes(probeArray);
    if (num_probes == 0) {
        iplProbeArrayRelease(&probeArray);
        // UNIFORMFLOOR from scene can yield 0 probes; use our manual floor grid in volume space.
        if (generation_type == GEN_UNIFORM_FLOOR) {
            if (eng && eng->is_editor_hint()) {
                _baker_print_rich("[color=cyan]Nexus Resonance:[/color] Steam Audio UniformFloor returned 0 probes (scene may have no detectable floor). Using flat-plane fallback - probes placed on horizontal plane in volume, NOT on ResonanceGeometry floor. Consider GEN_VOLUME for full 3D coverage.");
            }
            PackedVector3Array points = generate_manual_grid(volume_transform, extents, spacing, generation_type, height_above_floor);
            if (!points.is_empty()) {
                return bake_manual_grid(context, scene, scene_type, opencl_device, radeon_rays_device,
                                        points, num_bounces, num_rays, reflection_type, probe_data_res, progress_callback, progress_user_data, pathing_scheduled, num_threads, ambisonics_order);
            }
        }
        _baker_push_error("ResonanceBaker: Steam probe array generated 0 probes. Check volume and scene geometry.");
        return false;
    }
    if (eng && eng->is_editor_hint()) {
        _baker_print_rich("[color=cyan]Nexus Resonance:[/color] Probe array generated " + String::num(num_probes) + " probes.");
    }
    PackedVector3Array positions_for_viz;
    for (int i = 0; i < num_probes; i++) {
        IPLSphere sphere = iplProbeArrayGetProbe(probeArray, i);
        positions_for_viz.push_back(ResonanceUtils::to_godot_vector3(sphere.center));
    }
    IPLProbeBatch probeBatch = nullptr;
    if (iplProbeBatchCreate(context, &probeBatch) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceBaker: iplProbeBatchCreate failed (bake_with_probe_array).");
        iplProbeArrayRelease(&probeArray);
        return false;
    }
    IPLScopedRelease<IPLProbeArray> probeArrayGuard(probeArray, iplProbeArrayRelease);
    IPLScopedRelease<IPLProbeBatch> probeBatchGuard(probeBatch, iplProbeBatchRelease);
    iplProbeBatchAddProbeArray(probeBatch, probeArray);
    iplProbeBatchCommit(probeBatch);
    IPLReflectionsBakeParams bakeParams{};
    _fill_reflections_bake_params(bakeParams, scene, probeBatch, scene_type, opencl_device, radeon_rays_device,
                                  IPL_BAKEDDATAVARIATION_REVERB, num_rays, num_bounces, reflection_type, num_threads, ambisonics_order);
    AdapterData adapter = {progress_callback, progress_user_data};
    iplReflectionsBakerBake(context, &bakeParams,
                            (progress_callback && progress_user_data) ? _ipl_progress_adapter : nullptr,
                            (progress_callback && progress_user_data) ? &adapter : nullptr);
    IPLSerializedObjectSettings serialSettings{};
    IPLSerializedObject serializedObject = nullptr;
    if (iplSerializedObjectCreate(context, &serialSettings, &serializedObject) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceBaker: iplSerializedObjectCreate failed (bake_with_probe_array).");
        return false;
    }
    IPLScopedRelease<IPLSerializedObject> serialGuard(serializedObject, iplSerializedObjectRelease);
    iplProbeBatchSave(probeBatch, serializedObject);
    IPLsize size = iplSerializedObjectGetSize(serializedObject);
    IPLbyte* data = iplSerializedObjectGetData(serializedObject);
    if (size == 0 || !data) {
        _baker_push_error("Nexus Resonance Bake: iplReflectionsBakerBake produced no data. Possible causes: missing scene geometry, invalid probe positions, or invalid bake parameters. Check Steam Audio log (Godot Output) for details.");
        return false;
    }
    PackedByteArray pba;
    pba.resize((int64_t)size);
    memcpy(pba.ptrw(), data, size);
    probe_data_res->set_data(pba);
    probe_data_res->set_probe_positions(positions_for_viz);
    probe_data_res->set_baked_reflection_type(reflection_type);
    String path = _resolve_save_path(probe_data_res);
    if (!_save_probe_data_to_disk(probe_data_res, path, pba, reflection_type, size, pathing_scheduled)) {
        return false;
    }
    if (eng && eng->is_editor_hint() && !path.is_empty()) {
        int kb = (int)((size + 1023) / 1024);
        if (_baker_on_main_thread() && !pathing_scheduled) {
            _baker_print_rich("[color=cyan]Nexus Resonance:[/color] Saved " + String::num(kb) + " kilobytes (Probe Array bake).");
        } else {
            _baker_print_rich("[color=cyan]Nexus Resonance:[/color] Probe Array bake ready (" + String::num(kb) + " KB in memory).");
        }
    }
    return true;
}

bool ResonanceBaker::bake_manual_grid(IPLContext context, IPLScene scene, IPLSceneType scene_type, IPLOpenCLDevice opencl_device, IPLRadeonRaysDevice radeon_rays_device, const PackedVector3Array& probe_positions, int num_bounces, int num_rays, int reflection_type, Ref<ResonanceProbeData> probe_data_res, void (*progress_callback)(float, void*), void* progress_user_data, bool pathing_scheduled, int num_threads, int ambisonics_order) {
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint()) {
        const char* refl_name = (reflection_type == resonance::kReflectionConvolution) ? "Convolution" : (reflection_type == resonance::kReflectionParametric) ? "Parametric"
                                                                                                                                                               : "Hybrid";
        String msg = pathing_scheduled
                         ? String("Starting Bake (") + refl_name + " + Pathing) Process..."
                         : String("Starting Bake (") + refl_name + ") Process...";
        _baker_print_rich("[color=cyan]Nexus Resonance:[/color] " + msg);
    }

    if (probe_positions.size() == 0) {
        _baker_push_error("ResonanceBaker: No points to bake!");
        return false;
    }
    if (probe_positions.size() > resonance::kMaxProbesPerVolume) {
        _baker_push_error("ResonanceBaker: Probe count exceeds limit (" + String::num_int64((int64_t)resonance::kMaxProbesPerVolume) + "). Reduce spacing or volume size.");
        return false;
    }
    if (probe_data_res.is_null()) {
        _baker_push_error("ResonanceBaker: Resource is null.");
        return false;
    }
    if (!context || !scene) {
        _baker_push_error("ResonanceBaker: Steam Audio Context/Scene missing.");
        return false;
    }

    IPLProbeBatch probeBatch = nullptr;
    if (iplProbeBatchCreate(context, &probeBatch) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceBaker: iplProbeBatchCreate failed (bake_manual_grid).");
        return false;
    }

    for (int i = 0; i < probe_positions.size(); i++) {
        IPLSphere sphere{};
        sphere.center = ResonanceUtils::to_ipl_vector3(probe_positions[i]);
        sphere.radius = resonance::kBakerStaticEndpointSphereRadius;
        iplProbeBatchAddProbe(probeBatch, sphere);
    }
    iplProbeBatchCommit(probeBatch);

    if (eng && eng->is_editor_hint()) {
        _baker_print_rich("[color=cyan]Nexus Resonance:[/color] Batch committed with " + String::num((int)probe_positions.size()) + " probes. Calculating Reverb...");
    }

    // `_fill_reflections_bake_params` sets flags so conv/param/hybrid can run from one bake.
    IPLReflectionsBakeParams bakeParams{};
    _fill_reflections_bake_params(bakeParams, scene, probeBatch, scene_type, opencl_device, radeon_rays_device,
                                  IPL_BAKEDDATAVARIATION_REVERB, num_rays, num_bounces, reflection_type, num_threads, ambisonics_order);

    AdapterData adapter = {progress_callback, progress_user_data};
    iplReflectionsBakerBake(context, &bakeParams,
                            (progress_callback && progress_user_data) ? _ipl_progress_adapter : nullptr,
                            (progress_callback && progress_user_data) ? &adapter : nullptr);

    IPLScopedRelease<IPLProbeBatch> probeBatchGuard(probeBatch, iplProbeBatchRelease);
    IPLSerializedObjectSettings serialSettings{};
    IPLSerializedObject serializedObject = nullptr;
    if (iplSerializedObjectCreate(context, &serialSettings, &serializedObject) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceBaker: iplSerializedObjectCreate failed (bake_manual_grid).");
        return false;
    }
    IPLScopedRelease<IPLSerializedObject> serialGuard(serializedObject, iplSerializedObjectRelease);
    iplProbeBatchSave(probeBatch, serializedObject);

    IPLsize size = iplSerializedObjectGetSize(serializedObject);
    IPLbyte* data = iplSerializedObjectGetData(serializedObject);
    if (size == 0 || !data) {
        _baker_push_error("Nexus Resonance Bake: iplReflectionsBakerBake produced no data. Possible causes: missing scene geometry (add ResonanceGeometry nodes), invalid probe positions, or invalid bake parameters. Check Steam Audio log (Godot Output) for details.");
        return false;
    }

    PackedByteArray pba;
    pba.resize((int64_t)size);
    memcpy(pba.ptrw(), data, size);

    probe_data_res->set_data(pba);
    probe_data_res->set_probe_positions(probe_positions);
    probe_data_res->set_baked_reflection_type(reflection_type);

    String path = _resolve_save_path(probe_data_res);
    if (!_save_probe_data_to_disk(probe_data_res, path, pba, reflection_type, size, pathing_scheduled)) {
        return false;
    }
    if (eng && eng->is_editor_hint() && !path.is_empty()) {
        int kb = (int)((size + 1023) / 1024);
        if (_baker_on_main_thread() && !pathing_scheduled) {
            _baker_print_rich("[color=cyan]Nexus Resonance:[/color] Saved " + String::num(kb) + " kilobytes to " + path);
        } else {
            _baker_print_rich("[color=cyan]Nexus Resonance:[/color] Manual grid bake ready (" + String::num(kb) + " KB in memory).");
        }
    }

    return true;
}

bool ResonanceBaker::bake_pathing(IPLContext context, IPLScene scene, Ref<ResonanceProbeData> probe_data_res,
                                  float vis_range, float path_range, int num_samples, float radius, float threshold,
                                  void (*progress_callback)(float, void*), void* progress_user_data, int num_threads) {
    if (probe_data_res.is_null() || probe_data_res->get_data().is_empty()) {
        _baker_push_error("Nexus Resonance: bake_pathing requires probe_data with existing baked reflections.");
        return false;
    }
    if (!context || !scene) {
        _baker_push_error("Nexus Resonance: bake_pathing requires initialized context and scene.");
        return false;
    }

    IPLProbeBatch batch = _load_probe_batch_from_resource(context, probe_data_res);
    if (!batch) {
        _baker_push_error("Nexus Resonance: bake_pathing failed to load probe batch from probe_data.");
        return false;
    }
    IPLScopedRelease<IPLProbeBatch> batchGuard(batch, iplProbeBatchRelease);

    IPLPathBakeParams pathParams{};
    pathParams.scene = scene;
    pathParams.probeBatch = batch;
    pathParams.identifier.type = IPL_BAKEDDATATYPE_PATHING;
    pathParams.identifier.variation = IPL_BAKEDDATAVARIATION_DYNAMIC;
    pathParams.numSamples = num_samples;
    pathParams.radius = radius;
    pathParams.threshold = threshold;
    pathParams.visRange = vis_range;
    pathParams.pathRange = path_range;
    pathParams.numThreads = (num_threads < 1) ? 1 : num_threads;

    AdapterData adapter = {progress_callback, progress_user_data};
    iplPathBakerBake(context, &pathParams,
                     (progress_callback && progress_user_data) ? _ipl_progress_adapter : nullptr,
                     (progress_callback && progress_user_data) ? &adapter : nullptr);

    IPLSerializedObjectSettings serialSettings{};
    IPLSerializedObject serializedObject = nullptr;
    if (iplSerializedObjectCreate(context, &serialSettings, &serializedObject) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceBaker: iplSerializedObjectCreate failed (bake_pathing).");
        return false;
    }
    IPLScopedRelease<IPLSerializedObject> serialGuard(serializedObject, iplSerializedObjectRelease);
    iplProbeBatchSave(batch, serializedObject);

    IPLsize size = iplSerializedObjectGetSize(serializedObject);
    IPLbyte* data = iplSerializedObjectGetData(serializedObject);
    if (size == 0 || !data) {
        _baker_push_error("Nexus Resonance: bake_pathing produced no data. Possible causes: insufficient probes, scene geometry blocking paths, or invalid pathing parameters (vis_range, path_range). Check Steam Audio log (Godot Output) for details.");
        return false;
    }
    PackedByteArray newPba;
    newPba.resize((int64_t)size);
    memcpy(newPba.ptrw(), data, size);
    probe_data_res->set_data(newPba);

    Engine* path_eng = Engine::get_singleton();
    if (path_eng && path_eng->is_editor_hint()) {
        _baker_print_rich("[color=cyan]Nexus Resonance:[/color] Pathing baked successfully.");
    }
    return true;
}

bool ResonanceBaker::_bake_static_endpoint(IPLContext context, IPLScene scene, IPLSceneType scene_type,
                                           IPLOpenCLDevice opencl_device, IPLRadeonRaysDevice radeon_rays_device,
                                           Ref<ResonanceProbeData> probe_data_res, Vector3 endpoint_position, float influence_radius,
                                           IPLBakedDataVariation variation, const char* error_prefix, const char* success_msg,
                                           int num_bounces, int num_rays, void (*progress_callback)(float, void*), void* progress_user_data, int num_threads,
                                           int ambisonics_order) {
    String prefix(error_prefix);
    if (probe_data_res.is_null() || probe_data_res->get_data().is_empty()) {
        _baker_push_error("Nexus Resonance: " + prefix + " requires probe_data with existing baked probes (Bake Probes first).");
        return false;
    }
    if (!context || !scene) {
        _baker_push_error("Nexus Resonance: " + prefix + " requires initialized context and scene.");
        return false;
    }
    if (influence_radius <= 0.0f)
        influence_radius = resonance::kBakerStaticEndpointInfluenceFallback;

    IPLProbeBatch batch = _load_probe_batch_from_resource(context, probe_data_res);
    if (!batch) {
        _baker_push_error("Nexus Resonance: " + prefix + " failed to load probe batch.");
        return false;
    }
    IPLScopedRelease<IPLProbeBatch> batchGuard(batch, iplProbeBatchRelease);

    int baked_type = probe_data_res->get_baked_reflection_type();
    int refl_type = baked_type >= 0 ? baked_type : 2;
    IPLReflectionsBakeParams bakeParams{};
    _fill_reflections_bake_params(bakeParams, scene, batch, scene_type, opencl_device, radeon_rays_device,
                                  variation, num_rays, num_bounces, refl_type, num_threads, ambisonics_order,
                                  &endpoint_position, influence_radius);

    AdapterData adapter = {progress_callback, progress_user_data};
    iplReflectionsBakerBake(context, &bakeParams,
                            (progress_callback && progress_user_data) ? _ipl_progress_adapter : nullptr,
                            (progress_callback && progress_user_data) ? &adapter : nullptr);

    IPLSerializedObjectSettings serialSettings{};
    IPLSerializedObject serializedObject = nullptr;
    if (iplSerializedObjectCreate(context, &serialSettings, &serializedObject) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceBaker: iplSerializedObjectCreate failed (" + prefix + ").");
        return false;
    }
    IPLScopedRelease<IPLSerializedObject> serialGuard(serializedObject, iplSerializedObjectRelease);
    iplProbeBatchSave(batch, serializedObject);

    IPLsize size = iplSerializedObjectGetSize(serializedObject);
    IPLbyte* data = iplSerializedObjectGetData(serializedObject);
    if (size == 0 || !data) {
        _baker_push_error("Nexus Resonance: " + prefix + " produced no data. Possible causes: endpoint outside probe influence, missing scene geometry, or invalid parameters. Check Steam Audio log (Godot Output) for details.");
        return false;
    }
    PackedByteArray newPba;
    newPba.resize((int64_t)size);
    memcpy(newPba.ptrw(), data, size);
    probe_data_res->set_data(newPba);

    Engine* static_eng = Engine::get_singleton();
    if (static_eng && static_eng->is_editor_hint()) {
        _baker_print_rich("[color=cyan]Nexus Resonance:[/color] " + String(success_msg));
    }
    return true;
}

bool ResonanceBaker::bake_static_source(IPLContext context, IPLScene scene, IPLSceneType scene_type,
                                        IPLOpenCLDevice opencl_device, IPLRadeonRaysDevice radeon_rays_device,
                                        Ref<ResonanceProbeData> probe_data_res, Vector3 endpoint_position, float influence_radius,
                                        int num_bounces, int num_rays, void (*progress_callback)(float, void*), void* progress_user_data, int num_threads,
                                        int ambisonics_order) {
    return _bake_static_endpoint(context, scene, scene_type, opencl_device, radeon_rays_device,
                                 probe_data_res, endpoint_position, influence_radius,
                                 IPL_BAKEDDATAVARIATION_STATICSOURCE, "bake_static_source", "Static source baked successfully.",
                                 num_bounces, num_rays, progress_callback, progress_user_data, num_threads, ambisonics_order);
}

bool ResonanceBaker::bake_static_listener(IPLContext context, IPLScene scene, IPLSceneType scene_type,
                                          IPLOpenCLDevice opencl_device, IPLRadeonRaysDevice radeon_rays_device,
                                          Ref<ResonanceProbeData> probe_data_res, Vector3 endpoint_position, float influence_radius,
                                          int num_bounces, int num_rays, void (*progress_callback)(float, void*), void* progress_user_data, int num_threads,
                                          int ambisonics_order) {
    return _bake_static_endpoint(context, scene, scene_type, opencl_device, radeon_rays_device,
                                 probe_data_res, endpoint_position, influence_radius,
                                 IPL_BAKEDDATAVARIATION_STATICLISTENER, "bake_static_listener", "Static listener baked successfully.",
                                 num_bounces, num_rays, progress_callback, progress_user_data, num_threads, ambisonics_order);
}

int32_t ResonanceBaker::probe_data_get_num_probes(IPLContext context, Ref<ResonanceProbeData> probe_data_res) const {
    if (!context || probe_data_res.is_null() || probe_data_res->get_data().is_empty())
        return -1;
    IPLProbeBatch batch = _load_probe_batch_from_resource(context, probe_data_res);
    if (!batch)
        return -1;
    int32_t n = iplProbeBatchGetNumProbes(batch);
    iplProbeBatchRelease(&batch);
    return n;
}

bool ResonanceBaker::probe_data_remove_probe_at_index(IPLContext context, Ref<ResonanceProbeData> probe_data_res, int32_t index) const {
    if (!context || probe_data_res.is_null() || probe_data_res->get_data().is_empty()) {
        _baker_push_error("Nexus Resonance: probe_data_remove_probe_at_index requires context and non-empty probe data.");
        return false;
    }
    IPLProbeBatch batch = _load_probe_batch_from_resource(context, probe_data_res);
    if (!batch) {
        _baker_push_error("Nexus Resonance: probe_data_remove_probe_at_index failed to load probe batch.");
        return false;
    }
    IPLScopedRelease<IPLProbeBatch> batch_guard(batch, iplProbeBatchRelease);
    int32_t n = iplProbeBatchGetNumProbes(batch);
    if (index < 0 || index >= n) {
        if (n <= 0)
            _baker_push_error("Nexus Resonance: probe batch has no probes.");
        else
            _baker_push_error("Nexus Resonance: probe index out of range (0 .. " +
                              String::num_int64(static_cast<int64_t>(n - 1)) + ").");
        return false;
    }
    iplProbeBatchRemoveProbe(batch, index);
    iplProbeBatchCommit(batch);
    if (!_save_probe_batch_to_probe_data(context, batch, probe_data_res))
        return false;
    PackedVector3Array pos = probe_data_res->get_probe_positions();
    if (pos.size() == n) {
        pos.remove_at(index);
        probe_data_res->set_probe_positions(pos);
    }
    probe_data_res->set_pathing_params_hash(0);
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint()) {
        _baker_print_rich("[color=cyan]Nexus Resonance:[/color] Removed probe " +
                          String::num_int64(static_cast<int64_t>(index)) +
                          ". Re-bake pathing if needed; call reload_probe_batch on volumes using this resource.");
    }
    return true;
}

bool ResonanceBaker::probe_data_remove_baked_data_layer(IPLContext context, Ref<ResonanceProbeData> probe_data_res, int baked_data_type,
                                                        int variation, Vector3 endpoint, float influence_radius) const {
    if (!context || probe_data_res.is_null() || probe_data_res->get_data().is_empty()) {
        _baker_push_error("Nexus Resonance: probe_data_remove_baked_data_layer requires context and non-empty probe data.");
        return false;
    }
    if (baked_data_type < 0 || baked_data_type > 1) {
        _baker_push_error("Nexus Resonance: baked_data_type must be 0 (reflections) or 1 (pathing).");
        return false;
    }
    if (variation < 0 || variation > 3) {
        _baker_push_error("Nexus Resonance: variation must be 0–3 (reverb, static source, static listener, dynamic).");
        return false;
    }
    IPLBakedDataIdentifier id{};
    id.type = (baked_data_type == 0) ? IPL_BAKEDDATATYPE_REFLECTIONS : IPL_BAKEDDATATYPE_PATHING;
    id.variation = static_cast<IPLBakedDataVariation>(variation);
    if (id.variation == IPL_BAKEDDATAVARIATION_STATICSOURCE || id.variation == IPL_BAKEDDATAVARIATION_STATICLISTENER) {
        id.endpointInfluence.center = ResonanceUtils::to_ipl_vector3(endpoint);
        id.endpointInfluence.radius = influence_radius;
    }
    IPLProbeBatch batch = _load_probe_batch_from_resource(context, probe_data_res);
    if (!batch) {
        _baker_push_error("Nexus Resonance: probe_data_remove_baked_data_layer failed to load probe batch.");
        return false;
    }
    IPLScopedRelease<IPLProbeBatch> batch_guard(batch, iplProbeBatchRelease);
    IPLsize layer_size = iplProbeBatchGetDataSize(batch, &id);
    if (layer_size == 0) {
        _baker_push_warning(
            "Nexus Resonance: No baked data for the requested layer (type=" +
            String::num_int64(static_cast<int64_t>(baked_data_type)) + ", variation=" +
            String::num_int64(static_cast<int64_t>(variation)) + "). Nothing was removed.");
        return false;
    }
    iplProbeBatchRemoveData(batch, &id);
    iplProbeBatchCommit(batch);
    if (!_save_probe_batch_to_probe_data(context, batch, probe_data_res))
        return false;
    if (id.type == IPL_BAKEDDATATYPE_PATHING)
        probe_data_res->set_pathing_params_hash(0);
    if (id.type == IPL_BAKEDDATATYPE_REFLECTIONS && id.variation == IPL_BAKEDDATAVARIATION_REVERB)
        probe_data_res->set_baked_reflection_type(-1);
    if (id.type == IPL_BAKEDDATATYPE_REFLECTIONS && id.variation == IPL_BAKEDDATAVARIATION_STATICSOURCE)
        probe_data_res->set_static_source_params_hash(0);
    if (id.type == IPL_BAKEDDATATYPE_REFLECTIONS && id.variation == IPL_BAKEDDATAVARIATION_STATICLISTENER)
        probe_data_res->set_static_listener_params_hash(0);
    // Dropping pathing still shows probe layout as valid; only reflection layers invalidate gizmo bake state.
    if (id.type == IPL_BAKEDDATATYPE_REFLECTIONS)
        invalidate_probe_data_bake_params_hash(probe_data_res);
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint()) {
        _baker_print_rich(
            "[color=cyan]Nexus Resonance:[/color] Removed baked data layer. Call reload_probe_batch on volumes using this resource.");
    }
    return true;
}

float ResonanceBaker::probe_data_static_source_interpolated_energy(IPLContext context, Ref<ResonanceProbeData> probe_data_res,
                                                                   Vector3 endpoint_position, float influence_radius,
                                                                   Vector3 listener_position, float neighbor_radius_m,
                                                                   int* out_probes_with_data, int* out_probes_missing) const {
    if (out_probes_with_data)
        *out_probes_with_data = 0;
    if (out_probes_missing)
        *out_probes_missing = 0;
    if (!context || probe_data_res.is_null() || probe_data_res->get_data().is_empty())
        return 0.0f;
    if (influence_radius <= 0.0f)
        influence_radius = resonance::kBakerStaticEndpointInfluenceFallback;
    if (neighbor_radius_m <= 0.0f)
        neighbor_radius_m = resonance::kStaticSourceProbeNeighborRadiusM;

    IPLProbeBatch batch = _load_probe_batch_from_resource(context, probe_data_res);
    if (!batch)
        return 0.0f;
    IPLScopedRelease<IPLProbeBatch> batch_guard(batch, iplProbeBatchRelease);

    IPLBakedDataIdentifier id{};
    id.type = IPL_BAKEDDATATYPE_REFLECTIONS;
    id.variation = IPL_BAKEDDATAVARIATION_STATICSOURCE;
    id.endpointInfluence.center = ResonanceUtils::to_ipl_vector3(endpoint_position);
    id.endpointInfluence.radius = influence_radius;

    IPLEnergyFieldSettings ef_settings{};
    ef_settings.duration = resonance::kBakerSimulatedDuration;
    ef_settings.order = resonance::clamp_bake_ambisonics_order(resonance::kBakeDefaultAmbisonicsOrder);
    IPLEnergyField temp_field = nullptr;
    if (iplEnergyFieldCreate(context, &ef_settings, &temp_field) != IPL_STATUS_SUCCESS)
        return 0.0f;
    IPLScopedRelease<IPLEnergyField> field_guard(temp_field, iplEnergyFieldRelease);

    const PackedVector3Array probe_positions = probe_data_res->get_probe_positions();
    const int32_t batch_probe_count = iplProbeBatchGetNumProbes(batch);
    const int32_t count = static_cast<int32_t>(std::min(probe_positions.size(), static_cast<int64_t>(batch_probe_count)));

    struct Neighbor {
        int32_t index;
        float weight;
    };
    std::vector<Neighbor> neighbors;
    neighbors.reserve(16);
    float weight_sum = 0.0f;
    for (int32_t i = 0; i < count; ++i) {
        const float dist = probe_positions[i].distance_to(listener_position);
        if (dist > neighbor_radius_m)
            continue;
        const float w = 1.0f / std::max(dist, 0.1f);
        neighbors.push_back({i, w * w});
        weight_sum += neighbors.back().weight;
    }
    if (weight_sum <= 1e-8f || neighbors.empty())
        return 0.0f;

    double weighted_energy = 0.0;
    int with_data = 0;
    int missing = 0;
    for (const Neighbor& n : neighbors) {
        iplEnergyFieldReset(temp_field);
        iplProbeBatchGetEnergyField(batch, &id, n.index, temp_field);
        const float probe_energy = reflection_energy_field_total(temp_field);
        if (probe_energy > 1e-9f)
            with_data++;
        else
            missing++;
        weighted_energy += static_cast<double>(n.weight) * static_cast<double>(probe_energy);
    }
    weighted_energy /= static_cast<double>(weight_sum);
    if (out_probes_with_data)
        *out_probes_with_data = with_data;
    if (out_probes_missing)
        *out_probes_missing = missing;
    return static_cast<float>(weighted_energy);
}