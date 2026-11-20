#ifndef RESONANCE_IPL_RESOURCE_POOL_BACKEND_H
#define RESONANCE_IPL_RESOURCE_POOL_BACKEND_H

#include "resonance_reflection_effect_recipe.h"
#include "resonance_resource_pool.h"
#include <phonon.h>

namespace godot {

/// Builds player sources and shared reflection effects for the player resource pool.
struct IplResourcePoolBackend {
    using Source = IPLSource;
    using Effect = IPLReflectionEffect;

    IPLContext context = nullptr;
    IPLSimulator simulator = nullptr;
    IPLSourceSettings source{};
    ReflectionEffectRecipe reflection;

    IPLSource create_source() const;
    IPLReflectionEffect create_effect() const;
    void reset(IPLReflectionEffect effect) const;
    void release(IPLSource source) const;
    void release(IPLReflectionEffect effect) const;
    void on_pool_thread_start() const;
};

using ResonancePlayerResourcePool = resonance::ResourcePool<IplResourcePoolBackend>;

} // namespace godot

#endif // RESONANCE_IPL_RESOURCE_POOL_BACKEND_H
