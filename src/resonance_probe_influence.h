#ifndef RESONANCE_PROBE_INFLUENCE_H
#define RESONANCE_PROBE_INFLUENCE_H

#include "resonance_constants.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace resonance {

/// Per-probe influence radii for a manual probe layout.
///
/// Steam Audio's own probe generators use radius == spacing so any listener
/// position between grid points stays inside at least one influence sphere:
/// ProbeTree::getInfluencingProbes is a strict sphere-containment test, and a
/// miss means ProbeNeighborhood::hasValidProbes() is false and the baked
/// reverb lookup is skipped (silent wet path). Manual layouts have no spacing
/// parameter, so each probe uses its nearest-neighbor distance instead,
/// floored at kBakerStaticEndpointSphereRadius: dense sub-floor grids keep
/// the legacy radius, and lone or duplicated probes have no neighbor-derived
/// radius (FLT_MAX / 0 are both rejected, matching the original all-pairs
/// scan's guards).
///
/// Exact equivalent of the naive all-pairs scan, via a sweep over points
/// sorted lexicographically by (x, y, z): scanning outward from a point, the
/// |x| delta grows monotonically, so once dx^2 >= best no remaining point on
/// that side can be nearer; within an equal-x run the y delta behaves the
/// same way. Grid-like layouts (the only realistic bake input) examine O(1)
/// candidates per probe, leaving the sort dominant.
inline std::vector<float> probe_influence_radii(const std::vector<std::array<float, 3>>& centers) {
    const std::size_t n = centers.size();
    const float floor_radius = kBakerStaticEndpointSphereRadius;
    std::vector<float> radii(n, floor_radius);
    if (n < 2)
        return radii;

    std::vector<std::size_t> order(n);
    for (std::size_t i = 0; i < n; ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&centers](std::size_t a, std::size_t b) {
        const std::array<float, 3>& p = centers[a];
        const std::array<float, 3>& q = centers[b];
        if (p[0] != q[0])
            return p[0] < q[0];
        if (p[1] != q[1])
            return p[1] < q[1];
        return p[2] < q[2];
    });

    const float max_f = std::numeric_limits<float>::max();
    std::vector<float> nearest(n, max_f);
    for (std::size_t ii = 0; ii < n; ++ii) {
        const std::array<float, 3>& p = centers[order[ii]];
        float best = max_f; // squared distance to the nearest neighbor so far

        std::size_t jj = ii;
        while (jj + 1 < n) {
            ++jj;
            const std::array<float, 3>& q = centers[order[jj]];
            const float dx = q[0] - p[0];
            if (dx * dx >= best)
                break;
            if (q[0] == p[0]) {
                // Equal-x points form one contiguous run, sorted by y; dy >= 0 grows.
                const float dy = q[1] - p[1];
                if (dy * dy >= best) {
                    while (jj + 1 < n && centers[order[jj + 1]][0] == p[0])
                        ++jj;
                    continue;
                }
            }
            const float dy = q[1] - p[1];
            const float dz = q[2] - p[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < best)
                best = d2;
        }

        jj = ii;
        while (jj > 0) {
            --jj;
            const std::array<float, 3>& q = centers[order[jj]];
            const float dx = p[0] - q[0];
            if (dx * dx >= best)
                break;
            if (q[0] == p[0]) {
                const float dy = p[1] - q[1];
                if (dy * dy >= best) {
                    while (jj > 0 && centers[order[jj - 1]][0] == p[0])
                        --jj;
                    continue;
                }
            }
            const float dy = p[1] - q[1];
            const float dz = p[2] - q[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < best)
                best = d2;
        }

        if (best < max_f)
            nearest[order[ii]] = std::sqrt(best);
    }

    for (std::size_t i = 0; i < n; ++i) {
        if (nearest[i] < max_f && nearest[i] > floor_radius)
            radii[i] = nearest[i];
    }
    return radii;
}

} // namespace resonance

#endif // RESONANCE_PROBE_INFLUENCE_H
