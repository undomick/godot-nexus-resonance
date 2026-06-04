#include "resonance_constants.h"
#include "resonance_geometry.h"
#include "resonance_geometry_asset.h"
#include "resonance_scene_manager.h"
#include "resonance_server.h"
#include "resonance_utils.h"
#include <algorithm>
#include <chrono>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/main_loop.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <unordered_set>

using namespace godot;

// Scene graph I/O: triangle/transform notifications, OBJ/serialized loads, dynamic instanced mesh queues, static-scene export hashes.

// scene_dirty gates iplSceneCommit on the worker (set by geometry edits, coalesced transforms, or explicit mark_scene_commit_pending).

namespace {

void refresh_geometry_recursive(Node* node) {
    if (!node)
        return;
    if (ResonanceGeometry* geom = Object::cast_to<ResonanceGeometry>(node))
        geom->refresh_geometry();
    for (int i = 0; i < node->get_child_count(); ++i)
        refresh_geometry_recursive(node->get_child(i));
}

} // namespace

bool ResonanceServer::consume_geometry_transform_coalesce_tick() {
    constexpr int interval = resonance::kGeometryTransformCoalesceInterval;
    if (interval <= 1)
        return true;
    const uint32_t c = geometry_transform_coalesce_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
    return (c % static_cast<uint32_t>(interval)) == 0;
}

void ResonanceServer::notify_geometry_changed_assume_locked(int triangle_delta) {
    if (!_ctx())
        return;
    if (triangle_delta != 0)
        global_triangle_count.fetch_add(triangle_delta, std::memory_order_release);
    if (triangle_delta != 0) {
        phonon_scene_audio_ready_.store(false, std::memory_order_release);
        reset_spatial_audio_warmup_passes();
        scene_dirty.store(true, std::memory_order_release);
    } else if (consume_geometry_transform_coalesce_tick()) {
        scene_dirty.store(true, std::memory_order_release);
    }
}

void ResonanceServer::notify_geometry_changed(int triangle_delta) {
    if (!_ctx())
        return;
    notify_geometry_changed_assume_locked(triangle_delta);
    if (!_uses_main_thread_phonon_simulation() && thread_running) {
        std::lock_guard<std::mutex> lock(worker_mutex);
        simulation_requested = true;
        worker_cv.notify_one();
    }
}

void ResonanceServer::mark_scene_commit_pending_assume_locked() {
    if (!_ctx())
        return;
    scene_dirty.store(true, std::memory_order_release);
}

void ResonanceServer::mark_scene_commit_pending() {
    if (!_ctx())
        return;
    scene_dirty.store(true, std::memory_order_release);
}

void ResonanceServer::save_scene_data(String filename) {
    if (_scene_type() == IPL_SCENETYPE_CUSTOM) {
        UtilityFunctions::push_warning(
            "Nexus Resonance: save_scene_data is not supported when scene_type is Custom (no Phonon mesh data).");
        return;
    }
    std::lock_guard<std::mutex> lock(simulation_mutex);
    scene_manager_.save_scene_data(_ctx(), scene, filename);
}

void ResonanceServer::save_scene_obj(String file_base_name) {
    std::lock_guard<std::mutex> lock(simulation_mutex);
    if (!scene) {
        UtilityFunctions::push_warning("Nexus Resonance: No scene to export (save_scene_obj).");
        return;
    }
    String abs_path = file_base_name;
    if (file_base_name.begins_with("res://") || file_base_name.begins_with("user://")) {
        ProjectSettings* ps = ProjectSettings::get_singleton();
        if (ps)
            abs_path = ps->globalize_path(file_base_name);
    }
    // Phonon OBJ export uses the path verbatim (append .obj if missing).
    if (!abs_path.ends_with(".obj")) {
        abs_path = abs_path + ".obj";
    }
    Error write_err = ResonanceSceneManager::save_phonon_scene_obj_atomic(scene, abs_path);
    if (write_err != OK)
        UtilityFunctions::push_warning("Nexus Resonance: save_scene_obj failed (error " + String::num_int64(write_err) + ").");
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint()) {
        UtilityFunctions::print_rich("[color=cyan]Nexus Resonance:[/color] Scene exported to OBJ (base: " + file_base_name + ").");
    }
}

void ResonanceServer::load_scene_data(String filename) {
    if (!_ctx() || !simulator)
        return;
    if (_scene_type() == IPL_SCENETYPE_CUSTOM) {
        UtilityFunctions::push_warning(
            "Nexus Resonance: load_scene_data is not supported when scene_type is Custom (Godot Physics).");
        return;
    }
    bool loaded = false;
    {
        std::lock_guard<std::mutex> lock(simulation_mutex);
        loaded = scene_manager_.load_scene_data(_ctx(), &scene, simulator, _tracer_type_for_mesh_operations(), _embree(), _radeon(), filename,
                                                &global_triangle_count);
    }
    if (loaded)
        call_deferred("_deferred_refresh_all_geometry_after_scene_load");
}

void ResonanceServer::refresh_all_geometry_from_scene_tree() {
    Engine* eng = Engine::get_singleton();
    if (!eng)
        return;
    MainLoop* ml = eng->get_main_loop();
    SceneTree* tree = Object::cast_to<SceneTree>(ml);
    if (!tree)
        return;
    Window* win = tree->get_root();
    if (!win)
        return;
    // SceneTree root is Window (extends Viewport -> Node); use Node entry for traversal.
    Node* root = static_cast<Node*>(static_cast<Viewport*>(win));
    refresh_geometry_recursive(root);
}

void ResonanceServer::_deferred_refresh_all_geometry_after_scene_load() {
    refresh_all_geometry_from_scene_tree();
    reset_spatial_audio_warmup_passes();
}

Error ResonanceServer::export_static_scene_to_asset(Node* scene_root, const String& p_path) {
    return scene_manager_.export_static_scene_to_asset(scene_root, p_path);
}

Error ResonanceServer::export_static_scene_to_obj(Node* scene_root, const String& file_base_name) {
    return scene_manager_.export_static_scene_to_obj(scene_root, file_base_name);
}

int64_t ResonanceServer::get_static_scene_hash(Node* scene_root) {
    return scene_manager_.get_static_scene_hash(scene_root, [this](const PackedByteArray& pba) { return _hash_probe_data(pba); });
}

int64_t ResonanceServer::get_geometry_asset_hash(const Ref<ResonanceGeometryAsset>& p_asset) const {
    if (!p_asset.is_valid() || !p_asset->is_valid())
        return 0;
    PackedByteArray data = p_asset->get_mesh_data();
    return static_cast<int64_t>(_hash_probe_data(data));
}
uint64_t ResonanceServer::_hash_probe_data(const PackedByteArray& pba) {
    return _hash_probe_data(pba.ptr(), static_cast<size_t>(pba.size()));
}

uint64_t ResonanceServer::_hash_probe_data(const uint8_t* ptr, size_t size) {
    return resonance::fnv1a_hash(ptr, size);
}

void ResonanceServer::enqueue_dynamic_instanced_mesh_transform(IPLInstancedMesh mesh, const IPLMatrix4x4& transform) {
    if (!mesh || !_ctx())
        return;
    const auto t0 = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> qlock(dynamic_instanced_transform_queue_mutex_);
        dynamic_instanced_transform_queue_[mesh] = transform;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const uint64_t us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    instrumentation_main_us_last_dynamic_transform_enqueue_.store(us, std::memory_order_relaxed);
    instrumentation_main_us_dynamic_transform_enqueue_.fetch_add(us, std::memory_order_relaxed);
    instrumentation_dynamic_transform_enqueue_events_.fetch_add(1, std::memory_order_relaxed);
}

void ResonanceServer::cancel_pending_dynamic_instanced_mesh_transform(IPLInstancedMesh mesh) {
    if (!mesh)
        return;
    std::lock_guard<std::mutex> qlock(dynamic_instanced_transform_queue_mutex_);
    dynamic_instanced_transform_queue_.erase(mesh);
}

// Applies queued instanced-mesh transforms; may defer under dynamic_scene_commit_min_interval_ (re-queue transforms if not due).
bool ResonanceServer::_apply_queued_dynamic_instanced_mesh_transforms_assume_locked() {
    if (!_ctx() || !scene)
        return false;

    std::unordered_map<IPLInstancedMesh, IPLMatrix4x4> local;
    {
        std::lock_guard<std::mutex> qlock(dynamic_instanced_transform_queue_mutex_);
        local.swap(dynamic_instanced_transform_queue_);
    }

    if (local.empty())
        return false;

    const bool static_wants_commit = scene_dirty.load(std::memory_order_acquire);
    const auto now = std::chrono::steady_clock::now();
    bool apply_now = static_wants_commit || dynamic_scene_commit_min_interval_ <= 0.0f;
    if (!apply_now) {
        const float dt_sec = std::chrono::duration<float>(now - last_dynamic_scene_commit_time_).count();
        apply_now = (dt_sec >= dynamic_scene_commit_min_interval_);
    }

    if (!apply_now) {
        std::lock_guard<std::mutex> qlock(dynamic_instanced_transform_queue_mutex_);
        for (auto& kv : local)
            dynamic_instanced_transform_queue_[kv.first] = kv.second;
        return false;
    }

    for (auto& kv : local) {
        if (kv.first)
            iplInstancedMeshUpdateTransform(kv.first, scene, kv.second);
    }
    last_dynamic_scene_commit_time_ = now;
    mark_scene_commit_pending_assume_locked();
    return true;
}

void ResonanceServer::set_physics_world(const Ref<World3D>& world) {
    godot_physics_bridge_.set_world(world);
}

void ResonanceServer::_rebuild_and_apply_physics_ray_excludes_unlocked() {
    std::unordered_set<int64_t> seen;
    TypedArray<RID> merged;
    auto append = [&seen, &merged](RID r) {
        if (!r.is_valid())
            return;
        const int64_t id = r.get_id();
        if (!seen.insert(id).second)
            return;
        merged.append(r);
    };
    const int nu = physics_ray_exclude_rids_user_.size();
    for (int i = 0; i < nu; ++i)
        append(physics_ray_exclude_rids_user_[i]);
    const int nl = listener_physics_ray_exclude_rids_.size();
    for (int i = 0; i < nl; ++i)
        append(listener_physics_ray_exclude_rids_[i]);
    for (const RID& r : physics_ray_auto_exclude_active_)
        append(r);
    godot_physics_bridge_.set_exclude_rids(merged);
}

void ResonanceServer::_clear_physics_ray_excludes_state() {
    std::lock_guard<std::mutex> lock(physics_ray_excludes_mutex_);
    physics_ray_exclude_rids_user_.clear();
    listener_physics_ray_exclude_rids_.clear();
    physics_ray_auto_exclude_refcount_.clear();
    physics_ray_auto_exclude_active_.clear();
    godot_physics_bridge_.set_exclude_rids(TypedArray<RID>());
}

void ResonanceServer::set_physics_ray_exclude_rids(const TypedArray<RID>& exclude) {
    std::lock_guard<std::mutex> lock(physics_ray_excludes_mutex_);
    physics_ray_exclude_rids_user_ = exclude;
    _rebuild_and_apply_physics_ray_excludes_unlocked();
}

void ResonanceServer::set_listener_physics_ray_exclude_rids(const TypedArray<RID>& rids) {
    std::lock_guard<std::mutex> lock(physics_ray_excludes_mutex_);
    listener_physics_ray_exclude_rids_ = rids;
    _rebuild_and_apply_physics_ray_excludes_unlocked();
}

void ResonanceServer::register_physics_ray_auto_exclude_rid(RID rid) {
    if (!rid.is_valid())
        return;
    std::lock_guard<std::mutex> lock(physics_ray_excludes_mutex_);
    const int64_t id = rid.get_id();
    int& c = physics_ray_auto_exclude_refcount_[id];
    if (c == 0)
        physics_ray_auto_exclude_active_.push_back(rid);
    c++;
    _rebuild_and_apply_physics_ray_excludes_unlocked();
}

void ResonanceServer::unregister_physics_ray_auto_exclude_rid(RID rid) {
    if (!rid.is_valid())
        return;
    std::lock_guard<std::mutex> lock(physics_ray_excludes_mutex_);
    const int64_t id = rid.get_id();
    auto it = physics_ray_auto_exclude_refcount_.find(id);
    if (it == physics_ray_auto_exclude_refcount_.end() || it->second <= 0)
        return;
    it->second--;
    if (it->second == 0) {
        physics_ray_auto_exclude_refcount_.erase(it);
        auto vit = std::find_if(physics_ray_auto_exclude_active_.begin(), physics_ray_auto_exclude_active_.end(),
                                [id](const RID& r) { return r.get_id() == id; });
        if (vit != physics_ray_auto_exclude_active_.end())
            physics_ray_auto_exclude_active_.erase(vit);
    }
    _rebuild_and_apply_physics_ray_excludes_unlocked();
}
