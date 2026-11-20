#ifndef RESONANCE_SOURCE_UPDATE_POLICY_H
#define RESONANCE_SOURCE_UPDATE_POLICY_H

#include "resonance_constants.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace resonance {

template <typename SourceUpdateParamsT>
inline bool source_update_params_equal(const SourceUpdateParamsT& a, const SourceUpdateParamsT& b) {
    return std::memcmp(&a, &b, sizeof(SourceUpdateParamsT)) == 0;
}

/// Marks source snapshots stale from any thread without touching the worker-owned snapshot map. The
/// worker consumes the mark before its skip-unchanged check, so the next update runs SetInputs.
template <std::size_t N>
class SourceSnapshotInvalidation {
  public:
    /// A handle outside [0, N) invalidates every snapshot.
    void invalidate(int32_t handle) {
        if (handle < 0 || static_cast<std::size_t>(handle) >= N) {
            invalidate_all();
            return;
        }
        handle_invalid_[static_cast<std::size_t>(handle)].store(true, std::memory_order_release);
    }

    void invalidate_all() { generation_.fetch_add(1, std::memory_order_acq_rel); }

    /// seen_generation belongs to the snapshot record and is updated here.
    bool consume(int32_t handle, uint32_t& seen_generation) {
        bool invalid = false;
        if (handle >= 0 && static_cast<std::size_t>(handle) < N)
            invalid = handle_invalid_[static_cast<std::size_t>(handle)].exchange(false, std::memory_order_acq_rel);
        const uint32_t generation = generation_.load(std::memory_order_acquire);
        if (generation != seen_generation) {
            seen_generation = generation;
            invalid = true;
        }
        return invalid;
    }

  private:
    std::array<std::atomic<bool>, N> handle_invalid_{};
    std::atomic<uint32_t> generation_{0};
};

/// Override: -1 or 1 = on, 0 = off. Mix levels only scale the DSP and never clear the IPL flag.
inline bool source_sim_reflections_enabled(int reflections_enabled_override, bool output_reverb) {
    if (!output_reverb)
        return false;
    if (reflections_enabled_override == 0)
        return false;
    return true;
}

/// Override: -1 = use pathing_enabled_global, 0 = off, 1 = on.
inline bool source_sim_pathing_enabled(int pathing_enabled_override, bool pathing_enabled_global, bool output_reverb) {
    if (!output_reverb)
        return false;
    if (pathing_enabled_override == -1)
        return pathing_enabled_global;
    return pathing_enabled_override != 0;
}

inline bool playback_enable_direct(bool output_direct) {
    return output_direct;
}

/// A mix level of 0 still runs EffectApply on silence so the overlap-save state drains.
inline bool playback_enable_reverb(bool output_reverb) {
    return output_reverb;
}

/// Overrides as in source_sim_reflections_enabled and source_sim_pathing_enabled.
inline bool playback_wants_wet_effects(int reflections_enabled_override, int pathing_enabled_override,
                                       bool pathing_enabled_global) {
    const bool want_refl = reflections_enabled_override != 0;
    const bool want_path =
        (pathing_enabled_override == -1) ? pathing_enabled_global : (pathing_enabled_override != 0);
    return want_refl || want_path;
}

/// Only hybrid reverb comes back to each voice as ambisonics. Convolution and TAN go through the listener
/// reverb bus, parametric reverb is mono.
inline bool voice_needs_reverb_decoder(bool wants_wet_effects, int reflection_type) {
    return wants_wet_effects && reflection_type == kReflectionHybrid;
}

} // namespace resonance

#endif // RESONANCE_SOURCE_UPDATE_POLICY_H
