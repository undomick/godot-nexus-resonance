#ifndef RESONANCE_SPATIAL_OUTPUT_GATE_H
#define RESONANCE_SPATIAL_OUTPUT_GATE_H

#include <array>
#include <atomic>
#include <cstdint>

namespace resonance {

enum class SceneTracing {
    kPhonon,
    kGodotPhysics,
};

inline bool spatial_scene_ready(SceneTracing tracing, bool physics_world_bound, int triangle_count, bool scene_committed) {
    if (tracing == SceneTracing::kGodotPhysics)
        return physics_world_bound && scene_committed;
    return triangle_count <= 0 || scene_committed;
}

inline bool spatial_output_gate_allows(bool scene_ready, bool has_simulated_source, bool direct_ready) {
    if (!scene_ready)
        return false;
    return !has_simulated_source || direct_ready;
}

template <int kHandles>
class SourceDirectReadiness {
  public:
    uint32_t current_generation() const { return generation_.load(std::memory_order_acquire); }

    void arm() {
        if (generation_.fetch_add(1, std::memory_order_acq_rel) + 1 == kNeverPublished)
            generation_.fetch_add(1, std::memory_order_acq_rel);
    }

    void mark_published(int handle, uint32_t generation) {
        ready_generation_[static_cast<size_t>(handle)].store(generation, std::memory_order_release);
    }

    void reset_source(int handle) {
        ready_generation_[static_cast<size_t>(handle)].store(kNeverPublished, std::memory_order_release);
    }

    bool is_direct_ready(int handle) const {
        return ready_generation_[static_cast<size_t>(handle)].load(std::memory_order_acquire) == current_generation();
    }

  private:
    static constexpr uint32_t kNeverPublished = 0;

    std::atomic<uint32_t> generation_{kNeverPublished + 1};
    std::array<std::atomic<uint32_t>, kHandles> ready_generation_{};
};

} // namespace resonance

#endif // RESONANCE_SPATIAL_OUTPUT_GATE_H
