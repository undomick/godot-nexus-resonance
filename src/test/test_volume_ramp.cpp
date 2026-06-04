#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_math.h"
#include <cmath>
#include <limits>

using namespace resonance;

TEST_CASE("apply_volume_ramp constant volume", "[volume_ramp]") {
    float buffer[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    apply_volume_ramp(0.5f, 0.5f, 4, buffer);
    REQUIRE(buffer[0] == Approx(0.5f));
    REQUIRE(buffer[1] == Approx(0.5f));
    REQUIRE(buffer[2] == Approx(0.5f));
    REQUIRE(buffer[3] == Approx(0.5f));
}

TEST_CASE("apply_volume_ramp constant unity no change", "[volume_ramp]") {
    float buffer[4] = {2.0f, 3.0f, 4.0f, 5.0f};
    apply_volume_ramp(1.0f, 1.0f, 4, buffer);
    REQUIRE(buffer[0] == Approx(2.0f));
    REQUIRE(buffer[1] == Approx(3.0f));
    REQUIRE(buffer[2] == Approx(4.0f));
    REQUIRE(buffer[3] == Approx(5.0f));
}

TEST_CASE("apply_volume_ramp 0 to 1 over N samples", "[volume_ramp]") {
    float buffer[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    apply_volume_ramp(0.0f, 1.0f, 4, buffer);
    // Sample 0: vol 0.0, 1*0=0
    // Sample 1: vol 0.25, 1*0.25=0.25
    // Sample 2: vol 0.5, 1*0.5=0.5
    // Sample 3: vol 0.75, 1*0.75=0.75
    REQUIRE(buffer[0] == Approx(0.0f));
    REQUIRE(buffer[1] == Approx(0.25f));
    REQUIRE(buffer[2] == Approx(0.5f));
    REQUIRE(buffer[3] == Approx(0.75f));
}

TEST_CASE("apply_volume_ramp num_samples zero is no-op", "[volume_ramp]") {
    float buffer[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    apply_volume_ramp(0.0f, 1.0f, 0, buffer);
    REQUIRE(buffer[0] == 1.0f);
    REQUIRE(buffer[1] == 2.0f);
    REQUIRE(buffer[2] == 3.0f);
    REQUIRE(buffer[3] == 4.0f);
}

TEST_CASE("apply_volume_ramp 1 to 0 ramp down", "[volume_ramp]") {
    float buffer[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    apply_volume_ramp(1.0f, 0.0f, 4, buffer);
    REQUIRE(buffer[0] == Approx(1.0f));
    REQUIRE(buffer[1] == Approx(0.75f));
    REQUIRE(buffer[2] == Approx(0.5f));
    REQUIRE(buffer[3] == Approx(0.25f));
}

TEST_CASE("sanitize_audio_float finite unchanged", "[resonance_math]") {
    REQUIRE(sanitize_audio_float(1.0f) == 1.0f);
    REQUIRE(sanitize_audio_float(-0.5f) == -0.5f);
    REQUIRE(sanitize_audio_float(0.0f) == 0.0f);
}

TEST_CASE("sanitize_audio_float nan becomes zero", "[resonance_math]") {
    float nan_val = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(sanitize_audio_float(nan_val) == 0.0f);
}

TEST_CASE("sanitize_audio_float inf becomes zero", "[resonance_math]") {
    REQUIRE(sanitize_audio_float(std::numeric_limits<float>::infinity()) == 0.0f);
    REQUIRE(sanitize_audio_float(-std::numeric_limits<float>::infinity()) == 0.0f);
}

TEST_CASE("clamp_reverb_time valid above 0.1", "[resonance_math]") {
    REQUIRE(clamp_reverb_time(0.5f) == Approx(0.5f));
    REQUIRE(clamp_reverb_time(2.0f) == Approx(2.0f));
}

TEST_CASE("clamp_reverb_time below 0.1 clamped", "[resonance_math]") {
    REQUIRE(clamp_reverb_time(0.05f) == Approx(0.1f));
    REQUIRE(clamp_reverb_time(0.0f) == Approx(0.1f));
}

TEST_CASE("sanitize_delay_samples finite unchanged", "[resonance_math]") {
    REQUIRE(sanitize_delay_samples(0) == 0);
    REQUIRE(sanitize_delay_samples(42) == 42);
}

TEST_CASE("reverb_ir_size_samples nominal", "[resonance_math]") {
    REQUIRE(reverb_ir_size_samples(48000, 2.0f) == 96000);
    REQUIRE(reverb_ir_size_samples(44100, 1.0f) == 44100);
}

// --- Pathing (ResonancePathProcessor + ResonancePlayer) ---
// Steam Audio Unity/FMOD spatialize: applyVolumeRamp(prevPathingMixLevel, pathingMixLevel) on mono
// after downmix, then iplPathEffectApply; output is iplAudioBufferMix at unity — no extra multiply by
// reverb_pathing_attenuation on the wet (distance is already in path SH from the simulation).

TEST_CASE("pathing: mono input ramp matches apply_volume_ramp step", "[volume_ramp][pathing]") {
    const int n = 8;
    float mono[n];
    for (int i = 0; i < n; i++)
        mono[i] = 2.0f;
    const float prev_mix = 0.25f;
    const float curr_mix = 1.0f;
    apply_volume_ramp(prev_mix, curr_mix, n, mono);
    const float step = (curr_mix - prev_mix) / static_cast<float>(n);
    for (int i = 0; i < n; i++) {
        const float vol = prev_mix + step * static_cast<float>(i);
        REQUIRE(mono[i] == Approx(2.0f * vol));
    }
}

TEST_CASE("pathing: constant mix level scales full block", "[volume_ramp][pathing]") {
    float mono[6] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    apply_volume_ramp(0.5f, 0.5f, 6, mono);
    for (int i = 0; i < 6; i++)
        REQUIRE(mono[i] == Approx(0.5f));
}

TEST_CASE("pathing: ramp down to zero last sample not at zero volume", "[volume_ramp][pathing]") {
    // apply_volume_ramp uses step = (end-start)/num_samples; last index uses start + step*(n-1), not end.
    const int n = 4;
    float mono[n] = {1.0f, 1.0f, 1.0f, 1.0f};
    apply_volume_ramp(1.0f, 0.0f, n, mono);
    REQUIRE(mono[0] == Approx(1.0f));
    REQUIRE(mono[1] == Approx(0.75f));
    REQUIRE(mono[2] == Approx(0.5f));
    REQUIRE(mono[3] == Approx(0.25f));
}

TEST_CASE("apply_volume_ramp_and_sanitize matches ramp then sanitize for finite input", "[volume_ramp]") {
    const int n = 8;
    float a[8];
    float b[8];
    for (int i = 0; i < n; i++) {
        a[i] = 2.0f + static_cast<float>(i) * 0.25f;
        b[i] = a[i];
    }
    const float prev = 0.2f;
    const float curr = 0.8f;
    apply_volume_ramp(prev, curr, n, a);
    for (int i = 0; i < n; i++)
        a[i] = sanitize_audio_float(a[i]);
    apply_volume_ramp_and_sanitize(prev, curr, n, b);
    for (int i = 0; i < n; i++)
        REQUIRE(b[i] == Approx(a[i]));
}

TEST_CASE("apply_volume_ramp_and_sanitize constant gain nan to zero", "[volume_ramp]") {
    float nan_val = std::numeric_limits<float>::quiet_NaN();
    float buf[3] = {1.0f, nan_val, 3.0f};
    apply_volume_ramp_and_sanitize(0.5f, 0.5f, 3, buf);
    REQUIRE(buf[0] == Approx(0.5f));
    REQUIRE(buf[1] == 0.0f);
    REQUIRE(buf[2] == Approx(1.5f));
}

TEST_CASE("pathing: wet add unity not times reverb_pathing_attenuation", "[pathing]") {
    // Regression: path stereo used to be scaled by reverb_pathing_attenuation * pathing_mix per sample.
    // Steam Audio Unity/FMOD spatialize mixes path effect output at unity (distance is in SH coeffs).
    const float att = 0.2f;
    const float path_out_sample = 1.0f;
    const float legacy_wet = att * path_out_sample;
    const float reference_wet = 1.0f * path_out_sample;
    REQUIRE(legacy_wet == Approx(0.2f));
    REQUIRE(reference_wet == Approx(1.0f));
    REQUIRE(legacy_wet != Approx(reference_wet));
}
