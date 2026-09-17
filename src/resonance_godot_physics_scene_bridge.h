#ifndef RESONANCE_GODOT_PHYSICS_SCENE_BRIDGE_H
#define RESONANCE_GODOT_PHYSICS_SCENE_BRIDGE_H

#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <mutex>
#include <phonon.h>
#include <string>
#include <unordered_map>

namespace godot {

class Object;

/// Holds Godot world + trace settings for Steam Audio IPL_SCENETYPE_CUSTOM callbacks.
class ResonanceGodotPhysicsSceneBridge {
  public:
    void set_world(const Ref<World3D>& world);
    void clear_world();
    bool has_valid_world() const { return world_.is_valid(); }

    void set_collision_mask(uint32_t mask);
    uint32_t get_collision_mask() const { return collision_mask_; }

    void set_exclude_rids(const TypedArray<RID>& exclude);
    const TypedArray<RID>& get_exclude_rids() const { return exclude_rids_; }

    /// Directories scanned for ResonanceMaterial .tres/.res (basename -> IPLMaterial). Empty = ProjectSettings / default.
    void set_material_search_paths(const Vector<String>& paths);
    Vector<String> get_material_search_paths() const { return material_search_paths_; }

    /// Main-thread: load ResonanceMaterial resources from search paths into an IPLMaterial name map.
    void refresh_material_map();
    bool has_material_map() const;

    void* user_data() { return static_cast<void*>(this); }

    /// Bidirectional unique-geometry transmission walk (one CollisionObject RID each).
    /// Steam product+sqrt when more than one unique hit. Does not use Phonon closestHit (safe for reflections).
    /// Returns true when at least one unique surface was found.
    bool compute_unique_transmission(const IPLVector3& listener_origin, const IPLVector3& source_origin,
                                     int max_surfaces, float* out_lmh, float* out_hit0_lmh,
                                     bool* out_hit0_valid) const;

    static void IPLCALL closest_hit_callback(const IPLRay* ray, IPLfloat32 min_distance, IPLfloat32 max_distance, IPLHit* hit,
                                             void* user_data);
    static void IPLCALL any_hit_callback(const IPLRay* ray, IPLfloat32 min_distance, IPLfloat32 max_distance, IPLuint8* occluded,
                                         void* user_data);
    static void IPLCALL batched_closest_hit_callback(IPLint32 num_rays, const IPLRay* rays, const IPLfloat32* min_distances,
                                                     const IPLfloat32* max_distances, IPLHit* hits, void* user_data);
    static void IPLCALL batched_any_hit_callback(IPLint32 num_rays, const IPLRay* rays, const IPLfloat32* min_distances,
                                                 const IPLfloat32* max_distances, IPLuint8* occluded, void* user_data);

  private:
    void trace_closest(const IPLRay& ray, float min_distance, float max_distance, IPLHit* out_hit);
    bool trace_any(const IPLRay& ray, float min_distance, float max_distance);

    /// Closest hit excluding [param extra_exclude]. On hit, fills [param out_rid] when non-null.
    int64_t trace_closest_excluding(const IPLRay& ray, float min_distance, float max_distance,
                                    const TypedArray<RID>& extra_exclude, IPLHit* out_hit,
                                    IPLMaterial* material_storage, RID* out_rid) const;

    IPLMaterial material_for_collider(Object* collider) const;
    bool try_lookup_ipl_by_name(const String& name, IPLMaterial& out) const;

    Ref<World3D> world_;
    uint32_t collision_mask_ = 0xFFFFFFFFu;
    TypedArray<RID> exclude_rids_;

    Vector<String> material_search_paths_;
    mutable std::mutex material_map_mutex_;
    /// Basename (lower) -> IPLMaterial snapshot from .tres at refresh time (worker-safe copies).
    std::unordered_map<std::string, IPLMaterial> material_ipl_by_name_;
};

} // namespace godot

#endif
