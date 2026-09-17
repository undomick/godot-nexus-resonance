#ifndef RESONANCE_PATHING_FETCH_POLICY_H
#define RESONANCE_PATHING_FETCH_POLICY_H

#include "resonance_pathing_inputs_policy.h"
#include "resonance_reflection_cache_publish_policy.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace resonance {

/// Owned SH storage capacity for fetch_pathing_params (order 3 -> 16 coeffs).
constexpr int kPathingFetchedShCoeffs = (kPathingApplyOrderMax + 1) * (kPathingApplyOrderMax + 1);

/// Deep-copy SH coeffs from cache/worker output into caller-owned storage.
/// Returns false when sh_count is out of range or src is null.
/// Live pathing wet: on fetch miss, reuse tail only when pathing ran this worker tick and the capture
/// epoch still matches the published slot (brief cache-flip continuity). Skips (no listener, SEH,
/// cooldown) leave pathing_ran false so stale SH is not held indefinitely.
inline bool pathing_wet_should_apply_stale(bool fetch_ok, bool have_cached_tail_params, uint32_t tail_capture_epoch,
                                           uint32_t slot_epoch, bool pathing_ran_this_tick) {
    if (fetch_ok || !have_cached_tail_params || !pathing_ran_this_tick)
        return false;
    return reflection_cache_entry_epoch_fresh(slot_epoch, tail_capture_epoch);
}

inline bool pathing_fetch_copy_sh_coeffs(std::array<float, kPathingFetchedShCoeffs>& dst, const float* src, int sh_count) {
    if (sh_count <= 0 || !src || sh_count > kPathingFetchedShCoeffs)
        return false;
    std::memcpy(dst.data(), src, static_cast<size_t>(sh_count) * sizeof(float));
    for (int i = sh_count; i < kPathingFetchedShCoeffs; i++)
        dst[static_cast<size_t>(i)] = 0.0f;
    return true;
}

} // namespace resonance

#endif // RESONANCE_PATHING_FETCH_POLICY_H
