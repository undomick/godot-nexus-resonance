#include "resonance_godot_physics_scene_bridge.h"
#include "resonance_physics_ray_math.h"

#include <cmath>
#include <cstdint>
#include <limits>

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/physics_direct_space_state3d.hpp>
#include <godot_cpp/classes/physics_ray_query_parameters3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot {

namespace {

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

IPLMaterial ipl_material_named(const String& name) {
    const String lower = name.to_lower();
    IPLMaterial m{};
    if (lower == "brick") {
        m.absorption[0] = 0.03f;
        m.absorption[1] = 0.04f;
        m.absorption[2] = 0.07f;
        m.scattering = 0.05f;
        m.transmission[0] = m.transmission[1] = m.transmission[2] = 0.015f;
    } else if (lower == "concrete") {
        m.absorption[0] = 0.05f;
        m.absorption[1] = 0.07f;
        m.absorption[2] = 0.08f;
        m.scattering = 0.05f;
        m.transmission[0] = 0.015f;
        m.transmission[1] = 0.002f;
        m.transmission[2] = 0.001f;
    } else if (lower == "wood") {
        m.absorption[0] = 0.11f;
        m.absorption[1] = 0.07f;
        m.absorption[2] = 0.06f;
        m.scattering = 0.05f;
        m.transmission[0] = 0.070f;
        m.transmission[1] = 0.014f;
        m.transmission[2] = 0.005f;
    } else if (lower == "metal") {
        m.absorption[0] = 0.20f;
        m.absorption[1] = 0.07f;
        m.absorption[2] = 0.06f;
        m.scattering = 0.05f;
        m.transmission[0] = 0.200f;
        m.transmission[1] = 0.025f;
        m.transmission[2] = 0.010f;
    } else if (lower == "glass") {
        m.absorption[0] = 0.06f;
        m.absorption[1] = 0.03f;
        m.absorption[2] = 0.02f;
        m.scattering = 0.05f;
        m.transmission[0] = 0.060f;
        m.transmission[1] = 0.044f;
        m.transmission[2] = 0.011f;
    } else if (lower == "carpet") {
        m.absorption[0] = 0.24f;
        m.absorption[1] = 0.69f;
        m.absorption[2] = 0.73f;
        m.scattering = 0.05f;
        m.transmission[0] = 0.020f;
        m.transmission[1] = 0.005f;
        m.transmission[2] = 0.003f;
    } else {
        m = ipl_material_generic();
    }
    return m;
}

IPLMaterial material_for_collider(Object* collider) {
    thread_local static IPLMaterial s_storage;
    if (!collider)
        return ipl_material_generic();
    if (collider->has_meta(meta_material_preset())) {
        Variant v = collider->get_meta(meta_material_preset());
        if (v.get_type() == Variant::STRING) {
            s_storage = ipl_material_named((String)v);
            return s_storage;
        }
    }
    return ipl_material_generic();
}

void fill_miss(IPLHit* hit) {
    hit->distance = std::numeric_limits<float>::infinity();
    hit->triangleIndex = -1;
    hit->objectIndex = -1;
    hit->materialIndex = -1;
    hit->normal.x = hit->normal.y = hit->normal.z = 0.0f;
    hit->material = nullptr;
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

    const Vector3 from = origin + dir * min_distance;
    const Vector3 to = origin + dir * max_distance;

    Ref<PhysicsRayQueryParameters3D> params = PhysicsRayQueryParameters3D::create(from, to, collision_mask_, exclude_rids_);
    if (params.is_null())
        return;
    // hit_from_inside must stay OFF for closest-hit tracing: Steam Audio's
    // transmission rays march through solid surfaces in small offsets and rely on
    // each segment reaching the NEXT surface. Reporting an inside-start as a hit
    // at distance ~0 makes the loop re-hit the same collider until the
    // accumulated transmission collapses to ~0 (walls become fully opaque).
    // See resonance::custom_scene_closest_hit_from_inside().
    params->set_hit_from_inside(resonance::custom_scene_closest_hit_from_inside());

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
    params->set_hit_from_inside(resonance::custom_scene_any_hit_from_inside());

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
