#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_constants.h"
#include "../resonance_reflection_fetch_policy.h"

#include <phonon.h>

namespace {

IPLReflectionEffectParams make_params_with_ir() {
    IPLReflectionEffectParams p{};
    p.ir = reinterpret_cast<IPLReflectionEffectIR>(1);
    return p;
}

IPLReflectionEffectParams make_params_null_ir() {
    IPLReflectionEffectParams p{};
    return p;
}

IPLReflectionEffectParams make_parametric_params() {
    IPLReflectionEffectParams p{};
    p.reverbTimes[0] = 0.5f;
    return p;
}

} // namespace

TEST_CASE("stale epoch: convolution mixes non-null IR (Steam TripleBuffer)", "[reflection_fetch][steam]") {
    const auto with_ir = make_params_with_ir();
    REQUIRE(resonance::reflection_stale_epoch_usable_for_mix(resonance::kReflectionConvolution, with_ir));
    REQUIRE(resonance::reflection_stale_epoch_usable_for_mix(resonance::kReflectionTan, with_ir));

    const auto null_ir = make_params_null_ir();
    REQUIRE_FALSE(resonance::reflection_stale_epoch_usable_for_mix(resonance::kReflectionConvolution, null_ir));
    REQUIRE_FALSE(resonance::reflection_stale_epoch_usable_for_mix(resonance::kReflectionTan, null_ir));
}

TEST_CASE("stale epoch: hybrid IR rejected, parametric tail allowed", "[reflection_fetch]") {
    const auto with_ir = make_params_with_ir();
    REQUIRE_FALSE(resonance::reflection_stale_epoch_usable_for_mix(resonance::kReflectionHybrid, with_ir));

    const auto parametric = make_parametric_params();
    REQUIRE(resonance::reflection_stale_epoch_usable_for_mix(resonance::kReflectionHybrid, parametric));
}

TEST_CASE("conv wet mix level applies baked occlusion", "[reflection_fetch]") {
    REQUIRE(resonance::conv_reflection_wet_mix_level(1.0f, 1.0f) == Approx(1.0f));
    REQUIRE(resonance::conv_reflection_wet_mix_level(0.8f, 0.5f) == Approx(0.4f));
    REQUIRE(resonance::conv_reflection_wet_mix_level(0.0f, 0.0f) == Approx(0.0f));
}

TEST_CASE("reflection pending clears after RunReflections for eligible handles only", "[reflection_fetch]") {
    REQUIRE_FALSE(resonance::reflection_pending_should_clear_after_run(false, true, false));
    REQUIRE_FALSE(resonance::reflection_pending_should_clear_after_run(true, false, false));
    REQUIRE_FALSE(resonance::reflection_pending_should_clear_after_run(true, true, true));
    REQUIRE(resonance::reflection_pending_should_clear_after_run(true, true, false));
}

TEST_CASE("EOS TAN tail refreshes tanDevice from live device", "[reflection_fetch]") {
    IPLReflectionEffectParams p{};
    p.type = IPL_REFLECTIONEFFECTTYPE_TAN;
    p.tanDevice = reinterpret_cast<IPLTrueAudioNextDevice>(0x1);
    IPLTrueAudioNextDevice live = reinterpret_cast<IPLTrueAudioNextDevice>(0x2);
    resonance::reflection_eos_tail_refresh_tan_device(resonance::kReflectionTan, p, live);
    REQUIRE(p.tanDevice == live);

    IPLReflectionEffectParams conv{};
    conv.type = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
    conv.tanDevice = reinterpret_cast<IPLTrueAudioNextDevice>(0x1);
    resonance::reflection_eos_tail_refresh_tan_device(resonance::kReflectionConvolution, conv, live);
    REQUIRE(conv.tanDevice == reinterpret_cast<IPLTrueAudioNextDevice>(0x1));
}

TEST_CASE("EOS hybrid strips stale IR but keeps parametric tail", "[reflection_fetch]") {
    IPLReflectionEffectParams p{};
    p.ir = reinterpret_cast<IPLReflectionEffectIR>(1);
    p.type = IPL_REFLECTIONEFFECTTYPE_HYBRID;
    p.reverbTimes[0] = 0.4f;
    resonance::reflection_eos_tail_strip_stale_ir(resonance::kReflectionHybrid, 8u, 7u, p);
    REQUIRE(p.ir == nullptr);
    REQUIRE(p.type == IPL_REFLECTIONEFFECTTYPE_PARAMETRIC);
    REQUIRE(p.reverbTimes[0] == Approx(0.4f));

    IPLReflectionEffectParams fresh = p;
    fresh.ir = reinterpret_cast<IPLReflectionEffectIR>(2);
    fresh.type = IPL_REFLECTIONEFFECTTYPE_HYBRID;
    resonance::reflection_eos_tail_strip_stale_ir(resonance::kReflectionHybrid, 8u, 8u, fresh);
    REQUIRE(fresh.ir != nullptr);
    REQUIRE(fresh.type == IPL_REFLECTIONEFFECTTYPE_HYBRID);
}
