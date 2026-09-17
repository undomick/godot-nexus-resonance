#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_processor_hrtf_policy.h"

using namespace resonance;

TEST_CASE("processor HRTF: identity changed when handles differ", "[processor][hrtf]") {
    IPLHRTF a = reinterpret_cast<IPLHRTF>(1);
    IPLHRTF b = reinterpret_cast<IPLHRTF>(2);
    REQUIRE(hrtf_identity_changed(a, b));
    REQUIRE_FALSE(hrtf_identity_changed(a, a));
    REQUIRE(hrtf_identity_changed(nullptr, a));
}

TEST_CASE("processor HRTF: direct needs create when runtime valid but effect missing", "[processor][hrtf]") {
    IPLHRTF h = reinterpret_cast<IPLHRTF>(1);
    REQUIRE(direct_needs_hrtf_effect_create(false, nullptr, h));
    REQUIRE_FALSE(direct_needs_hrtf_effect_create(true, h, h));
    REQUIRE_FALSE(direct_needs_hrtf_effect_create(false, nullptr, nullptr));
}

TEST_CASE("processor HRTF: direct needs recreate on bound/runtime mismatch", "[processor][hrtf]") {
    IPLHRTF old_h = reinterpret_cast<IPLHRTF>(1);
    IPLHRTF new_h = reinterpret_cast<IPLHRTF>(2);
    REQUIRE(direct_needs_hrtf_effect_recreate(true, old_h, new_h));
    REQUIRE_FALSE(direct_needs_hrtf_effect_recreate(false, old_h, new_h));
    REQUIRE_FALSE(direct_needs_hrtf_effect_recreate(true, old_h, old_h));
}

TEST_CASE("processor HRTF: path needs recreate when bound HRTF drifted", "[processor][hrtf]") {
    IPLHRTF old_h = reinterpret_cast<IPLHRTF>(1);
    IPLHRTF new_h = reinterpret_cast<IPLHRTF>(2);
    REQUIRE(path_needs_hrtf_effect_recreate(true, old_h, new_h));
    REQUIRE(path_needs_hrtf_effect_recreate(true, nullptr, new_h));
    REQUIRE_FALSE(path_needs_hrtf_effect_recreate(true, old_h, nullptr));
    REQUIRE_FALSE(path_needs_hrtf_effect_recreate(false, old_h, new_h));
}

TEST_CASE("processor HRTF: ambisonic spatial tail active when decode or rotation tails remain", "[processor][hrtf]") {
    REQUIRE(ambisonic_spatial_tail_active(0, 128));
    REQUIRE(ambisonic_spatial_tail_active(64, 0));
    REQUIRE_FALSE(ambisonic_spatial_tail_active(0, 0));
}

TEST_CASE("processor HRTF: path spatialize requires stereo out buffer", "[processor][path]") {
    float l[512]{};
    float r[512]{};
    float* ch[2] = {l, r};
    IPLAudioBuffer stereo{};
    stereo.data = ch;
    stereo.numChannels = 2;
    stereo.numSamples = 512;
    REQUIRE(path_spatialize_stereo_out_ready(stereo, 512));

    IPLAudioBuffer mono = stereo;
    mono.numChannels = 1;
    REQUIRE_FALSE(path_spatialize_stereo_out_ready(mono, 512));

    IPLAudioBuffer short_buf = stereo;
    short_buf.numSamples = 256;
    REQUIRE_FALSE(path_spatialize_stereo_out_ready(short_buf, 512));
}
