#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_probe_influence.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <random>
#include <vector>

namespace {

using Pt = std::array<float, 3>;

std::vector<Pt> make_grid_3d(float spacing, int nx, int ny, int nz) {
    std::vector<Pt> pts;
    for (int x = 0; x < nx; ++x)
        for (int y = 0; y < ny; ++y)
            for (int z = 0; z < nz; ++z)
                pts.push_back({x * spacing, y * spacing, z * spacing});
    return pts;
}

std::vector<Pt> make_grid_floor(float spacing, int nx, int nz) {
    std::vector<Pt> pts;
    for (int x = 0; x < nx; ++x)
        for (int z = 0; z < nz; ++z)
            pts.push_back({x * spacing, 0.0f, z * spacing});
    return pts;
}

std::vector<Pt> make_grid_yz(float spacing, int ny, int nz) {
    std::vector<Pt> pts;
    for (int y = 0; y < ny; ++y)
        for (int z = 0; z < nz; ++z)
            pts.push_back({0.0f, y * spacing, z * spacing});
    return pts;
}

// Reference implementation: the original all-pairs scan semantics
// (nearest-neighbor distance, floored at kBakerStaticEndpointSphereRadius,
// with the FLT_MAX and nearest > floor guards preserved).
std::vector<float> brute_force_radii(const std::vector<Pt>& centers) {
    const float floor_radius = resonance::kBakerStaticEndpointSphereRadius;
    std::vector<float> radii(centers.size(), floor_radius);
    for (std::size_t i = 0; i < centers.size(); ++i) {
        float nearest = std::numeric_limits<float>::max();
        for (std::size_t j = 0; j < centers.size(); ++j) {
            if (j == i)
                continue;
            const float dx = centers[i][0] - centers[j][0];
            const float dy = centers[i][1] - centers[j][1];
            const float dz = centers[i][2] - centers[j][2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d < nearest)
                nearest = d;
        }
        if (nearest < std::numeric_limits<float>::max() && nearest > floor_radius)
            radii[i] = nearest;
    }
    return radii;
}

void require_matches_brute_force(const std::vector<Pt>& centers) {
    const std::vector<float> got = resonance::probe_influence_radii(centers);
    const std::vector<float> want = brute_force_radii(centers);
    REQUIRE(got.size() == want.size());
    for (std::size_t i = 0; i < want.size(); ++i)
        REQUIRE(got[i] == Approx(want[i]).margin(1e-4f));
}

} // namespace

TEST_CASE("probe_influence_radii degenerate layouts keep the floor radius", "[baker][probes]") {
    SECTION("empty layout") {
        REQUIRE(resonance::probe_influence_radii({}).empty());
    }
    SECTION("single probe") {
        const std::vector<float> r = resonance::probe_influence_radii({Pt{1.0f, 2.0f, 3.0f}});
        REQUIRE(r.size() == 1);
        REQUIRE(r[0] == Approx(resonance::kBakerStaticEndpointSphereRadius).margin(1e-6f));
    }
    SECTION("duplicate probes (nearest == 0)") {
        const std::vector<float> r = resonance::probe_influence_radii({Pt{5.0f, 5.0f, 5.0f}, Pt{5.0f, 5.0f, 5.0f}});
        REQUIRE(r.size() == 2);
        REQUIRE(r[0] == Approx(resonance::kBakerStaticEndpointSphereRadius).margin(1e-6f));
        REQUIRE(r[1] == Approx(resonance::kBakerStaticEndpointSphereRadius).margin(1e-6f));
    }
    SECTION("spacing exactly at the floor keeps the floor") {
        // Matches the original `nearest > floor` guard: == is not >.
        const std::vector<float> r = resonance::probe_influence_radii({Pt{0.0f, 0.0f, 0.0f}, Pt{1.0f, 0.0f, 0.0f}});
        REQUIRE(r[0] == Approx(resonance::kBakerStaticEndpointSphereRadius).margin(1e-6f));
        REQUIRE(r[1] == Approx(resonance::kBakerStaticEndpointSphereRadius).margin(1e-6f));
    }
    SECTION("dense sub-floor grid keeps the floor") {
        const std::vector<float> r = resonance::probe_influence_radii(make_grid_3d(0.5f, 3, 3, 3));
        for (float radius : r)
            REQUIRE(radius == Approx(resonance::kBakerStaticEndpointSphereRadius).margin(1e-6f));
    }
}

TEST_CASE("probe_influence_radii uses the grid spacing (Steam Audio generator parity)", "[baker][probes]") {
    const float spacing = 2.0f;
    SECTION("3D volume grid") {
        const std::vector<float> r = resonance::probe_influence_radii(make_grid_3d(spacing, 3, 3, 3));
        REQUIRE(r.size() == 27u);
        for (float radius : r)
            REQUIRE(radius == Approx(spacing).margin(1e-6f));
    }
    SECTION("floor grid") {
        const std::vector<float> r = resonance::probe_influence_radii(make_grid_floor(spacing, 4, 4));
        REQUIRE(r.size() == 16u);
        for (float radius : r)
            REQUIRE(radius == Approx(spacing).margin(1e-6f));
    }
    SECTION("radius covers listeners between probes (bug 2 regression)") {
        // The original bug: a fixed 1.0 m radius left listeners > 1 m from every
        // probe outside all influence spheres (silent wet path). With radius ==
        // spacing, the farthest point from the nearest probe (cell space-diagonal
        // center) is sqrt(3)/2 * spacing, inside the sphere.
        const std::vector<float> r = resonance::probe_influence_radii(make_grid_3d(spacing, 4, 4, 4));
        REQUIRE(std::sqrt(3.0f) * 0.5f * spacing < r.front());
    }
}

TEST_CASE("probe_influence_radii matches the all-pairs reference", "[baker][probes]") {
    SECTION("regular grids") {
        require_matches_brute_force(make_grid_3d(2.0f, 4, 4, 4));
        require_matches_brute_force(make_grid_floor(2.0f, 5, 5));
    }
    SECTION("rotated grid (30 degrees around y)") {
        const float c = std::cos(0.5235988f);
        const float s = std::sin(0.5235988f);
        std::vector<Pt> centers;
        for (const Pt& p : make_grid_floor(2.0f, 4, 4))
            centers.push_back({c * p[0] + s * p[2], p[1], -s * p[0] + c * p[2]});
        require_matches_brute_force(centers);
    }
    SECTION("yz-plane grid (single equal-x run; exercises the run skip)") {
        require_matches_brute_force(make_grid_yz(1.5f, 5, 5));
    }
    SECTION("collinear probes") {
        std::vector<Pt> centers;
        for (int i = 0; i < 12; ++i)
            centers.push_back({0.0f, static_cast<float>(i) * 1.2f, 0.0f});
        require_matches_brute_force(centers);
    }
    SECTION("separated clusters") {
        std::vector<Pt> centers;
        for (int i = 0; i < 3; ++i)
            centers.push_back({2.0f * static_cast<float>(i), 0.0f, 0.0f});
        for (int i = 0; i < 3; ++i)
            centers.push_back({40.0f + 2.0f * static_cast<float>(i), 0.0f, 0.0f});
        require_matches_brute_force(centers);
    }
    SECTION("deterministic pseudo-random cloud") {
        std::mt19937 rng(20260912u);
        std::vector<Pt> centers;
        for (int i = 0; i < 300; ++i)
            centers.push_back({(rng() % 40001) / 1000.0f - 20.0f,
                               (rng() % 40001) / 1000.0f - 20.0f,
                               (rng() % 40001) / 1000.0f - 20.0f});
        require_matches_brute_force(centers);
    }
}
