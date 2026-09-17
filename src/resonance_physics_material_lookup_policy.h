#ifndef RESONANCE_PHYSICS_MATERIAL_LOOKUP_POLICY_H
#define RESONANCE_PHYSICS_MATERIAL_LOOKUP_POLICY_H

namespace resonance {

/// How Custom-scene collider materials resolve to an IPLMaterial.
enum class PhysicsMaterialLookupResult {
    DirectResource, // collider meta resonance_physics_material (ResonanceMaterial)
    NameMap,        // collider meta resonance_physics_material_preset found in loaded .tres map
    Fallback,       // generic hardcoded IPLMaterial
};

inline PhysicsMaterialLookupResult physics_material_lookup_path(bool has_direct_resource, bool has_name_meta,
                                                                bool name_in_map) {
    if (has_direct_resource)
        return PhysicsMaterialLookupResult::DirectResource;
    if (has_name_meta && name_in_map)
        return PhysicsMaterialLookupResult::NameMap;
    return PhysicsMaterialLookupResult::Fallback;
}

} // namespace resonance

#endif // RESONANCE_PHYSICS_MATERIAL_LOOKUP_POLICY_H
