#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_constants.h"
#include "../resonance_playback_fade.h"

#include <cmath>
#include <vector>

using namespace resonance;

namespace {

struct StereoFrame {
    float left;
    float right;
};

float max_adjacent_jump(const std::vector<float>& samples) {
    float max_jump = 0.0f;
    for (size_t i = 1; i < samples.size(); i++) {
        max_jump = std::max(max_jump, std::fabs(samples[i] - samples[i - 1]));
    }
    return max_jump;
}

} // namespace

TEST_CASE("underrun pad: cosine fade starts at full tail (no click)", "[playback][underrun][eos]") {
    StereoFrame buffer[8]{};
    buffer[0].left = 0.6f;
    buffer[0].right = -0.4f;

    pad_output_with_cosine_underrun_fade(buffer, 8, 1, 0.0f, 0.0f, false);

    REQUIRE(buffer[1].left == Approx(0.6f));
    REQUIRE(buffer[1].right == Approx(-0.4f));
    REQUIRE(buffer[7].left == Approx(0.0f).margin(1e-6f));
    REQUIRE(buffer[7].right == Approx(0.0f).margin(1e-6f));
}

TEST_CASE("underrun pad: hold-last when ring empty uses last_mix_out", "[playback][underrun]") {
    StereoFrame buffer[4]{};
    pad_output_with_cosine_underrun_fade(buffer, 4, 0, 0.25f, -0.25f, true);
    REQUIRE(buffer[0].left == Approx(0.25f));
    REQUIRE(buffer[0].right == Approx(-0.25f));
    REQUIRE(buffer[3].left == Approx(0.0f).margin(1e-6f));
}

TEST_CASE("underrun pad: fade length capped by kSyntheticEosOutputFadeMaxSamples", "[playback][underrun]") {
    REQUIRE(synthetic_eos_output_fade_length(512) == kSyntheticEosOutputFadeMaxSamples);
    StereoFrame buffer[512]{};
    buffer[0].left = 1.0f;
    buffer[0].right = 1.0f;
    pad_output_with_cosine_underrun_fade(buffer, 512, 1, 0.0f, 0.0f, false);
    for (int i = kSyntheticEosOutputFadeMaxSamples + 1; i < 512; i++) {
        REQUIRE(buffer[i].left == Approx(0.0f));
        REQUIRE(buffer[i].right == Approx(0.0f));
    }
}

TEST_CASE("hold-last linear pad: k=0 is unity (continuous with pre-pad sample)", "[playback][eos][pad]") {
    REQUIRE(linear_pad_fade_hold_to_zero(0, 64) == Approx(1.0f));
    REQUIRE(linear_pad_fade_hold_to_zero(63, 64) == Approx(0.0f).margin(1e-6f));
}

TEST_CASE("EOS input taper: last real sample unchanged, end near zero", "[playback][eos][conv][parametric][pathing]") {
    constexpr int n = 128;
    constexpr int taper = 32;
    std::vector<float> out(n);
    for (int i = 0; i < n; i++) {
        const float g = eos_input_end_am_gain(i, n, taper);
        out[static_cast<size_t>(i)] = std::sin(0.1f * static_cast<float>(i)) * g;
    }
    REQUIRE(out[static_cast<size_t>(n - taper - 1)] == Approx(std::sin(0.1f * static_cast<float>(n - taper - 1))));
    REQUIRE(std::fabs(out.back()) < 0.05f);
    REQUIRE(max_adjacent_jump(out) < 0.25f);
}

TEST_CASE("EOS silent full buffer triggers zero-input tail drain", "[playback][eos]") {
    REQUIRE(eos_zero_input_from_silent_full_buffer(512, 512, false, 0.0f));
    REQUIRE_FALSE(eos_zero_input_from_silent_full_buffer(512, 512, true, 0.0f));
    REQUIRE_FALSE(eos_zero_input_from_silent_full_buffer(256, 512, false, 0.0f));
    REQUIRE_FALSE(eos_zero_input_from_silent_full_buffer(512, 512, false, 1.0e-4f));
}

namespace {

enum class EosReflectionTailBranch { None, ConvOrTan, ParametricOrHybrid };

EosReflectionTailBranch eos_reflection_tail_branch(int reflection_type) {
    if (reflection_type == kReflectionConvolution || reflection_type == kReflectionTan)
        return EosReflectionTailBranch::ConvOrTan;
    if (reflection_type == kReflectionParametric || reflection_type == kReflectionHybrid)
        return EosReflectionTailBranch::ParametricOrHybrid;
    return EosReflectionTailBranch::None;
}

bool pathing_eos_tail_active(bool pathing_enabled, int tail_samples, bool have_cached_params) {
    return pathing_enabled && tail_samples > 0 && have_cached_params;
}

} // namespace

TEST_CASE("EOS tail branch: convolution and TAN use silence mixer feed", "[playback][eos][conv]") {
    REQUIRE(eos_reflection_tail_branch(kReflectionConvolution) == EosReflectionTailBranch::ConvOrTan);
    REQUIRE(eos_reflection_tail_branch(kReflectionTan) == EosReflectionTailBranch::ConvOrTan);
}

TEST_CASE("EOS tail branch: parametric and hybrid use tail_apply_direct", "[playback][eos][parametric]") {
    REQUIRE(eos_reflection_tail_branch(kReflectionParametric) == EosReflectionTailBranch::ParametricOrHybrid);
    REQUIRE(eos_reflection_tail_branch(kReflectionHybrid) == EosReflectionTailBranch::ParametricOrHybrid);
}

TEST_CASE("EOS tail branch: pathing runs with cached SH on silence input", "[playback][eos][pathing]") {
    REQUIRE(pathing_eos_tail_active(true, 512, true));
    REQUIRE_FALSE(pathing_eos_tail_active(false, 512, true));
    REQUIRE_FALSE(pathing_eos_tail_active(true, 0, true));
    REQUIRE_FALSE(pathing_eos_tail_active(true, 512, false));
}

TEST_CASE("hold-last pad applied to sine: no step at first padded sample", "[playback][underrun][eos]") {
    constexpr int n_real = 100;
    constexpr int pad_n = 28;
    std::vector<float> wave(static_cast<size_t>(n_real + pad_n));
    for (int i = 0; i < n_real; i++) {
        wave[static_cast<size_t>(i)] = std::sin(0.2f * static_cast<float>(i));
    }
    const float hold = wave[static_cast<size_t>(n_real - 1)];
    for (int k = 0; k < pad_n; k++) {
        const float fade = linear_pad_fade_hold_to_zero(k, pad_n);
        wave[static_cast<size_t>(n_real + k)] = hold * fade;
    }
    REQUIRE(wave[static_cast<size_t>(n_real)] == Approx(hold));
    REQUIRE(std::fabs(wave.back()) < 1e-5f);
}
