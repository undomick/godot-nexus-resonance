#include "resonance_godot_physics_scene_bridge.h"
#include "resonance_constants.h"
#include "resonance_material.h"
#include "resonance_physics_material_lookup_policy.h"
#include "resonance_physics_ray_math.h"

#include <cmath>
#include <cstdint>
#include <limits>

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/physics_direct_space_state3d.hpp>
#include <godot_cpp/classes/physics_ray_query_parameters3d.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot {

namespace {

const StringName& meta_material_resource() {
    static const StringName n("resonance_physics_material");
    return n;
}

const StringName& meta_material_preset() {
    static const StringName n("resonance_physics_material_preset");
    return n;
}

IPLMaterial ipl_material_generic() {
    IPLMaterial m{};
    m.absorption[0] = 0.10f;
    m.absorption[1] = 0.20f;
    m.absorption[2] = 0.30f;
    m.scattering = 0.05f;
    m.transmission[0] = 0.100f;
    m.transmission[1] = 0.050f;
    m.transmission[2] = 0.030f;
    return m;
}

void fill_miss(IPLHit* hit) {
    hit->distance = std::numeric_limits<float>::infinity();
    hit->triangleIndex = -1;
    hit->objectIndex = -1;
    hit->materialIndex = -1;
    hit->normal.x = hit->normal.y = hit->normal.z = 0.0f;
    hit->material = nullptr;
}

std::string material_name_key(const String& name) {
    const CharString utf8 = name.to_lower().utf8();
    return std::string(utf8.get_data());
}

} // namespace

void ResonanceGodotPhysicsSceneBridge::set_world(const Ref<World3D>& world) {
    world_ = world;
}

void ResonanceGodotPhysicsSceneBridge::clear_world() {
    world_.unref();
}

void ResonanceGodotPhysicsSceneBridge::set_collision_mask(uint32_t mask) {
    collision_mask_ = mask;
}

void ResonanceGodotPhysicsSceneBridge::set_exclude_rids(const TypedArray<RID>& exclude) {
    exclude_rids_ = exclude;
}

void ResonanceGodotPhysicsSceneBridge::set_material_search_paths(const Vector<String>& paths) {
    material_search_paths_ = paths;
}

bool ResonanceGodotPhysicsSceneBridge::has_material_map() const {
    std::lock_guard<std::mutex> lock(material_map_mutex_);
    return !material_ipl_by_name_.empty();
}

void ResonanceGodotPhysicsSceneBridge::refresh_material_map() {
    Vector<String> paths = material_search_paths_;
    if (paths.is_empty()) {
        ProjectSettings* ps = ProjectSettings::get_singleton();
        const String key =
            String(resonance::kProjectSettingsResonancePrefix) + resonance::kProjectSettingsPhysicsMaterialSearchPaths;
        if (ps && ps->has_setting(key)) {
            const Variant v = ps->get_setting(key);
            if (v.get_type() == Variant::PACKED_STRING_ARRAY) {
                const PackedStringArray arr = v;
                for (int i = 0; i < arr.size(); ++i) {
                    const String p = arr[i];
                    if (!p.is_empty())
                        paths.push_back(p);
                }
            }
        }
        if (paths.is_empty())
            paths.push_back(String(resonance::kDefaultPhysicsMaterialSearchPath));
    }

    std::unordered_map<std::string, IPLMaterial> next;
    ResourceLoader* loader = ResourceLoader::get_singleton();
    for (int i = 0; i < paths.size(); ++i) {
        String dir = paths[i];
        if (dir.is_empty())
            continue;
        if (!dir.ends_with("/"))
            dir += "/";

        const PackedStringArray files = DirAccess::get_files_at(dir);
        for (int f = 0; f < files.size(); ++f) {
            const String file = files[f];
            const String ext = file.get_extension().to_lower();
            if (ext != "tres" && ext != "res")
                continue;
            if (!loader)
                continue;
            const Ref<Resource> res = loader->load(dir + file);
            const Ref<ResonanceMaterial> mat = res;
            if (mat.is_null())
                continue;
            next[material_name_key(file.get_basename())] = mat->get_ipl_material();
        }
    }

    {
        std::lock_guard<std::mutex> lock(material_map_mutex_);
        material_ipl_by_name_.swap(next);
    }
}

bool ResonanceGodotPhysicsSceneBridge::try_lookup_ipl_by_name(const String& name, IPLMaterial& out) const {
    const std::string key = material_name_key(name);
    std::lock_guard<std::mutex> lock(material_map_mutex_);
    const auto it = material_ipl_by_name_.find(key);
    if (it == material_ipl_by_name_.end())
        return false;
    out = it->second;
    return true;
}

IPLMaterial ResonanceGodotPhysicsSceneBridge::material_for_collider(Object* collider) const {
    if (!collider)
        return ipl_material_generic();

    Ref<ResonanceMaterial> direct_mat;
    const bool has_direct_meta = collider->has_meta(meta_material_resource());
    if (has_direct_meta) {
        const Variant v = collider->get_meta(meta_material_resource());
        if (v.get_type() == Variant::OBJECT) {
            if (ResonanceMaterial* rm = Object::cast_to<ResonanceMaterial>(v.operator Object*()))
                direct_mat = Ref<ResonanceMaterial>(rm);
        }
    }
    const bool has_direct_resource = direct_mat.is_valid();

    String preset_name;
    bool has_name_meta = false;
    if (collider->has_meta(meta_material_preset())) {
        const Variant v = collider->get_meta(meta_material_preset());
        if (v.get_type() == Variant::STRING) {
            preset_name = v;
            has_name_meta = !preset_name.is_empty();
        }
    }

    IPLMaterial named{};
    const bool name_in_map = has_name_meta && try_lookup_ipl_by_name(preset_name, named);

    switch (resonance::physics_material_lookup_path(has_direct_resource, has_name_meta, name_in_map)) {
    case resonance::PhysicsMaterialLookupResult::DirectResource:
        return direct_mat->get_ipl_material();
    case resonance::PhysicsMaterialLookupResult::NameMap:
        return named;
    case resonance::PhysicsMaterialLookupResult::Fallback:
    default:
        return ipl_material_generic();
    }
}

void IPLCALL ResonanceGodotPhysicsSceneBridge::closest_hit_callback(const IPLRay* ray, IPLfloat32 min_distance, IPLfloat32 max_distance,
                                                                    IPLHit* hit, void* user_data) {
    auto* self = static_cast<ResonanceGodotPhysicsSceneBridge*>(user_data);
    if (!self || !ray || !hit) {
        if (hit)
            fill_miss(hit);
        return;
    }
    self->trace_closest(*ray, min_distance, max_distance, hit);
}

void IPLCALL ResonanceGodotPhysicsSceneBridge::any_hit_callback(const IPLRay* ray, IPLfloat32 min_distance, IPLfloat32 max_distance,
                                                                IPLuint8* occluded, void* user_data) {
    auto* self = static_cast<ResonanceGodotPhysicsSceneBridge*>(user_data);
    if (!occluded)
        return;
    if (!self || !ray) {
        *occluded = 1;
        return;
    }
    *occluded = self->trace_any(*ray, min_distance, max_distance) ? 1 : 0;
}

void IPLCALL ResonanceGodotPhysicsSceneBridge::batched_closest_hit_callback(IPLint32 num_rays, const IPLRay* rays,
                                                                            const IPLfloat32* min_distances, const IPLfloat32* max_distances,
                                                                            IPLHit* hits, void* user_data) {
    auto* self = static_cast<ResonanceGodotPhysicsSceneBridge*>(user_data);
    if (!hits || num_rays <= 0)
        return;
    if (!self || !rays || !min_distances || !max_distances) {
        for (IPLint32 i = 0; i < num_rays; ++i)
            fill_miss(&hits[i]);
        return;
    }
    for (IPLint32 i = 0; i < num_rays; ++i)
        self->trace_closest(rays[i], min_distances[i], max_distances[i], &hits[i]);
}

void IPLCALL ResonanceGodotPhysicsSceneBridge::batched_any_hit_callback(IPLint32 num_rays, const IPLRay* rays,
                                                                        const IPLfloat32* min_distances, const IPLfloat32* max_distances,
                                                                        IPLuint8* occluded, void* user_data) {
    auto* self = static_cast<ResonanceGodotPhysicsSceneBridge*>(user_data);
    if (!occluded || num_rays <= 0)
        return;
    if (!self || !rays || !min_distances || !max_distances) {
        for (IPLint32 i = 0; i < num_rays; ++i)
            occluded[i] = 1;
        return;
    }
    for (IPLint32 i = 0; i < num_rays; ++i) {
        if (max_distances[i] < min_distances[i]) {
            occluded[i] = 1;
            continue;
        }
        occluded[i] = self->trace_any(rays[i], min_distances[i], max_distances[i]) ? 1 : 0;
    }
}

void ResonanceGodotPhysicsSceneBridge::trace_closest(const IPLRay& ray, float min_distance, float max_distance, IPLHit* out_hit) {
    fill_miss(out_hit);
    if (max_distance <= min_distance)
        return;
    if (!world_.is_valid())
        return;

    PhysicsDirectSpaceState3D* space = world_->get_direct_space_state();
    if (!space)
        return;

    Vector3 origin(ray.origin.x, ray.origin.y, ray.origin.z);
    Vector3 dir(ray.direction.x, ray.direction.y, ray.direction.z);
    if (dir.length_squared() < 1e-20f)
        return;
    dir.normalize();

    // Same segment and hit-from-inside policy as trace_any / Custom ClosestHit+AnyHit.
    const float t_from = resonance::custom_scene_occlusion_ray_start_t(min_distance, max_distance);
    const Vector3 from = origin + dir * t_from;
    const Vector3 to = origin + dir * max_distance;

    Ref<PhysicsRayQueryParameters3D> params = PhysicsRayQueryParameters3D::create(from, to, collision_mask_, exclude_rids_);
    if (params.is_null())
        return;
    params->set_hit_from_inside(false);

    const Dictionary d = space->intersect_ray(params);
    if (d.is_empty())
        return;

    Variant pos_v = d.get("position", Variant());
    if (pos_v.get_type() != Variant::VECTOR3)
        return;
    const Vector3 hit_pos = pos_v;

    const float dist = (hit_pos - origin).length();
    if (dist < min_distance || dist > max_distance)
        return;

    thread_local static IPLMaterial s_hit_material;

    out_hit->distance = dist;
    out_hit->triangleIndex = -1;
    out_hit->objectIndex = -1;
    out_hit->materialIndex = 0;

    Variant norm_v = d.get("normal", Variant());
    if (norm_v.get_type() == Variant::VECTOR3) {
        Vector3 n = norm_v;
        if (n.length_squared() > 1e-20f) {
            n.normalize();
            out_hit->normal.x = n.x;
            out_hit->normal.y = n.y;
            out_hit->normal.z = n.z;
        }
    }

    Variant coll_v = d.get("collider", Variant());
    Object* collider = nullptr;
    if (coll_v.get_type() == Variant::OBJECT)
        collider = coll_v.operator Object*();
    s_hit_material = material_for_collider(collider);
    out_hit->material = &s_hit_material;
}

bool ResonanceGodotPhysicsSceneBridge::trace_any(const IPLRay& ray, float min_distance, float max_distance) {
    if (max_distance <= min_distance)
        return false;
    if (!world_.is_valid())
        return false;
    PhysicsDirectSpaceState3D* space = world_->get_direct_space_state();
    if (!space)
        return false;

    Vector3 origin(ray.origin.x, ray.origin.y, ray.origin.z);
    Vector3 dir(ray.direction.x, ray.direction.y, ray.direction.z);
    if (dir.length_squared() < 1e-20f)
        return false;
    dir.normalize();

    const float t_from = resonance::custom_scene_occlusion_ray_start_t(min_distance, max_distance);
    const float t_to = max_distance;
    const Vector3 from = origin + dir * t_from;
    const Vector3 to = origin + dir * t_to;

    Ref<PhysicsRayQueryParameters3D> params = PhysicsRayQueryParameters3D::create(from, to, collision_mask_, exclude_rids_);
    if (params.is_null())
        return false;
    params->set_hit_from_inside(false);

    const Dictionary d = space->intersect_ray(params);
    if (d.is_empty())
        return false;

    Variant pos_v = d.get("position", Variant());
    if (pos_v.get_type() != Variant::VECTOR3)
        return false;
    const Vector3 hit_pos = pos_v;
    const float dist = (hit_pos - origin).length();
    return dist >= min_distance && dist <= max_distance;
}

} // namespace godot
