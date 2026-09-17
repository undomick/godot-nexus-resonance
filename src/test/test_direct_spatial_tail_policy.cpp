#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_direct_spatial_tail_policy.h"

using namespace resonance;

TEST_CASE("direct spatial tail: Steam Audio direct effect has no EOS tail samples", "[playback][eos][direct][spatial]") {
    REQUIRE_FALSE(direct_effect_has_eos_tail(0));
    REQUIRE(direct_effect_has_eos_tail(512));
}

TEST_CASE("direct spatial tail: binaural GetTail gates EOS when direct tail is empty", "[playback][eos][direct][spatial]") {
    REQUIRE(direct_spatial_tail_active(true, false, 0, 256, 0));
    REQUIRE_FALSE(direct_spatial_tail_active(false, false, 0, 256, 0));
    REQUIRE_FALSE(direct_spatial_tail_active(true, false, 0, 0, 0));
}

TEST_CASE("direct spatial tail: HOA binaural GetTail gates EOS on ambisonics encode path", "[playback][eos][direct][spatial]") {
    REQUIRE(direct_spatial_tail_active(true, true, 0, 0, 128));
    REQUIRE_FALSE(direct_spatial_tail_active(true, false, 0, 0, 128));
    REQUIRE_FALSE(direct_spatial_tail_active(true, true, 0, 0, 0));
}

TEST_CASE("direct spatial tail: FMOD cascade uses Apply while direct tail remains", "[playback][eos][direct][spatial]") {
    REQUIRE(direct_spatial_tail_uses_apply_on_direct_mono(64));
    REQUIRE_FALSE(direct_spatial_tail_uses_spatial_get_tail(64));
}

TEST_CASE("direct spatial tail: after direct tail, spatialization drains via GetTail", "[playback][eos][direct][spatial]") {
    REQUIRE(direct_spatial_tail_uses_spatial_get_tail(0));
    REQUIRE_FALSE(direct_spatial_tail_uses_apply_on_direct_mono(0));
}

TEST_CASE("direct spatial tail: produced until IPL reports TAILCOMPLETE", "[playback][eos][direct][spatial]") {
    REQUIRE(direct_spatial_tail_produced(IPL_AUDIOEFFECTSTATE_TAILREMAINING));
    REQUIRE_FALSE(direct_spatial_tail_produced(IPL_AUDIOEFFECTSTATE_TAILCOMPLETE));
}

TEST_CASE("direct spatial tail: spatial_blend blend waits for both binaural and HOA tails", "[playback][eos][direct][spatial]") {
    REQUIRE(direct_spatial_blend_tail_active(IPL_AUDIOEFFECTSTATE_TAILREMAINING, IPL_AUDIOEFFECTSTATE_TAILCOMPLETE));
    REQUIRE(direct_spatial_blend_tail_active(IPL_AUDIOEFFECTSTATE_TAILCOMPLETE, IPL_AUDIOEFFECTSTATE_TAILREMAINING));
    REQUIRE_FALSE(direct_spatial_blend_tail_active(IPL_AUDIOEFFECTSTATE_TAILCOMPLETE, IPL_AUDIOEFFECTSTATE_TAILCOMPLETE));
}

TEST_CASE("direct spatial tail: sample budget is max of active stage tails", "[playback][eos][direct][spatial]") {
    REQUIRE(direct_spatial_tail_sample_budget(true, false, 0, 256, 0) == 256);
    REQUIRE(direct_spatial_tail_sample_budget(true, true, 0, 128, 512) == 512);
    REQUIRE(direct_spatial_tail_sample_budget(true, false, 64, 0, 0) == 64);
    REQUIRE(direct_spatial_tail_sample_budget(true, false, 0, 0, 0) == 0);
}
