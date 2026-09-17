#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_pathing_fetch_policy.h"

using namespace resonance;

TEST_CASE("pathing wet stale fallback on fetch miss", "[pathing][wet][continuity]") {
    REQUIRE(pathing_wet_should_apply_stale(false, true, 3u, 3u, true));
    REQUIRE_FALSE(pathing_wet_should_apply_stale(true, true, 3u, 3u, true));
    REQUIRE_FALSE(pathing_wet_should_apply_stale(false, false, 3u, 3u, true));
    REQUIRE_FALSE(pathing_wet_should_apply_stale(false, true, 3u, 3u, false));
    REQUIRE_FALSE(pathing_wet_should_apply_stale(false, true, 2u, 3u, true));
}

TEST_CASE("pathing_fetch_copy_sh_coeffs copies and zero-pads", "[pathing][fetch]") {
    std::array<float, kPathingFetchedShCoeffs> dst{};
    const float src[] = {1.0f, 2.0f, 3.0f, 4.0f};

    REQUIRE(pathing_fetch_copy_sh_coeffs(dst, src, 4));
    REQUIRE(dst[0] == 1.0f);
    REQUIRE(dst[3] == 4.0f);
    for (int i = 4; i < kPathingFetchedShCoeffs; i++)
        REQUIRE(dst[static_cast<size_t>(i)] == 0.0f);
}

TEST_CASE("pathing_fetch_copy_sh_coeffs rejects invalid input", "[pathing][fetch]") {
    std::array<float, kPathingFetchedShCoeffs> dst{};
    const float src[] = {1.0f};

    REQUIRE_FALSE(pathing_fetch_copy_sh_coeffs(dst, nullptr, 1));
    REQUIRE_FALSE(pathing_fetch_copy_sh_coeffs(dst, src, 0));
    REQUIRE_FALSE(pathing_fetch_copy_sh_coeffs(dst, src, kPathingFetchedShCoeffs + 1));
}

TEST_CASE("pathing_fetch_copy_sh_coeffs does not alias source buffer", "[pathing][fetch]") {
    float src[] = {0.5f, 0.25f, 0.125f, 0.0625f};
    std::array<float, kPathingFetchedShCoeffs> owned{};
    REQUIRE(pathing_fetch_copy_sh_coeffs(owned, src, 4));

    src[0] = 99.0f;
    REQUIRE(owned[0] == 0.5f);
}
