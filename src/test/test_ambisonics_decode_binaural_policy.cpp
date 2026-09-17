#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_ambisonics_decode_binaural_policy.h"

using namespace resonance;

TEST_CASE("ambisonics decode binaural: tri-state override", "[ambisonics][binaural]") {
    REQUIRE(resolve_binaural_override(-1, true));
    REQUIRE_FALSE(resolve_binaural_override(-1, false));
    REQUIRE(resolve_binaural_override(1, false));
    REQUIRE_FALSE(resolve_binaural_override(0, true));
}

TEST_CASE("ambisonics decode binaural: virtual surround gated by apply flag", "[ambisonics][binaural]") {
    REQUIRE(ambisonics_decode_use_virtual_surround_chain(true, true));
    REQUIRE_FALSE(ambisonics_decode_use_virtual_surround_chain(true, false));
    REQUIRE_FALSE(ambisonics_decode_use_virtual_surround_chain(false, true));
}

TEST_CASE("ambisonics decode binaural: effect HRTF params", "[ambisonics][binaural]") {
    IPLHRTF fake = reinterpret_cast<IPLHRTF>(1);
    IPLHRTF out_h = nullptr;
    IPLbool out_b = IPL_FALSE;

    ambisonics_decode_effect_hrtf(true, fake, out_h, out_b);
    REQUIRE(out_h == fake);
    REQUIRE(out_b == IPL_TRUE);

    ambisonics_decode_effect_hrtf(true, nullptr, out_h, out_b);
    REQUIRE(out_h == nullptr);
    REQUIRE(out_b == IPL_FALSE);

    ambisonics_decode_effect_hrtf(false, fake, out_h, out_b);
    REQUIRE(out_h == nullptr);
    REQUIRE(out_b == IPL_FALSE);
}
