#ifndef RESONANCE_ENERGY_FIELD_QUERY_POLICY_H
#define RESONANCE_ENERGY_FIELD_QUERY_POLICY_H

#include <cstdint>

namespace resonance {

/// Steam Audio energy fields store 3 diffuse bands per channel; band count is not a separate IPL getter.
constexpr int kEnergyFieldDiffuseBands = 3;

/// Skip IR reconstruct below this total (empty / missing baked energy).
constexpr float kEnergyFieldReconstructMinTotal = 1e-9f;

constexpr const char* kProbeQueryErrNoContext = "no_context";
constexpr const char* kProbeQueryErrProbeDataMissing = "probe_data_missing";
constexpr const char* kProbeQueryErrInvalidContextOrProbeData = "invalid_context_or_probe_data";
constexpr const char* kProbeQueryErrProbeBatchLoadFailed = "probe_batch_load_failed";
constexpr const char* kProbeQueryErrProbeIndexOutOfRange = "probe_index_out_of_range";
constexpr const char* kProbeQueryErrNoProbes = "no_probes";
constexpr const char* kProbeQueryErrNoNeighborsInRadius = "no_neighbors_in_radius";
constexpr const char* kProbeQueryErrEnergyFieldCreateFailed = "energy_field_create_failed";

inline bool energy_field_pack_skip(bool field_valid, int channels, int bins, bool has_data) {
    return !field_valid || channels <= 0 || bins <= 0 || !has_data;
}

inline int64_t energy_field_sample_count(int channels, int bins) {
    if (channels <= 0 || bins <= 0)
        return 0;
    return static_cast<int64_t>(channels) * kEnergyFieldDiffuseBands * bins;
}

inline float energy_field_total_from_samples(const float* data, int64_t count) {
    if (!data || count <= 0)
        return 0.0f;
    double sum = 0.0;
    for (int64_t i = 0; i < count; ++i) {
        if (data[i] > 0.0f)
            sum += static_cast<double>(data[i]);
    }
    return static_cast<float>(sum);
}

inline bool energy_field_should_reconstruct_ir(bool reconstruct_requested, float total_energy) {
    return reconstruct_requested && total_energy > kEnergyFieldReconstructMinTotal;
}

inline bool energy_field_reconstruct_ir_args_ok(bool has_context, bool has_field, bool has_out_ir) {
    return has_context && has_field && has_out_ir;
}

} // namespace resonance

#endif
