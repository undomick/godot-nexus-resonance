#ifndef RESONANCE_BAKER_IPL_POLICY_H
#define RESONANCE_BAKER_IPL_POLICY_H

#include <atomic>
#include <cmath>
#include <cstdint>
#include <phonon.h>

namespace resonance {

/// Steam Audio 4.8.1: ipl*BakerBake is void; track outcome via progress + optional cancel flag.
struct BakerProgressState {
    float last_progress = 0.0f;
    bool saw_progress = false;
    bool cancelled = false;
};

enum class BakerOutcome : uint8_t {
    Success,
    Cancelled,
    Failure,
};

inline void baker_progress_note(BakerProgressState& state, float progress, bool cancel_flag_set) {
    state.saw_progress = true;
    state.last_progress = progress;
    if (!std::isfinite(progress))
        state.cancelled = true;
    if (cancel_flag_set)
        state.cancelled = true;
}

inline BakerOutcome baker_outcome_after_void_bake(const BakerProgressState& state, bool cancel_flag_set) {
    if (cancel_flag_set || state.cancelled)
        return BakerOutcome::Cancelled;
    return BakerOutcome::Success;
}

inline bool baker_probe_layer_has_data(IPLProbeBatch batch, const IPLBakedDataIdentifier& id) {
    if (!batch)
        return false;
    IPLBakedDataIdentifier query = id;
    return iplProbeBatchGetDataSize(batch, &query) > 0;
}

inline IPLerror baker_outcome_to_ipl_status(BakerOutcome outcome) {
    if (outcome == BakerOutcome::Success)
        return IPL_STATUS_SUCCESS;
    return IPL_STATUS_FAILURE;
}

} // namespace resonance

#endif // RESONANCE_BAKER_IPL_POLICY_H
