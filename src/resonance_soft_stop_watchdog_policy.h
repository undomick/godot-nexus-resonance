#ifndef RESONANCE_SOFT_STOP_WATCHDOG_POLICY_H
#define RESONANCE_SOFT_STOP_WATCHDOG_POLICY_H

namespace resonance {

/// Stall threshold before treating soft-stop as a dead mix callback (Dummy driver / no _mix).
constexpr double kSoftStopMixStallThresholdSec = 0.5;
/// Absolute cap while mix is still advancing (long tails must not be cut at max_reverb + margin).
constexpr double kSoftStopActiveDrainCapMultiplier = 3.0;
constexpr double kSoftStopActiveDrainCapMarginSec = 2.0;

/// Force AudioStreamPlayer3D::stop when soft-stop watchdog should give up.
/// Active mix (stall below threshold): only the generous absolute cap applies.
/// Stalled mix: allow the legacy max_reverb + margin cap so voices do not stick forever.
inline bool soft_stop_watchdog_should_force_stop(float max_reverb_duration_sec, double elapsed_sec,
                                                 double mix_stall_sec) {
    const double max_rev = static_cast<double>(max_reverb_duration_sec);
    const double active_cap = max_rev * kSoftStopActiveDrainCapMultiplier + kSoftStopActiveDrainCapMarginSec;
    if (elapsed_sec >= active_cap)
        return true;
    if (mix_stall_sec >= kSoftStopMixStallThresholdSec) {
        const double stalled_cap = max_rev + 0.5;
        return elapsed_sec >= stalled_cap;
    }
    return false;
}

} // namespace resonance

#endif // RESONANCE_SOFT_STOP_WATCHDOG_POLICY_H
