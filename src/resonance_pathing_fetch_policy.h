#ifndef RESONANCE_PATHING_FETCH_POLICY_H
#define RESONANCE_PATHING_FETCH_POLICY_H

#include "resonance_pathing_inputs_policy.h"
#include "resonance_reflection_cache_publish_policy.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace resonance {

/// SH storage capacity for fetch_pathing_params (order 3 -> 16 coeffs).
constexpr int kPathingFetchedShCoeffs = (kPathingApplyOrderMax + 1) * (kPathingApplyOrderMax + 1);

/// On a fetch miss, reuse the cached tail only while pathing results are current and its capture epoch still
/// matches the slot. Passes that could not run pathing leave results not current, so stale SH is not held forever.
inline bool pathing_wet_should_apply_stale(bool fetch_ok, bool have_cached_tail_params, uint32_t tail_capture_epoch,
                                           uint32_t slot_epoch, bool pathing_results_current) {
    if (fetch_ok || !have_cached_tail_params || !pathing_results_current)
        return false;
    return reflection_cache_entry_epoch_fresh(slot_epoch, tail_capture_epoch);
}

/// Zero-fills coeffs past sh_count. Returns false when sh_count is out of range or src is null.
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
