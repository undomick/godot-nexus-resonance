#ifndef RESONANCE_TAIL_DRAIN_POLICY_H
#define RESONANCE_TAIL_DRAIN_POLICY_H

#include "resonance_constants.h"
#include <cmath>
#include <cstdint>

namespace resonance {

constexpr float kTailDrainSilenceEpsilon = 1.0e-5f;

struct TailDrainIplGate {
    bool can_run_ipl_tail = false;
    bool force_drain_complete = false;
    bool set_grace_to_zero = false;
};

/// Error paths still output `frames` of silence so AudioServer does not detach _mix before the voice stops.
inline TailDrainIplGate tail_drain_ipl_gate(bool playback_initialized, bool steam_context_stale,
                                            bool srv_initialized, bool context_matches) {
    TailDrainIplGate gate;
    if (!playback_initialized) {
        gate.force_drain_complete = true;
        return gate;
    }
    if (steam_context_stale) {
        // Rings drain without IPL GetTail; grace stays armed so queued output can play out.
        return gate;
    }
    if (!srv_initialized || !context_matches) {
        gate.set_grace_to_zero = true;
        return gate;
    }
    gate.can_run_ipl_tail = true;
    return gate;
}

/// AudioServer detaches _mix when fewer than `frames` are returned; tail drain always reports full frames.
inline int32_t tail_drain_mix_return_frames(int32_t frames) {
    return frames;
}

/// EOS input-ring flush is independent of the live geometry gate (hold_decode only pauses live decode).
inline bool tail_drain_should_flush_input(bool hold_decode) {
    (void)hold_decode;
    return true;
}

struct TailDrainEosPullPlan {
    bool direct = false;
    bool reflection = false;
    bool path = false;
};

inline bool tail_drain_eos_pull_plan_any(const TailDrainEosPullPlan& plan) {
    return plan.direct || plan.reflection || plan.path;
}

/// Which tails this EOS block will pull with process_tail / GetTail.
inline TailDrainEosPullPlan tail_drain_eos_pull_plan(bool gate_open, int direct_tail_samples, bool reflection_have_params,
                                                     int reflection_tail_samples, bool reflection_will_pull,
                                                     bool pathing_enabled, int path_tail_samples, bool path_buffers_ok) {
    TailDrainEosPullPlan plan;
    if (!gate_open)
        return plan;
    if (direct_tail_samples > 0)
        plan.direct = true;
    if (reflection_have_params && reflection_tail_samples > 0 && reflection_will_pull)
        plan.reflection = true;
    if (pathing_enabled && path_tail_samples > 0 && path_buffers_ok)
        plan.path = true;
    return plan;
}

/// Convolution and TAN pull the tail from the mixer, so they need a live mixer handle; GetTail has no cache-epoch gate.
inline bool tail_drain_reflection_eos_will_pull(int reflection_type, bool have_params, int reflection_tail_samples,
                                                bool source_handle_valid, bool mixer_available) {
    if (!source_handle_valid || !have_params || reflection_tail_samples <= 0)
        return false;
    if (reflection_type == resonance::kReflectionConvolution || reflection_type == resonance::kReflectionTan)
        return mixer_available;
    if (reflection_type == resonance::kReflectionParametric || reflection_type == resonance::kReflectionHybrid)
        return true;
    return false;
}

/// The emitter's shared reflection effect gets one Apply or GetTail per round. A draining voice pulls the
/// tail only when it closes a round that no voice fed with dry audio; otherwise the round's Apply carries it.
inline bool reflection_round_should_drain_tail(bool is_round_closer, bool round_applied, bool round_have_dry,
                                               bool tail_applyable, int tail_samples) {
    return is_round_closer && !round_applied && !round_have_dry && tail_applyable && tail_samples > 0;
}

/// Convolution and TAN wet audio sits on the shared bus, not in the player rings, so the rings alone cannot tell.
inline bool tail_drain_eos_actively_pullable(const TailDrainEosPullPlan& plan) {
    return tail_drain_eos_pull_plan_any(plan);
}

inline bool tail_drain_pullable_blocks_drain(int64_t tail_grace_blocks_remaining, bool eos_actively_pullable) {
    return tail_grace_blocks_remaining > 0 && eos_actively_pullable;
}

/// GetTailSize must not gate completion: it can stay > 0 once GetTail stops being pulled. Pullable tails
/// extend the drain only while grace remains.
inline bool tail_drain_complete(bool output_ring_drained, bool reverb_ring_drained,
                                int64_t tail_grace_blocks_remaining, bool eos_actively_pullable = false) {
    if (tail_drain_pullable_blocks_drain(tail_grace_blocks_remaining, eos_actively_pullable))
        return false;
    return output_ring_drained && reverb_ring_drained && tail_grace_blocks_remaining == 0;
}

inline bool reflection_eos_tail_produced(bool tail_complete) {
    return !tail_complete;
}

inline bool tail_grace_end_early(bool produced_any, bool output_ring_drained, bool reverb_ring_drained,
                                 bool last_out_valid, float last_out_l, float last_out_r,
                                 bool eos_actively_pullable = false, int64_t tail_grace_blocks_remaining = 0) {
    if (tail_drain_pullable_blocks_drain(tail_grace_blocks_remaining, eos_actively_pullable))
        return false;
    if (!produced_any || !output_ring_drained || !reverb_ring_drained || !last_out_valid)
        return false;
    return std::fabs(last_out_l) < kTailDrainSilenceEpsilon && std::fabs(last_out_r) < kTailDrainSilenceEpsilon;
}

} // namespace resonance

#endif // RESONANCE_TAIL_DRAIN_POLICY_H
