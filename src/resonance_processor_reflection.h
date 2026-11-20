#ifndef RESONANCE_PROCESSOR_REFLECTION_H
#define RESONANCE_PROCESSOR_REFLECTION_H

#include "resonance_block_stage_timer.h"
#include "resonance_constants.h"
#include "resonance_reflection_effect_recipe.h"
#include <cstdint>
#include <phonon.h>

namespace godot {

enum class ReflectionInitFlags : int {
    NONE = 0,
    REFLECTIONEFFECT = 1 << 0,
    BUFFERS = 1 << 1,
};
inline ReflectionInitFlags operator|(ReflectionInitFlags a, ReflectionInitFlags b) {
    return static_cast<ReflectionInitFlags>(static_cast<int>(a) | static_cast<int>(b));
}
inline bool operator&(ReflectionInitFlags a, ReflectionInitFlags b) { return (static_cast<int>(a) & static_cast<int>(b)) != 0; }

struct DetachedReflectionEffect {
    IPLReflectionEffect effect = nullptr;
    ReflectionEffectRecipe recipe;
};

class ResonanceReflectionProcessor {
  private:
    IPLContext context = nullptr;
    IPLReflectionEffect reflection_effect = nullptr;

    IPLAudioBuffer sa_mono_buffer{};
    IPLAudioBuffer sa_temp_out_buffer{}; // Apply needs an output buffer even when it feeds a mixer

    ReflectionInitFlags init_flags = ReflectionInitFlags::NONE;
    int frame_size = resonance::kGodotDefaultFrameSize;
    int sample_rate = 48000;
    int num_channels = 4; // Ambisonic channels for convolution, 1 for parametric
    int reflection_type = resonance::kReflectionConvolution;
    float effect_ir_duration_sec_ = resonance::kDefaultReverbDurationSec;
    /// 0 = no cap. Caps the applied IR length for convolution, hybrid and TAN.
    int convolution_ir_max_samples_ = 0;
    int effect_max_ir_samples_ = 0;
    ReflectionEffectRecipe recipe_;
    resonance::DurationTotals effect_apply_;

  public:
    ResonanceReflectionProcessor() = default;
    ~ResonanceReflectionProcessor();

    ResonanceReflectionProcessor(const ResonanceReflectionProcessor&) = delete;
    ResonanceReflectionProcessor& operator=(const ResonanceReflectionProcessor&) = delete;
    ResonanceReflectionProcessor(ResonanceReflectionProcessor&&) = delete;
    ResonanceReflectionProcessor& operator=(ResonanceReflectionProcessor&&) = delete;

    static ReflectionEffectRecipe effect_recipe(int p_sample_rate, int p_frame_size, int p_ambisonic_order, int p_reflection_type,
                                                float p_ir_duration_sec, int p_convolution_ir_max_samples);

    /// p_ir_duration_sec and p_convolution_ir_max_samples must match the server's simulation settings.
    /// p_pooled_effect, when set, must come from effect_recipe() with the same arguments; ownership moves here.
    void initialize(IPLContext p_context, int p_sample_rate, int p_frame_size, int p_ambisonic_order, int p_reflection_type,
                    float p_ir_duration_sec, int p_convolution_ir_max_samples = 0, IPLReflectionEffect p_pooled_effect = nullptr);
    /// Hands the effect and the recipe it was built from to the caller, so cleanup() does not release it.
    DetachedReflectionEffect detach_effect();
    void cleanup();

    /// Ramps the mono downmix from prev to current mix level (prev < 0: constant gain), then applies wet_extra_gain.
    /// Returns false when convolution or hybrid has no IR, so the caller keeps its ramp state.
    bool process_mix(const IPLAudioBuffer& in_buffer,
                     const IPLReflectionEffectParams& reverb_params,
                     IPLReflectionMixer mixer_handle,
                     float prev_reflections_mix_level,
                     float reflections_mix_level,
                     float wet_extra_gain);

    /// Applies without a mixer; the output stays in get_direct_output_buffer() until the next call.
    /// Callers start prev_reflections_mix_level at 0 to ramp from silence.
    bool process_mix_direct(const IPLAudioBuffer& in_buffer, const IPLReflectionEffectParams& reverb_params,
                            float prev_reflections_mix_level, float reflections_mix_level);
    IPLAudioBuffer* get_direct_output_buffer() { return &sa_temp_out_buffer; }
    bool is_parametric() const { return reflection_type == resonance::kReflectionParametric; }

    /// Tail decay when dry input stopped (parametric/hybrid direct decode path).
    IPLAudioEffectState tail_apply_direct(IPLReflectionEffectParams* params);
    /// Tail into the reflection mixer (reverb bus path).
    IPLAudioEffectState tail_apply_to_mixer(IPLReflectionEffectParams* params, IPLReflectionMixer mixer);
    int get_tail_size_samples() const;
    void reset_effect();

    /// Time inside iplReflectionEffectApply and iplReflectionEffectGetTail: the sum and the longest single call.
    const resonance::DurationTotals& get_effect_apply_times() const { return effect_apply_; }
    void reset_effect_apply_times() { effect_apply_.reset(); }

  private:
    void sanitize_reflection_params(IPLReflectionEffectParams* params) const;
    IPLAudioEffectState tail_apply(IPLReflectionEffectParams* params, IPLReflectionMixer mixer);
};

} // namespace godot

#endif