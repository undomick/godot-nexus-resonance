#include "resonance_constants.h"
#include "resonance_geometry_asset.h"
#include "resonance_ipl_guard.h"
#include "resonance_log.h"
#include "resonance_reflection_ir_fingerprint.h"
#include "resonance_server.h"
#include "resonance_utils.h"
#include <cstdint>
#include <cstring>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <set>

using namespace godot;

namespace {
void bake_progress_callback(float p, void* ud) {
    static_cast<godot::ResonanceServer*>(ud)->emit_bake_progress(p);
}

void release_bake_temp_scene(IPLStaticMesh temp_mesh, IPLScene temp_scene) {
    if (temp_mesh && temp_scene) {
        iplStaticMeshRemove(temp_mesh, temp_scene);
        iplStaticMeshRelease(&temp_mesh);
    }
    if (temp_scene)
        iplSceneRelease(&temp_scene);
}
} // namespace

PackedVector3Array ResonanceServer::generate_manual_grid(const Transform3D& tr, Vector3 extents, float spacing,
                                                         int generation_type, float height_above_floor) {
    return baker.generate_manual_grid(tr, extents, spacing, generation_type, height_above_floor);
}

PackedVector3Array ResonanceServer::generate_probes_scene_aware(const Transform3D& volume_transform, Vector3 extents,
                                                                float spacing, int generation_type, float height_above_floor) {
    PackedVector3Array out;
    if (!_ctx())
        return out;
    if (generation_type != ResonanceBaker::GEN_CENTROID && generation_type != ResonanceBaker::GEN_UNIFORM_FLOOR)
        return out;
    IPLScene temp_scene = nullptr;
    IPLStaticMesh temp_mesh = nullptr;
    IPLScene bake_scene = nullptr;
    bool bake_uses_live_scene = false;
    {
        std::lock_guard<std::mutex> lock(simulation_mutex);
        if (scene_dirty) {
            iplSceneCommit(scene);
            scene_dirty = false;
        }
        bake_scene = _prepare_bake_scene(&temp_scene, &temp_mesh);
        bake_uses_live_scene = (temp_scene == nullptr && bake_scene == scene);
    }
    if (!bake_scene)
        return out;
    IPLProbeArray probeArray = nullptr;
    if (iplProbeArrayCreate(_ctx(), &probeArray) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceServer: iplProbeArrayCreate failed (generate_probes_scene_aware).");
        release_bake_temp_scene(temp_mesh, temp_scene);
        return out;
    }
    IPLProbeGenerationParams genParams{};
    genParams.type = (generation_type == ResonanceBaker::GEN_CENTROID) ? IPL_PROBEGENERATIONTYPE_CENTROID : IPL_PROBEGENERATIONTYPE_UNIFORMFLOOR;
    genParams.spacing = spacing;
    genParams.height = height_above_floor;
    genParams.transform = ResonanceUtils::create_volume_transform_rotated(volume_transform, extents);
    if (bake_uses_live_scene) {
        std::lock_guard<std::mutex> lock(simulation_mutex);
        iplProbeArrayGenerateProbes(probeArray, bake_scene, &genParams);
    } else {
        iplProbeArrayGenerateProbes(probeArray, bake_scene, &genParams);
    }
    int num_probes = iplProbeArrayGetNumProbes(probeArray);
    for (int i = 0; i < num_probes; i++) {
        IPLSphere sphere = iplProbeArrayGetProbe(probeArray, i);
        out.push_back(ResonanceUtils::to_godot_vector3(sphere.center));
    }
    iplProbeArrayRelease(&probeArray);
    release_bake_temp_scene(temp_mesh, temp_scene);
    return out;
}

void ResonanceServer::set_bake_pipeline_pathing(bool p_pathing) {
    _bake_pipeline_pathing = p_pathing;
}

void ResonanceServer::set_bake_static_scene_asset(const Ref<ResonanceGeometryAsset>& p_asset) {
    _bake_static_scene_asset = p_asset;
}

void ResonanceServer::clear_static_scenes() {
    if (!_ctx() || !scene)
        return;
    std::lock_guard<std::mutex> lock(simulation_mutex);
    RuntimeSceneState state(_runtime_static_meshes, _runtime_static_triangle_count, _runtime_static_debug_mesh_ids,
                            &global_triangle_count, &scene_dirty, _runtime_static_sub_scenes, _runtime_static_instanced_meshes);
    scene_manager_.clear_static_scenes(scene, &ray_trace_debug_context_, state);
    reset_spatial_audio_warmup_passes();
}

void ResonanceServer::add_static_scene_from_asset(const Ref<ResonanceGeometryAsset>& p_asset, const Transform3D& p_transform) {
    if (!_ctx() || !scene)
        return;
    if (_scene_type() == IPL_SCENETYPE_CUSTOM) {
        UtilityFunctions::push_warning(
            "Nexus Resonance: add_static_scene_from_asset has no effect when scene_type is Custom (Godot Physics).");
        return;
    }
    std::lock_guard<std::mutex> lock(simulation_mutex);
    RuntimeSceneState state(_runtime_static_meshes, _runtime_static_triangle_count, _runtime_static_debug_mesh_ids,
                            &global_triangle_count, &scene_dirty, _runtime_static_sub_scenes, _runtime_static_instanced_meshes);
    scene_manager_.add_static_scene_from_asset(_ctx(), scene, p_asset, &ray_trace_debug_context_,
                                               wants_debug_reflection_viz(), state, p_transform, _tracer_type_for_mesh_operations(), _embree(), _radeon());
    reset_spatial_audio_warmup_passes();
}

void ResonanceServer::load_static_scene_from_asset(const Ref<ResonanceGeometryAsset>& p_asset, const Transform3D& p_transform) {
    if (!_ctx() || !scene)
        return;
    if (_scene_type() == IPL_SCENETYPE_CUSTOM) {
        UtilityFunctions::push_warning(
            "Nexus Resonance: load_static_scene_from_asset has no effect when scene_type is Custom (Godot Physics).");
        return;
    }
    std::lock_guard<std::mutex> lock(simulation_mutex);
    RuntimeSceneState state(_runtime_static_meshes, _runtime_static_triangle_count, _runtime_static_debug_mesh_ids,
                            &global_triangle_count, &scene_dirty, _runtime_static_sub_scenes, _runtime_static_instanced_meshes);
    scene_manager_.load_static_scene_from_asset(_ctx(), scene, p_asset, &ray_trace_debug_context_,
                                                wants_debug_reflection_viz(), state, p_transform, _tracer_type_for_mesh_operations(), _embree(), _radeon());
    reset_spatial_audio_warmup_passes();
}

void ResonanceServer::set_bake_params(Dictionary params) {
    if (params.has("bake_num_rays"))
        _bake_num_rays = (int)params["bake_num_rays"];
    if (params.has("bake_num_bounces"))
        _bake_num_bounces = (int)params["bake_num_bounces"];
    if (params.has("bake_num_threads"))
        _bake_num_threads = (int)params["bake_num_threads"];
    if (params.has("bake_reflection_type"))
        _bake_reflection_type = (int)params["bake_reflection_type"];
    if (params.has("bake_pathing_vis_range"))
        _bake_pathing_vis_range = (float)params["bake_pathing_vis_range"];
    if (params.has("bake_pathing_path_range"))
        _bake_pathing_path_range = (float)params["bake_pathing_path_range"];
    if (params.has("bake_pathing_num_samples"))
        _bake_pathing_num_samples = (int)params["bake_pathing_num_samples"];
    if (params.has("bake_pathing_radius"))
        _bake_pathing_radius = (float)params["bake_pathing_radius"];
    if (params.has("bake_pathing_threshold"))
        _bake_pathing_threshold = (float)params["bake_pathing_threshold"];
    if (params.has("bake_ambisonics_order"))
        _bake_ambisonics_order = (int)params["bake_ambisonics_order"];
}

int ResonanceServer::_get_bake_num_rays() const {
    if (_bake_num_rays >= 0)
        return _bake_num_rays;
    ProjectSettings* ps = ProjectSettings::get_singleton();
    if (ps)
        return (int)ps->get_setting(String(resonance::kProjectSettingsResonancePrefix) + "bake_num_rays",
                                    resonance::kBakeDefaultNumRays);
    return resonance::kBakeDefaultNumRays;
}

int ResonanceServer::_get_bake_num_bounces() const {
    if (_bake_num_bounces >= 0)
        return _bake_num_bounces;
    ProjectSettings* ps = ProjectSettings::get_singleton();
    if (ps)
        return (int)ps->get_setting(String(resonance::kProjectSettingsResonancePrefix) + "bake_num_bounces",
                                    resonance::kBakeDefaultNumBounces);
    return resonance::kBakeDefaultNumBounces;
}

int ResonanceServer::_get_bake_num_threads() const {
    if (_bake_num_threads >= 0)
        return _bake_num_threads;
    ProjectSettings* ps = ProjectSettings::get_singleton();
    if (ps)
        return (int)ps->get_setting(String(resonance::kProjectSettingsResonancePrefix) + "bake_num_threads",
                                    resonance::kBakeDefaultNumThreads);
    return resonance::kBakeDefaultNumThreads;
}

int ResonanceServer::_get_bake_ambisonics_order() const {
    if (_bake_ambisonics_order >= 0)
        return resonance::clamp_bake_ambisonics_order(_bake_ambisonics_order);
    return resonance::kBakeDefaultAmbisonicsOrder;
}

int ResonanceServer::_get_bake_reflection_type() const {
    if (_bake_reflection_type >= 0)
        return _bake_reflection_type;
    ProjectSettings* ps = ProjectSettings::get_singleton();
    if (ps)
        return (int)ps->get_setting(String(resonance::kProjectSettingsResonancePrefix) + "bake_reflection_type", 2);
    return 2;
}

float ResonanceServer::_get_bake_pathing_param(const char* key, float default_val) const {
    if (strcmp(key, "bake_pathing_vis_range") == 0 && _bake_pathing_vis_range >= 0)
        return _bake_pathing_vis_range;
    if (strcmp(key, "bake_pathing_path_range") == 0 && _bake_pathing_path_range >= 0)
        return _bake_pathing_path_range;
    if (strcmp(key, "bake_pathing_radius") == 0 && _bake_pathing_radius >= 0)
        return _bake_pathing_radius;
    if (strcmp(key, "bake_pathing_threshold") == 0 && _bake_pathing_threshold >= 0)
        return _bake_pathing_threshold;
    ProjectSettings* ps = ProjectSettings::get_singleton();
    if (!ps)
        return default_val;
    String path = String(resonance::kProjectSettingsResonancePrefix) + key;
    return (float)ps->get_setting(path, default_val);
}

int ResonanceServer::_get_bake_pathing_num_samples() const {
    if (_bake_pathing_num_samples >= 0)
        return _bake_pathing_num_samples;
    ProjectSettings* ps = ProjectSettings::get_singleton();
    if (ps)
        return (int)ps->get_setting(String(resonance::kProjectSettingsResonancePrefix) + "bake_pathing_num_samples",
                                    resonance::kBakePathingDefaultNumSamples);
    return resonance::kBakePathingDefaultNumSamples;
}

IPLScene ResonanceServer::_prepare_bake_scene(IPLScene* out_temp_scene, IPLStaticMesh* out_temp_mesh) {
    *out_temp_scene = nullptr;
    *out_temp_mesh = nullptr;
    if (_bake_static_scene_asset.is_valid() && _bake_static_scene_asset->is_valid() && _ctx()) {
        IPLSceneSettings sceneSettings{};
        sceneSettings.type = _tracer_type_for_mesh_operations();
        sceneSettings.embreeDevice = _embree();
        sceneSettings.radeonRaysDevice = _radeon();
        IPLScene temp = nullptr;
        if (iplSceneCreate(_ctx(), &sceneSettings, &temp) != IPL_STATUS_SUCCESS) {
            ResonanceLog::error("ResonanceServer: iplSceneCreate failed (_prepare_bake_scene asset path).");
            return scene;
        }
        IPLSerializedObjectSettings serialSettings{};
        serialSettings.data = const_cast<IPLbyte*>(reinterpret_cast<const IPLbyte*>(_bake_static_scene_asset->get_data_ptr()));
        serialSettings.size = static_cast<IPLsize>(_bake_static_scene_asset->get_size());
        IPLSerializedObject serialObj = nullptr;
        if (iplSerializedObjectCreate(_ctx(), &serialSettings, &serialObj) != IPL_STATUS_SUCCESS) {
            ResonanceLog::error("ResonanceServer: iplSerializedObjectCreate failed (_prepare_bake_scene).");
            iplSceneRelease(&temp);
            return scene;
        }
        IPLScopedRelease<IPLSerializedObject> serialGuard(serialObj, iplSerializedObjectRelease);
        IPLStaticMesh loadMesh = nullptr;
        if (iplStaticMeshLoad(temp, serialObj, nullptr, nullptr, &loadMesh) != IPL_STATUS_SUCCESS) {
            ResonanceLog::error("ResonanceServer: iplStaticMeshLoad failed (_prepare_bake_scene).");
            iplSceneRelease(&temp);
            return scene;
        }
        iplStaticMeshAdd(loadMesh, temp);
        iplSceneCommit(temp);
        *out_temp_scene = temp;
        *out_temp_mesh = loadMesh;
        return temp;
    }
    return scene;
}

bool ResonanceServer::_with_bake_scene(std::function<bool(IPLScene)> bake_fn) {
    IPLScene temp_scene = nullptr;
    IPLStaticMesh temp_mesh = nullptr;
    IPLScene bake_scene = nullptr;
    bool bake_uses_live_scene = false;
    {
        std::lock_guard<std::mutex> lock(simulation_mutex);
        if (scene_dirty) {
            iplSceneCommit(scene);
            scene_dirty = false;
        }
        bake_scene = _prepare_bake_scene(&temp_scene, &temp_mesh);
        bake_uses_live_scene = (temp_scene == nullptr && bake_scene == scene);
    }
    if (!bake_scene)
        return false;
    bool ok = false;
    if (bake_uses_live_scene) {
        std::lock_guard<std::mutex> lock(simulation_mutex);
        ok = bake_fn(bake_scene);
    } else {
        ok = bake_fn(bake_scene);
    }
    release_bake_temp_scene(temp_mesh, temp_scene);
    return ok;
}

bool ResonanceServer::bake_manual_grid(const PackedVector3Array& points, Ref<ResonanceProbeData> data) {
    if (!_ctx() || !scene) {
        UtilityFunctions::push_error("Nexus Resonance Bake: Server not initialized.");
        return false;
    }
    if (!_bake_static_scene_asset.is_valid() || !_bake_static_scene_asset->is_valid()) {
        if (global_triangle_count.load(std::memory_order_acquire) <= 0) {
            UtilityFunctions::push_error("Nexus Resonance Bake: Scene not exported. Use Tools > Nexus Resonance > Export Static Scene before baking.");
            return false;
        }
    }
    int nb = _get_bake_num_bounces();
    int nr = _get_bake_num_rays();
    int bake_reflection = _get_bake_reflection_type();
    int nt = _get_bake_num_threads();
    int ao = _get_bake_ambisonics_order();
    return _with_bake_scene([this, &points, &data, nb, nr, bake_reflection, nt, ao](IPLScene bake_scene) {
        return baker.bake_manual_grid(_ctx(), bake_scene, _tracer_type_for_mesh_operations(), _opencl(), _radeon(), points, nb, nr, bake_reflection, data, bake_progress_callback, this, _bake_pipeline_pathing, nt, ao);
    });
}

bool ResonanceServer::bake_probes_for_volume(const Transform3D& volume_transform, Vector3 extents, float spacing,
                                             int generation_type, float height_above_floor, Ref<ResonanceProbeData> probe_data_res) {
    if (!_ctx() || !scene) {
        UtilityFunctions::push_error("Nexus Resonance Bake: Server not initialized.");
        return false;
    }
    if (!_bake_static_scene_asset.is_valid() || !_bake_static_scene_asset->is_valid()) {
        if (global_triangle_count.load(std::memory_order_acquire) <= 0) {
            UtilityFunctions::push_error("Nexus Resonance Bake: Scene not exported. Use Tools > Nexus Resonance > Export Static Scene before baking.");
            return false;
        }
    }
    int nb = _get_bake_num_bounces();
    int nr = _get_bake_num_rays();
    int bake_reflection = _get_bake_reflection_type();
    int nt = _get_bake_num_threads();
    int ao = _get_bake_ambisonics_order();
    return _with_bake_scene([this, volume_transform, extents, spacing, generation_type, height_above_floor, probe_data_res, nb, nr, bake_reflection, nt, ao](IPLScene bake_scene) {
        if (generation_type == ResonanceBaker::GEN_CENTROID || generation_type == ResonanceBaker::GEN_UNIFORM_FLOOR) {
            return baker.bake_with_probe_array(_ctx(), bake_scene, _tracer_type_for_mesh_operations(), _opencl(), _radeon(),
                                               volume_transform, extents, spacing, generation_type, height_above_floor,
                                               nb, nr, bake_reflection, probe_data_res, bake_progress_callback, this, _bake_pipeline_pathing, nt, ao);
        }
        PackedVector3Array points = baker.generate_manual_grid(volume_transform, extents, spacing, generation_type, height_above_floor);
        if (points.size() == 0)
            return false;
        return baker.bake_manual_grid(_ctx(), bake_scene, _tracer_type_for_mesh_operations(), _opencl(), _radeon(), points, nb, nr, bake_reflection, probe_data_res, bake_progress_callback, this, _bake_pipeline_pathing, nt, ao);
    });
}

bool ResonanceServer::bake_pathing(Ref<ResonanceProbeData> data) {
    if (!_ctx() || !scene)
        return false;
    if (data.is_null() || data->get_data().is_empty())
        return false;
    float vis_range = _get_bake_pathing_param("bake_pathing_vis_range", resonance::kBakePathingDefaultVisRange);
    float path_range = _get_bake_pathing_param("bake_pathing_path_range", resonance::kBakePathingDefaultPathRange);
    int num_samples = _get_bake_pathing_num_samples();
    float radius = _get_bake_pathing_param("bake_pathing_radius", resonance::kBakePathingDefaultRadius);
    float threshold = _get_bake_pathing_param("bake_pathing_threshold", resonance::kBakePathingDefaultThreshold);
    int nt = _get_bake_num_threads();
    return _with_bake_scene([this, data, vis_range, path_range, num_samples, radius, threshold, nt](IPLScene bake_scene) {
        return baker.bake_pathing(_ctx(), bake_scene, data, vis_range, path_range, num_samples, radius, threshold, bake_progress_callback, this, nt);
    });
}

bool ResonanceServer::bake_static_source(Ref<ResonanceProbeData> data, Vector3 endpoint_position, float influence_radius) {
    if (!_ctx() || !scene)
        return false;
    if (data.is_null() || data->get_data().is_empty())
        return false;
    int nb = _get_bake_num_bounces();
    int nr = _get_bake_num_rays();
    int nt = _get_bake_num_threads();
    int ao = _get_bake_ambisonics_order();
    return _with_bake_scene([this, data, endpoint_position, influence_radius, nb, nr, nt, ao](IPLScene bake_scene) {
        return baker.bake_static_source(_ctx(), bake_scene, _tracer_type_for_mesh_operations(), _opencl(), _radeon(),
                                        data, endpoint_position, influence_radius, nb, nr, bake_progress_callback, this, nt, ao);
    });
}

bool ResonanceServer::bake_static_listener(Ref<ResonanceProbeData> data, Vector3 endpoint_position, float influence_radius) {
    if (!_ctx() || !scene)
        return false;
    if (data.is_null() || data->get_data().is_empty())
        return false;
    int nb = _get_bake_num_bounces();
    int nr = _get_bake_num_rays();
    int nt = _get_bake_num_threads();
    int ao = _get_bake_ambisonics_order();
    return _with_bake_scene([this, data, endpoint_position, influence_radius, nb, nr, nt, ao](IPLScene bake_scene) {
        return baker.bake_static_listener(_ctx(), bake_scene, _tracer_type_for_mesh_operations(), _opencl(), _radeon(),
                                          data, endpoint_position, influence_radius, nb, nr, bake_progress_callback, this, nt, ao);
    });
}

void ResonanceServer::cancel_reflections_bake() {
    if (_ctx())
        iplReflectionsBakerCancelBake(_ctx());
}

void ResonanceServer::cancel_pathing_bake() {
    if (_ctx())
        iplPathBakerCancelBake(_ctx());
}

int32_t ResonanceServer::load_probe_batch(Ref<ResonanceProbeData> data) {
    if (!_ctx() || data.is_null()) {
        UtilityFunctions::push_warning("Nexus Resonance: load_probe_batch skipped (no context or null data)");
        return -1;
    }
    const int64_t probe_size = data->get_size();
    if (probe_size <= 0) {
        UtilityFunctions::push_warning("Nexus Resonance: load_probe_batch skipped - probe_data.data is empty! Re-bake the probes.");
        return -1;
    }
    const uint8_t* probe_ptr = data->get_data_ptr();
    if (probe_ptr == nullptr) {
        UtilityFunctions::push_warning("Nexus Resonance: load_probe_batch skipped - probe_data.data is empty! Re-bake the probes.");
        return -1;
    }

    int baked_type = data->get_baked_reflection_type();
    if (baked_type >= 0 && baked_type <= 2) {
        if (!_is_reflection_type_compatible(baked_type)) {
            static std::set<std::pair<int, int>> s_warned_mismatch;
            auto key = std::make_pair(baked_type, reflection_type);
            if (s_warned_mismatch.find(key) == s_warned_mismatch.end()) {
                s_warned_mismatch.insert(key);
                const char* baked_names[] = {"Convolution", "Parametric", "Hybrid"};
                const char* runt_names[] = {"Convolution", "Parametric", "Hybrid", "TrueAudio Next"};
                String err = String("Probe data was baked as ") + baked_names[baked_type] +
                             " but runtime is set to " + runt_names[(reflection_type >= resonance::kReflectionConvolution && reflection_type <= resonance::kReflectionTan) ? reflection_type : resonance::kReflectionConvolution] +
                             ". Re-bake probes with matching reflection type or change runtime ReflectionType to match.";
                UtilityFunctions::push_error(err);
                Engine* eng = Engine::get_singleton();
                if (eng && eng->has_singleton("ResonanceLogger")) {
                    Dictionary log_data;
                    log_data["baked_reflection_type"] = baked_type;
                    log_data["runtime_reflection_type"] = reflection_type;
                    resonance_logger_log("validation", err.utf8().get_data(), log_data);
                }
            }
            return -1;
        }
    }

    // Skip pathing validation when called from bake_probes() mid-pipeline: pathing is baked
    // in a later step, so hash is 0 at this point. Validation runs again on reload after save.
    bool skip_pathing_check = _bake_pipeline_pathing;
    if (!skip_pathing_check && pathing_enabled && data->get_pathing_params_hash() == 0) {
        static bool s_warned_pathing_mismatch = false;
        if (!s_warned_pathing_mismatch) {
            s_warned_pathing_mismatch = true;
            String err = "Nexus Resonance: Pathing is enabled but probe data has no pathing baked. Bake Pathing in the Probe Volume editor (enable Pathing in bake_config, then Bake).";
            UtilityFunctions::push_warning(err);
            Engine* eng_path = Engine::get_singleton();
            if (eng_path && eng_path->has_singleton("ResonanceLogger")) {
                Dictionary log_data;
                log_data["pathing_enabled"] = true;
                log_data["pathing_params_hash"] = 0;
                resonance_logger_log("validation", err.utf8().get_data(), log_data);
            }
        }
        return -1;
    }

    const size_t probe_size_u = static_cast<size_t>(probe_size);
    uint64_t data_hash = _hash_probe_data(probe_ptr, probe_size_u);
    int32_t handle = probe_batch_registry_.load_batch(_ctx(), simulator, &simulation_mutex, data, data_hash,
                                                      probe_ptr, probe_size);
    // Baked-only (max_rays==0): tick() only arms reflection heavy when batches exist. After reinit or first load,
    // without this the worker can skip RunReflections until the next interval, leaving reflections_have_run_once
    // false and fetch_reverb_params failing for parametric/hybrid (no wet). Request heavy immediately.
    if (handle >= 0) {
        reflection_sim_heavy_requested.store(true, std::memory_order_release);
        if (pathing_enabled) {
            pathing_sim_heavy_requested.store(true, std::memory_order_release);
        }
    }
    return handle;
}

void ResonanceServer::_clear_all_param_caches() {
    _clear_reverb_params_likely_available_hints();
    // Invalidate lock-free caches so audio thread won't use stale params after probe batch changes.
    const int reverb_back = 1 - reverb_param_cache_front_.load(std::memory_order_acquire);
    const int refl_back = 1 - reflection_param_cache_front_.load(std::memory_order_acquire);
    const int path_back = 1 - pathing_param_cache_front_.load(std::memory_order_acquire);
    reverb_param_cache_epoch_[reverb_back]++;
    reflection_param_cache_epoch_[refl_back]++;
    pathing_param_cache_epoch_[path_back]++;
    if (reverb_param_cache_epoch_[reverb_back] == 0u)
        reverb_param_cache_epoch_[reverb_back] = 1u;
    if (reflection_param_cache_epoch_[refl_back] == 0u)
        reflection_param_cache_epoch_[refl_back] = 1u;
    if (pathing_param_cache_epoch_[path_back] == 0u)
        pathing_param_cache_epoch_[path_back] = 1u;
    reverb_param_cache_front_.store(reverb_back, std::memory_order_release);
    reflection_param_cache_front_.store(refl_back, std::memory_order_release);
    pathing_param_cache_front_.store(path_back, std::memory_order_release);
    for (size_t i = 0; i < kMaxCacheHandles; ++i) {
        reflection_baked_energy_last_[i] = 0.0f;
        last_good_reflection_valid_[i].store(0, std::memory_order_relaxed);
    }
}

void ResonanceServer::remove_probe_batch(int32_t handle) {
    if (handle < 0 || is_shutting_down_flag.load(std::memory_order_acquire) || !_ctx() || !simulator)
        return;
    probe_batch_registry_.remove_batch(handle, simulator, &simulation_mutex);
    _clear_all_param_caches();
}

IPLProbeBatch ResonanceServer::_get_pathing_batch_for_source(int32_t preferred_handle) {
    return probe_batch_registry_.get_pathing_batch(preferred_handle);
}

bool ResonanceServer::_uses_convolution_or_hybrid_or_tan() const {
    return reflection_type == resonance::kReflectionConvolution || reflection_type == resonance::kReflectionHybrid || reflection_type == resonance::kReflectionTan;
}

bool ResonanceServer::_uses_parametric_or_hybrid() const {
    return reflection_type == resonance::kReflectionParametric || reflection_type == resonance::kReflectionHybrid;
}

bool ResonanceServer::_is_reflection_type_compatible(int baked_type) const {
    return (baked_type == 2) ||
           (baked_type == 0 && _uses_convolution_or_hybrid_or_tan()) ||
           (baked_type == 1 && _uses_parametric_or_hybrid());
}

bool ResonanceServer::_is_batch_compatible_with_config(int32_t handle) const {
    return probe_batch_registry_.is_compatible(handle, reflection_type, pathing_enabled);
}

int ResonanceServer::revalidate_probe_batches_with_config() {
    int n = probe_batch_registry_.revalidate_with_config(simulator, &simulation_mutex, reflection_type, pathing_enabled);
    if (n > 0) {
        _clear_all_param_caches();
    }
    return n;
}

void ResonanceServer::clear_probe_batches() {
    if (is_shutting_down_flag.load(std::memory_order_acquire) || !_ctx())
        return;
    probe_batch_registry_.clear_batches(simulator, &simulation_mutex);
    _clear_all_param_caches();
}
void ResonanceServer::emit_bake_progress(float progress) {
    emit_signal("bake_progress", progress);
}

int32_t ResonanceServer::editor_probe_data_get_num_probes(Ref<ResonanceProbeData> data) const {
    return baker.probe_data_get_num_probes(_ctx(), data);
}

bool ResonanceServer::editor_probe_data_remove_probe(Ref<ResonanceProbeData> data, int32_t index) {
    if (!_ctx() || data.is_null())
        return false;
    return baker.probe_data_remove_probe_at_index(_ctx(), data, index);
}

bool ResonanceServer::editor_probe_data_remove_baked_layer(Ref<ResonanceProbeData> data, int baked_data_type, int variation,
                                                           Vector3 endpoint, float influence_radius) {
    if (!_ctx() || data.is_null())
        return false;
    return baker.probe_data_remove_baked_data_layer(_ctx(), data, baked_data_type, variation, endpoint, influence_radius);
}

float ResonanceServer::probe_data_static_source_energy_at(Ref<ResonanceProbeData> data, Vector3 endpoint, Vector3 listener,
                                                          float influence_radius, float neighbor_radius) {
    if (!_ctx() || data.is_null())
        return 0.0f;
    if (neighbor_radius <= 0.0f)
        neighbor_radius = resonance::kStaticSourceProbeNeighborRadiusM;
    int with_data = 0;
    int missing = 0;
    const float energy = baker.probe_data_static_source_interpolated_energy(_ctx(), data, endpoint, influence_radius, listener,
                                                                            neighbor_radius, &with_data, &missing);
    if (missing > 0 && with_data == 0) {
        UtilityFunctions::push_warning(
            "Nexus Resonance: STATICSOURCE audit at listener " + listener.operator String() +
            " found " + String::num_int64(missing) +
            " neighboring probes without baked energy for endpoint " + endpoint.operator String() +
            ". Re-bake static source pass (ResonanceBakeRunner.run_bake).");
    }
    return energy;
}

uint16_t ResonanceServer::get_reflection_baked_energy_q16(int32_t handle) const {
    if (handle < 0 || handle >= kMaxCacheHandles)
        return 0;
    return reflection_baked_energy_to_q16(reflection_baked_energy_last_[static_cast<size_t>(handle)]);
}
