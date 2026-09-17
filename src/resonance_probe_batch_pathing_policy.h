#ifndef RESONANCE_PROBE_BATCH_PATHING_POLICY_H
#define RESONANCE_PROBE_BATCH_PATHING_POLICY_H

#include <cstdint>
#include <phonon.h>

#include "resonance_baker_ipl_policy.h"

namespace resonance {

/// Steam Audio pathing bake identifier (PATHING + DYNAMIC variation).
inline IPLBakedDataIdentifier pathing_baked_data_identifier() {
    IPLBakedDataIdentifier id{};
    id.type = IPL_BAKEDDATATYPE_PATHING;
    id.variation = IPL_BAKEDDATAVARIATION_DYNAMIC;
    return id;
}

inline bool probe_batch_has_pathing_layer(IPLProbeBatch batch) {
    return baker_probe_layer_has_data(batch, pathing_baked_data_identifier());
}

/// Runtime pathing capability comes from the IPL layer; hash is an incremental-bake hint only.
inline bool resolve_handle_has_pathing_from_load(bool ipl_has_pathing_layer, int64_t pathing_params_hash) {
    (void)pathing_params_hash;
    return ipl_has_pathing_layer;
}

inline bool should_warn_pathing_hash_without_layer(bool ipl_has_pathing_layer, int64_t pathing_params_hash) {
    return pathing_params_hash > 0 && !ipl_has_pathing_layer;
}

} // namespace resonance

#endif // RESONANCE_PROBE_BATCH_PATHING_POLICY_H
