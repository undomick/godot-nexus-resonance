#ifndef RESONANCE_PROCESSOR_REFLECTION_H
#define RESONANCE_PROCESSOR_REFLECTION_H

#include "resonance_constants.h"
#include <phonon.h>

namespace godot {

// Per-playback IPL reflection effect: downmix → Apply (mixer or direct HOA buffer), plus tail helpers. Used by ResonancePlayer / bus effect path.

enum class ReflectionInitFlags : int {
    NONE = 0,
    REFLECTIONEFFECT = 1 << 0,
    BUFFERS = 1 << 1,
    AIRABSORPTIONEFFECT = 1 << 2,
};
inline ReflectionInitFlags operator|(ReflectionInitFlags a, ReflectionInitFlags b) {
    return static_cast<ReflectionInitFlags>(static_cast<int>(a) | static_cast<int>(b));
}
inline bool operator&(ReflectionInitFlags a, ReflectionInitFlags b) { return (static_cast<int>(a) & static_cast<int>(b)) != 0; }

class ResonanceReflectionProcessor {
  private:
    IPLContext context = nullptr;
    IPLReflectionEffect reflection_effect = nullptr;
    /// Secondary IPLDirectEffect used purely as a 3-band air-absorption pre-EQ on the wet mono tap, so the baked
    /// reverb tail picks up source→listener air absorption that the IR itself does not encode (parity with
    /// realtime ray-traced reflections, which include air absorption per ray).
    IPLDirectEffect air_absorption_effect = nullptr;
    IPLAudioBuffer sa_air_absorption_in_buffer{};
    IPLAudioBuffer sa_air_absorption_out_buffer{};

    IPLAudioBuffer sa_mono_buffer{};     // downmixed dry feed for Apply
    IPLAudioBuffer sa_temp_out_buffer{}; // effect output (Ambisonics or parametric mono); API needs a destination even when feeding a mixer

    ReflectionInitFlags init_flags = ReflectionInitFlags::NONE;
    int frame_size = resonance::kGodotDefaultFrameSize;
    int sample_rate = 48000;
    int num_channels = 4;                                    // Ambisonic channels for convolution, 1 for parametric
    int reflection_type = resonance::kReflectionConvolution; // 0=Convolution, 1=Parametric, 2=Hybrid, 3=TAN
    /// IR length (seconds) used for iplReflectionEffectCreate and param sanitize; aligned with ResonanceServer max_reverb_duration.
    float effect_ir_duration_sec_ = resonance::kDefaultReverbDurationSec;
    /// 0 = no cap. Clamp applied IR length (convolution/hybrid/tan) to at most this and effect allocation.
    int convolution_ir_max_samples_ = 0;
    int effect_max_ir_samples_ = 0;
    /// Last wet air-absorption targets (for IIR reset on bypass); not used for manual block ramping.
    resonance::AudioBands3 last_air_absorption_{1.0f, 1.0f, 1.0f};
    bool air_absorption_iir_active_ = false;

  public:
    ResonanceReflectionProcessor() = default;
    ~ResonanceReflectionProcessor();

    ResonanceReflectionProcessor(const ResonanceReflectionProcessor&) = delete;
    ResonanceReflectionProcessor& operator=(const ResonanceReflectionProcessor&) = delete;
    ResonanceReflectionProcessor(ResonanceReflectionProcessor&&) = delete;
    ResonanceReflectionProcessor& operator=(ResonanceReflectionProcessor&&) = delete;

    /// `p_max_reverb_duration_sec` and optional `p_convolution_ir_max_samples` must stay consistent with ResonanceServer (IR allocation vs. sim).
    void initialize(IPLContext p_context, int p_sample_rate, int p_frame_size, int p_ambisonic_order, int p_reflection_type,
                    float p_max_reverb_duration_sec, int p_convolution_ir_max_samples = 0);
    void cleanup();

    /// Downmix, ramp `reflections_mix_level` on mono (prev = -1: no ramp on first block), then `wet_extra_gain` (no cross-fade between blocks).
    /// When `apply_air_absorption` is true, run the mono tap through the air-absorption pre-EQ before Apply (per-block IPL targets).
    /// Returns false if convolution/hybrid IR is null so the caller’s ramp state stays aligned.
    bool process_mix(const IPLAudioBuffer& in_buffer,
                     const IPLReflectionEffectParams& reverb_params,
                     IPLReflectionMixer mixer_handle,
                     float prev_reflections_mix_level,
                     float reflections_mix_level,
                     float wet_extra_gain,
                     bool apply_air_absorption,
                     const resonance::AudioBands3& air_absorption);

    /// Mixer bypass: Apply with `mixer=null`, HOA (or parametric) in `sa_temp_out_buffer` until the next call.
    /// Parametric callers typically start `prev_reflections_mix_level` at 0 (ramp from silence); convolution uses `process_mix` with prev=-1.
    bool process_mix_direct(const IPLAudioBuffer& in_buffer, const IPLReflectionEffectParams& reverb_params,
                            float prev_reflections_mix_level, float reflections_mix_level,
                            bool apply_air_absorption,
                            const resonance::AudioBands3& air_absorption);
    IPLAudioBuffer* get_direct_output_buffer() { return &sa_temp_out_buffer; }
    bool is_parametric() const { return reflection_type == resonance::kReflectionParametric; }

    /// Tail decay when dry input stopped (parametric/hybrid direct decode path).
    IPLAudioEffectState tail_apply_direct(IPLReflectionEffectParams* params);
    /// Tail into the reflection mixer (reverb bus path; same routing idea as Apply with a non-null mixer).
    IPLAudioEffectState tail_apply_to_mixer(IPLReflectionEffectParams* params, IPLReflectionMixer mixer);
    int get_tail_size_samples() const;
    void reset_effect();

  private:
    void sanitize_reflection_params(IPLReflectionEffectParams* params) const;
    /// Run the mono tap through the air-absorption pre-EQ in place. No-op if the effect is not initialized.
    void apply_air_absorption_in_place(const resonance::AudioBands3& target);
    IPLAudioEffectState tail_apply(IPLReflectionEffectParams* params, IPLReflectionMixer mixer);
};

} // namespace godot

#endif