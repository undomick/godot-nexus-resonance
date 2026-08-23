#include "resonance_constants.h"
#include "resonance_epoch.h"
#include "resonance_geometry_asset.h"
#include "resonance_ipl_guard.h"
#include "resonance_log.h"
#include "resonance_probe_exclusion_filter.h"
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

bool clone_static_mesh_into_scene(IPLContext ctx, IPLScene dest, IPLStaticMesh src, std::vector<IPLStaticMesh>& out_meshes) {
    if (!ctx || !dest || !src)
        return false;
    IPLSerializedObjectSettings serialSettings{};
    IPLSerializedObject serialObj = nullptr;
    if (iplSerializedObjectCreate(ctx, &serialSettings, &serialObj) != IPL_STATUS_SUCCESS)
        return false;
    iplStaticMeshSave(src, serialObj);
    IPLStaticMesh loaded = nullptr;
    const IPLerror load_err = iplStaticMeshLoad(dest, serialObj, nullptr, nullptr, &loaded);
    iplSerializedObjectRelease(&serialObj);
    if (load_err != IPL_STATUS_SUCCESS || !loaded)
        return false;
    iplStaticMeshAdd(loaded, dest);
    out_meshes.push_back(loaded);
    return true;
}

bool clone_sub_scene_as_instanced(IPLContext ctx, IPLScene dest, IPLScene src_sub, const IPLMatrix4x4& transform,
                                  IPLSceneType scene_type, IPLEmbreeDevice embree, IPLRadeonRaysDevice radeon,
                                  std::vector<IPLScene>& out_subs, std::vector<IPLInstancedMesh>& out_instanced) {
    if (!ctx || !dest || !src_sub)
        return false;
    IPLSerializedObjectSettings serialSettings{};
    IPLSerializedObject serialObj = nullptr;
    if (iplSerializedObjectCreate(ctx, &serialSettings, &serialObj) != IPL_STATUS_SUCCESS)
        return false;
    iplSceneSave(src_sub, serialObj);
    IPLSceneSettings subSettings{};
    subSettings.type = scene_type;
    subSettings.embreeDevice = embree;
    subSettings.radeonRaysDevice = radeon;
    IPLScene new_sub = nullptr;
    const IPLerror load_err = iplSceneLoad(ctx, &subSettings, serialObj, nullptr, nullptr, &new_sub);
    iplSerializedObjectRelease(&serialObj);
    if (load_err != IPL_STATUS_SUCCESS || !new_sub)
        return false;
    IPLInstancedMeshSettings instSettings{};
    instSettings.subScene = new_sub;
    instSettings.transform = transform;
    IPLInstancedMesh inst = nullptr;
    if (iplInstancedMeshCreate(dest, &instSettings, &inst) != IPL_STATUS_SUCCESS) {
        iplSceneRelease(&new_sub);
        return false;
    }
    iplInstancedMeshAdd(inst, dest);
    out_subs.push_back(new_sub);
    out_instanced.push_back(inst);
    return true;
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
    BakeSceneScratch scratch;
    {
        std::lock_guard<std::mutex> lock(simulation_mutex);
        if (scene_dirty) {
            iplSceneCommit(scene);
            scene_dirty.store(false, std::memory_order_release);
        }
        if (!_prepare_bake_scene(scratch))
            return out;
    }
    if (!scratch.scene)
        return out;
    IPLProbeArray probeArray = nullptr;
    if (iplProbeArrayCreate(_ctx(), &probeArray) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceServer: iplProbeArrayCreate failed (generate_probes_scene_aware).");
        _release_bake_scene_scratch(scratch);
        return out;
    }
    IPLProbeGenerationParams genParams{};
    genParams.type = (generation_type == ResonanceBaker::GEN_CENTROID) ? IPL_PROBEGENERATIONTYPE_CENTROID : IPL_PROBEGENERATIONTYPE_UNIFORMFLOOR;
    genParams.spacing = spacing;
    genParams.height = height_above_floor;
    genParams.transform = ResonanceUtils::create_volume_transform_rotated(volume_transform, extents);
    iplProbeArrayGenerateProbes(probeArray, scratch.scene, &genParams);
    int num_probes = iplProbeArrayGetNumProbes(probeArray);
    for (int i = 0; i < num_probes; i++) {
        IPLSphere sphere = iplProbeArrayGetProbe(probeArray, i);
        out.push_back(ResonanceUtils::to_godot_vector3(sphere.center));
    }
    iplProbeArrayRelease(&probeArray);
    _release_bake_scene_scratch(scratch);
    return out;
}

void ResonanceServer::set_bake_pipeline_pathing(bool p_pathing) {
    _bake_pipeline_pathing = p_pathing;
}

void ResonanceServer::set_bake_static_scene_asset(const Ref<ResonanceGeometryAsset>& p_asset) {
    _bake_static_scene_assets.clear();
    _bake_static_scene_transforms.clear();
    if (p_asset.is_valid() && p_asset->is_valid()) {
        _bake_static_scene_assets.push_back(p_asset);
        _bake_static_scene_transforms.push_back(Transform3D());
    }
}

void ResonanceServer::set_bake_static_scenes_from_assets(const TypedArray<ResonanceGeometryAsset>& assets,
                                                         const TypedArray<Transform3D>& transforms) {
    _bake_static_scene_assets.clear();
    _bake_static_scene_transforms.clear();
    const int n = assets.size();
    const int n_xf = transforms.size();
    for (int i = 0; i < n; i++) {
        Ref<ResonanceGeometryAsset> asset = assets[i];
        if (asset.is_null() || !asset->is_valid())
            continue;
        _bake_static_scene_assets.push_back(asset);
        _bake_static_scene_transforms.push_back((i < n_xf) ? Transform3D(transforms[i]) : Transform3D());
    }
}

bool ResonanceServer::_has_bake_static_scene_assets() const {
    for (const Ref<ResonanceGeometryAsset>& asset : _bake_static_scene_assets) {
        if (asset.is_valid() && asset->is_valid())
            return true;
    }
    return false;
}

void ResonanceServer::_release_static_pack_assume_locked(RuntimeStaticPack& pack) {
    if (pack.debug_id >= 0) {
        ray_trace_debug_context_.unregister_mesh(pack.debug_id);
        pack.debug_id = -1;
    }
    if (pack.instanced && scene) {
        iplInstancedMeshRemove(pack.instanced, scene);
        iplInstancedMeshRelease(&pack.instanced);
    }
    pack.instanced = nullptr;
    if (pack.mesh_in_sub && pack.sub_scene) {
        iplStaticMeshRemove(pack.mesh_in_sub, pack.sub_scene);
        iplStaticMeshRelease(&pack.mesh_in_sub);
    }
    pack.mesh_in_sub = nullptr;
    if (pack.sub_scene) {
        iplSceneRelease(&pack.sub_scene);
        pack.sub_scene = nullptr;
    }
    if (pack.tri_count > 0) {
        const int sub = pack.tri_count;
        const int prev = global_triangle_count.fetch_sub(sub, std::memory_order_release);
        if (prev < sub)
            global_triangle_count.store(0, std::memory_order_release);
        _runtime_static_triangle_count -= sub;
        if (_runtime_static_triangle_count < 0)
            _runtime_static_triangle_count = 0;
        pack.tri_count = 0;
    }
    scene_dirty.store(true, std::memory_order_release);
}

void ResonanceServer::_clear_static_packs_assume_locked() {
    for (auto& kv : _runtime_static_packs) {
        _release_static_pack_assume_locked(kv.second);
    }
    _runtime_static_packs.clear();
    _runtime_static_triangle_count = 0;
}

bool ResonanceServer::_create_instanced_static_pack_assume_locked(const Ref<ResonanceGeometryAsset>& asset,
                                                                  const Transform3D& transform, RuntimeStaticPack& out) {
    out = RuntimeStaticPack{};
    if (!asset.is_valid() || !asset->is_valid() || !scene || !_ctx())
        return false;

    std::vector<IPLStaticMesh> meshes;
    std::vector<IPLStaticMesh> meshes_in_sub;
    std::vector<IPLScene> subs;
    std::vector<IPLInstancedMesh> insts;
    std::vector<IPLMatrix4x4> xfs;
    std::vector<int> debug_ids;
    int tri = 0;
    // Do not touch global_triangle_count here - add_static_scene_from_asset updates it; we copy tri into the pack.
    std::atomic<int> unused_gtc{0};
    std::atomic<bool> unused_dirty{false};
    RuntimeSceneState state(meshes, tri, debug_ids, &unused_gtc, &unused_dirty, subs, insts, xfs, &meshes_in_sub);
    scene_manager_.add_static_scene_from_asset(_ctx(), scene, asset, &ray_trace_debug_context_, wants_debug_reflection_viz(),
                                               state, transform, _tracer_type_for_mesh_operations(), _embree(), _radeon(),
                                               true /* force_instanced */);
    if (insts.empty() || subs.empty())
        return false;

    // add_static_scene_from_asset with unused_gtc did not update global_triangle_count - fix that.
    const int added_tri = asset->get_triangle_count();
    if (added_tri > 0) {
        const int after = global_triangle_count.fetch_add(added_tri, std::memory_order_release) + added_tri;
        const int before = after - added_tri;
        if (resonance::spatial_audio_geometry_notify_should_arm_gate(before, after))
            arm_spatial_audio_output_gate();
    }
    scene_dirty.store(true, std::memory_order_release);

    out.sub_scene = subs[0];
    out.instanced = insts[0];
    out.transform = xfs[0];
    out.mesh_in_sub = meshes_in_sub.empty() ? nullptr : meshes_in_sub[0];
    out.debug_id = debug_ids.empty() ? -1 : debug_ids[0];
    out.tri_count = added_tri;
    // Prevent clear_static_scenes-style double-free: we stole the handles into out.
    return true;
}

void ResonanceServer::clear_static_scenes() {
    if (!_ctx() || !scene)
        return;
    std::lock_guard<std::mutex> lock(simulation_mutex);
    _clear_static_packs_assume_locked();
    arm_spatial_audio_output_gate();
}

void ResonanceServer::remove_static_pack(uint64_t object_id) {
    if (!_ctx() || !scene || object_id == 0)
        return;
    std::lock_guard<std::mutex> lock(simulation_mutex);
    auto it = _runtime_static_packs.find(object_id);
    if (it == _runtime_static_packs.end())
        return;
    _release_static_pack_assume_locked(it->second);
    _runtime_static_packs.erase(it);
}

void ResonanceServer::add_or_replace_static_pack(uint64_t object_id, const Ref<ResonanceGeometryAsset>& p_asset,
                                                 const Transform3D& p_transform) {
    if (!_ctx() || !scene || object_id == 0)
        return;
    if (_scene_type() == IPL_SCENETYPE_CUSTOM) {
        UtilityFunctions::push_warning(
            "Nexus Resonance: add_or_replace_static_pack has no effect when scene_type is Custom (Godot Physics).");
        return;
    }
    std::lock_guard<std::mutex> lock(simulation_mutex);
    if (!p_asset.is_valid() || !p_asset->is_valid()) {
        auto it = _runtime_static_packs.find(object_id);
        if (it != _runtime_static_packs.end()) {
            _release_static_pack_assume_locked(it->second);
            _runtime_static_packs.erase(it);
        }
        return;
    }
    // Build the replacement first so an IPL/create failure keeps the existing pack.
    RuntimeStaticPack pack;
    if (!_create_instanced_static_pack_assume_locked(p_asset, p_transform, pack)) {
        return;
    }
    auto it = _runtime_static_packs.find(object_id);
    if (it != _runtime_static_packs.end()) {
        _release_static_pack_assume_locked(it->second);
        _runtime_static_packs.erase(it);
    }
    _runtime_static_triangle_count += pack.tri_count;
    _runtime_static_packs.emplace(object_id, pack);
}

void ResonanceServer::replace_static_scenes_from_assets(const TypedArray<ResonanceGeometryAsset>& assets,
                                                        const TypedArray<Transform3D>& transforms) {
    if (!_ctx() || !scene)
        return;
    std::lock_guard<std::mutex> lock(simulation_mutex);
    _clear_static_packs_assume_locked();
    if (_scene_type() == IPL_SCENETYPE_CUSTOM) {
        if (assets.size() > 0) {
            UtilityFunctions::push_warning(
                "Nexus Resonance: replace_static_scenes_from_assets has no effect when scene_type is Custom (Godot Physics).");
        }
        arm_spatial_audio_output_gate();
        return;
    }
    const int n = assets.size();
    const int n_xf = transforms.size();
    for (int i = 0; i < n; i++) {
        Ref<ResonanceGeometryAsset> asset = assets[i];
        if (asset.is_null() || !asset->is_valid())
            continue;
        Transform3D xf = (i < n_xf) ? Transform3D(transforms[i]) : Transform3D();
        const uint64_t id = _next_ephemeral_static_pack_id++;
        RuntimeStaticPack pack;
        if (!_create_instanced_static_pack_assume_locked(asset, xf, pack))
            continue;
        _runtime_static_triangle_count += pack.tri_count;
        _runtime_static_packs.emplace(id, pack);
    }
    arm_spatial_audio_output_gate();
}

void ResonanceServer::add_static_scene_from_asset(const Ref<ResonanceGeometryAsset>& p_asset, const Transform3D& p_transform) {
    if (!p_asset.is_valid() || !p_asset->is_valid())
        return;
    const uint64_t id = _next_ephemeral_static_pack_id++;
    add_or_replace_static_pack(id, p_asset, p_transform);
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
    _clear_static_packs_assume_locked();
    if (!p_asset.is_valid() || !p_asset->is_valid()) {
        arm_spatial_audio_output_gate();
        return;
    }
    const uint64_t id = _next_ephemeral_static_pack_id++;
    RuntimeStaticPack pack;
    if (_create_instanced_static_pack_assume_locked(p_asset, p_transform, pack)) {
        _runtime_static_triangle_count += pack.tri_count;
        _runtime_static_packs.emplace(id, pack);
    }
    arm_spatial_audio_output_gate();
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

void ResonanceServer::_release_bake_scene_scratch(BakeSceneScratch& scratch) {
    for (IPLInstancedMesh& im : scratch.instanced) {
        if (im && scratch.scene) {
            iplInstancedMeshRemove(im, scratch.scene);
            iplInstancedMeshRelease(&im);
        }
        im = nullptr;
    }
    scratch.instanced.clear();
    for (IPLStaticMesh& m : scratch.meshes) {
        if (m && scratch.scene) {
            iplStaticMeshRemove(m, scratch.scene);
            iplStaticMeshRelease(&m);
        }
        m = nullptr;
    }
    scratch.meshes.clear();
    const size_t n_sub_mesh = scratch.meshes_in_sub.size();
    for (size_t i = 0; i < n_sub_mesh; i++) {
        IPLStaticMesh& m = scratch.meshes_in_sub[i];
        IPLScene sub = (i < scratch.sub_scenes.size()) ? scratch.sub_scenes[i] : nullptr;
        if (m && sub) {
            iplStaticMeshRemove(m, sub);
            iplStaticMeshRelease(&m);
        }
        m = nullptr;
    }
    scratch.meshes_in_sub.clear();
    for (IPLScene& sub : scratch.sub_scenes) {
        if (sub)
            iplSceneRelease(&sub);
        sub = nullptr;
    }
    scratch.sub_scenes.clear();
    if (scratch.scene) {
        iplSceneRelease(&scratch.scene);
        scratch.scene = nullptr;
    }
}

bool ResonanceServer::_prepare_bake_scene(BakeSceneScratch& out) {
    out = BakeSceneScratch{};
    if (!_ctx())
        return false;

    IPLSceneSettings sceneSettings{};
    sceneSettings.type = _tracer_type_for_mesh_operations();
    sceneSettings.embreeDevice = _embree();
    sceneSettings.radeonRaysDevice = _radeon();

    if (_has_bake_static_scene_assets()) {
        if (iplSceneCreate(_ctx(), &sceneSettings, &out.scene) != IPL_STATUS_SUCCESS) {
            ResonanceLog::error("ResonanceServer: iplSceneCreate failed (_prepare_bake_scene asset path).");
            return false;
        }
        int bake_tri_count = 0;
        std::vector<int> bake_debug_ids;
        std::atomic<int> bake_gtc{0};
        std::atomic<bool> bake_dirty{false};
        std::vector<IPLMatrix4x4> bake_instanced_transforms;
        RuntimeSceneState state(out.meshes, bake_tri_count, bake_debug_ids, &bake_gtc, &bake_dirty, out.sub_scenes,
                                out.instanced, bake_instanced_transforms, &out.meshes_in_sub);
        const size_t n = _bake_static_scene_assets.size();
        for (size_t i = 0; i < n; i++) {
            const Ref<ResonanceGeometryAsset>& asset = _bake_static_scene_assets[i];
            if (!asset.is_valid() || !asset->is_valid())
                continue;
            const Transform3D& xf =
                (i < _bake_static_scene_transforms.size()) ? _bake_static_scene_transforms[i] : Transform3D();
            scene_manager_.add_static_scene_from_asset(_ctx(), out.scene, asset, nullptr, false, state, xf,
                                                       _tracer_type_for_mesh_operations(), _embree(), _radeon());
        }
        if (out.meshes.empty() && out.instanced.empty()) {
            ResonanceLog::error("ResonanceServer: bake static assets produced no meshes.");
            _release_bake_scene_scratch(out);
            return false;
        }
        iplSceneCommit(out.scene);
        return true;
    }

    const bool has_runtime_static = !_runtime_static_packs.empty();
    if (has_runtime_static) {
        if (iplSceneCreate(_ctx(), &sceneSettings, &out.scene) != IPL_STATUS_SUCCESS) {
            ResonanceLog::error("ResonanceServer: iplSceneCreate failed (_prepare_bake_scene runtime static clone).");
            return false;
        }
        for (const auto& kv : _runtime_static_packs) {
            const RuntimeStaticPack& pack = kv.second;
            if (!pack.sub_scene || !pack.instanced)
                continue;
            if (!clone_sub_scene_as_instanced(_ctx(), out.scene, pack.sub_scene, pack.transform, _tracer_type_for_mesh_operations(),
                                              _embree(), _radeon(), out.sub_scenes, out.instanced)) {
                ResonanceLog::error("ResonanceServer: failed cloning runtime static instanced mesh for bake.");
                _release_bake_scene_scratch(out);
                return false;
            }
        }
        if (out.instanced.empty()) {
            ResonanceLog::error("ResonanceServer: runtime static packs produced no bake meshes.");
            _release_bake_scene_scratch(out);
            return false;
        }
        iplSceneCommit(out.scene);
        return true;
    }

    // Last resort: snapshot the live Phonon scene into an isolated temp (may include dynamics frozen at t0).
    if (!scene) {
        ResonanceLog::error("ResonanceServer: no live scene for bake snapshot.");
        return false;
    }
    IPLSerializedObjectSettings serialSettings{};
    IPLSerializedObject serialObj = nullptr;
    if (iplSerializedObjectCreate(_ctx(), &serialSettings, &serialObj) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceServer: iplSerializedObjectCreate failed (live bake snapshot).");
        return false;
    }
    iplSceneSave(scene, serialObj);
    const IPLsize snap_size = iplSerializedObjectGetSize(serialObj);
    if (snap_size == 0) {
        iplSerializedObjectRelease(&serialObj);
        ResonanceLog::error("ResonanceServer: live bake snapshot produced no data.");
        return false;
    }
    const IPLerror load_err = iplSceneLoad(_ctx(), &sceneSettings, serialObj, nullptr, nullptr, &out.scene);
    iplSerializedObjectRelease(&serialObj);
    if (load_err != IPL_STATUS_SUCCESS || !out.scene) {
        ResonanceLog::error("ResonanceServer: iplSceneLoad failed (live bake snapshot).");
        out.scene = nullptr;
        return false;
    }
    UtilityFunctions::push_warning(
        "Nexus Resonance Bake: no static scene asset; using a live-scene snapshot. "
        "Prefer ResonanceStaticScene / set_bake_static_scenes_from_assets so dynamics are excluded.");
    return true;
}

bool ResonanceServer::_with_bake_scene(std::function<bool(IPLScene)> bake_fn) {
    BakeSceneScratch scratch;
    {
        std::lock_guard<std::mutex> lock(simulation_mutex);
        if (scene_dirty.load(std::memory_order_acquire)) {
            iplSceneCommit(scene);
            scene_dirty.store(false, std::memory_order_release);
        }
        if (!_prepare_bake_scene(scratch))
            return false;
    }
    if (!scratch.scene)
        return false;
    bake_progress_.store(0.0f, std::memory_order_release);
    const bool ok = bake_fn(scratch.scene);
    _release_bake_scene_scratch(scratch);
    return ok;
}

bool ResonanceServer::bake_manual_grid(const PackedVector3Array& points, Ref<ResonanceProbeData> data) {
    if (!_ctx() || !scene) {
        UtilityFunctions::push_error("Nexus Resonance Bake: Server not initialized.");
        return false;
    }
    if (!_has_bake_static_scene_assets()) {
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
                                             int generation_type, float height_above_floor, Ref<ResonanceProbeData> probe_data_res,
                                             const Array& exclusion_boxes) {
    if (!_ctx() || !scene) {
        UtilityFunctions::push_error("Nexus Resonance Bake: Server not initialized.");
        return false;
    }
    if (!_has_bake_static_scene_assets()) {
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
    return _with_bake_scene([this, volume_transform, extents, spacing, generation_type, height_above_floor, probe_data_res,
                             exclusion_boxes, nb, nr, bake_reflection, nt, ao](IPLScene bake_scene) {
        PackedVector3Array points;
        if (generation_type == ResonanceBaker::GEN_CENTROID || generation_type == ResonanceBaker::GEN_UNIFORM_FLOOR) {
            IPLProbeArray probeArray = nullptr;
            if (iplProbeArrayCreate(_ctx(), &probeArray) == IPL_STATUS_SUCCESS) {
                IPLProbeGenerationParams genParams{};
                genParams.type = (generation_type == ResonanceBaker::GEN_CENTROID)
                                     ? IPL_PROBEGENERATIONTYPE_CENTROID
                                     : IPL_PROBEGENERATIONTYPE_UNIFORMFLOOR;
                genParams.spacing = spacing;
                genParams.height = height_above_floor;
                genParams.transform = ResonanceUtils::create_volume_transform_rotated(volume_transform, extents);
                iplProbeArrayGenerateProbes(probeArray, bake_scene, &genParams);
                const int num_probes = iplProbeArrayGetNumProbes(probeArray);
                for (int i = 0; i < num_probes; i++) {
                    IPLSphere sphere = iplProbeArrayGetProbe(probeArray, i);
                    points.push_back(ResonanceUtils::to_godot_vector3(sphere.center));
                }
                iplProbeArrayRelease(&probeArray);
            }
            if (points.is_empty()) {
                points = baker.generate_manual_grid(volume_transform, extents, spacing, generation_type, height_above_floor);
            }
        } else {
            points = baker.generate_manual_grid(volume_transform, extents, spacing, generation_type, height_above_floor);
        }

        points = resonance::filter_points_outside_exclusion_boxes(points, exclusion_boxes);
        if (points.is_empty()) {
            UtilityFunctions::push_error(
                "Nexus Resonance Bake: No probes left after exclusion volumes. Enlarge the volume or disable exclusions.");
            return false;
        }
        return baker.bake_manual_grid(_ctx(), bake_scene, _tracer_type_for_mesh_operations(), _opencl(), _radeon(), points, nb,
                                      nr, bake_reflection, probe_data_res, bake_progress_callback, this, _bake_pipeline_pathing, nt,
                                      ao);
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
    resonance::bump_slot_epoch(reverb_param_cache_epoch_[reverb_back]);
    resonance::bump_slot_epoch(reflection_param_cache_epoch_[refl_back]);
    resonance::bump_slot_epoch(pathing_param_cache_epoch_[path_back]);
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
    {
        // Clear source pathing before PathSimulator erase: Phonon keeps ProbeBatch alive via
        // shared_ptr, but RunPathing null-dereferences when the batch is gone from the simulator map.
        std::lock_guard<std::mutex> lock(simulation_mutex);
        _clear_pathing_for_probe_batch_assume_locked(handle);
        probe_batch_registry_.remove_batch(handle, simulator, nullptr);
    }
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
    {
        std::lock_guard<std::mutex> lock(simulation_mutex);
        std::vector<int32_t> handles;
        source_manager.get_all_handles(handles);
        for (int32_t h : handles) {
            IPLSource src = source_manager.get_source(h);
            if (!src)
                continue;
            _clear_source_pathing_inputs_assume_locked(src, h);
            iplSourceRelease(&src);
        }
        probe_batch_registry_.clear_batches(simulator, nullptr);
    }
    _clear_all_param_caches();
}
void ResonanceServer::emit_bake_progress(float progress) {
    // Bake thread safe: no Godot signal emit from worker threads.
    bake_progress_.store(progress, std::memory_order_release);
}

float ResonanceServer::get_bake_progress() const {
    return bake_progress_.load(std::memory_order_acquire);
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
