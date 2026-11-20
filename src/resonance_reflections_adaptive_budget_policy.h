#ifndef RESONANCE_REFLECTIONS_ADAPTIVE_BUDGET_POLICY_H
#define RESONANCE_REFLECTIONS_ADAPTIVE_BUDGET_POLICY_H

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace resonance {

struct AdaptiveRayLimits {
    int max_rays = 0;
    int min_rays = 1;
    float recover_frac = 0.0f;
    /// 0 means no cap on the per-pass recovery step.
    int recover_cap = 0;
};

struct AdaptiveIntervalLimits {
    float step_sec = 0.0f;
    float max_extra_sec = 0.0f;
    float decay_per_sec = 0.0f;
};

inline int adaptive_min_num_rays(const AdaptiveRayLimits& limits) {
    return std::max(1, std::min(limits.min_rays, limits.max_rays));
}

/// pass_us is the RunReflections duration measured with current_rays.
inline int adaptive_next_num_rays(int current_rays, uint64_t pass_us, uint64_t budget_us, const AdaptiveRayLimits& limits) {
    const int min_rays = adaptive_min_num_rays(limits);
    if (budget_us > 0 && pass_us > budget_us) {
        const double ratio = static_cast<double>(budget_us) / static_cast<double>(pass_us);
        const int scaled = static_cast<int>(static_cast<double>(current_rays) * ratio);
        return std::clamp(scaled, min_rays, std::max(min_rays, limits.max_rays));
    }
    const float frac = std::clamp(limits.recover_frac, 0.0f, 1.0f);
    int step = static_cast<int>(std::ceil(static_cast<float>(limits.max_rays) * frac));
    if (limits.recover_cap > 0)
        step = std::min(step, limits.recover_cap);
    step = std::max(1, step);
    return std::min(limits.max_rays, current_rays + step);
}

/// Called every tick. A slow pass only widens the interval once, and only when fewer rays cannot help,
/// so the ray and interval controllers never both react to the same measurement.
inline float adaptive_next_extra_interval(float current_extra, bool last_pass_over_budget, bool last_pass_is_new,
                                          bool rays_at_floor, float delta, const AdaptiveIntervalLimits& limits) {
    if (!last_pass_over_budget)
        return std::max(0.0f, current_extra - limits.decay_per_sec * delta);
    if (last_pass_is_new && rays_at_floor)
        return std::min(current_extra + limits.step_sec, limits.max_extra_sec);
    return current_extra;
}

} // namespace resonance

#endif // RESONANCE_REFLECTIONS_ADAPTIVE_BUDGET_POLICY_H
