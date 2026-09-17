#ifndef RESONANCE_PROBE_INTERPOLATION_POLICY_H
#define RESONANCE_PROBE_INTERPOLATION_POLICY_H

#include <cmath>
#include <cstdint>
#include <vector>

namespace resonance {

struct ProbeNeighborWeight {
    int32_t index = -1;
    float weight = 0.0f;
};

/// Inverse-distance-squared weights for probes within [neighbor_radius_m] of [query_position].
inline void probe_neighbors_inverse_distance_squared(const float* probe_xyz, int32_t probe_count, float query_x, float query_y,
                                                     float query_z, float neighbor_radius_m, std::vector<ProbeNeighborWeight>& out_neighbors,
                                                     float& out_weight_sum) {
    out_neighbors.clear();
    out_weight_sum = 0.0f;
    if (!probe_xyz || probe_count <= 0 || neighbor_radius_m <= 0.0f)
        return;
    out_neighbors.reserve(16);
    for (int32_t i = 0; i < probe_count; ++i) {
        const float px = probe_xyz[i * 3 + 0];
        const float py = probe_xyz[i * 3 + 1];
        const float pz = probe_xyz[i * 3 + 2];
        const float dx = px - query_x;
        const float dy = py - query_y;
        const float dz = pz - query_z;
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > neighbor_radius_m)
            continue;
        const float w = 1.0f / std::max(dist, 0.1f);
        out_neighbors.push_back({i, w * w});
        out_weight_sum += out_neighbors.back().weight;
    }
}

} // namespace resonance

#endif
