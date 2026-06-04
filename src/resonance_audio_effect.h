#ifndef RESONANCE_AUDIO_EFFECT_H
#define RESONANCE_AUDIO_EFFECT_H

#include "resonance_mixer_processor.h"
#include <godot_cpp/classes/audio_effect.hpp>
#include <godot_cpp/classes/audio_effect_instance.hpp>
#include <godot_cpp/classes/ref.hpp>

namespace godot {

class ResonanceAudioEffect;

/// Resonance Reverb Bus Effect.
/// Reads from IPLReflectionMixer (fed by ResonancePlayer when reflection_type is Convolution). Parametric/Hybrid use per-source process_mix_direct instead.
class ResonanceAudioEffectInstance : public AudioEffectInstance {
    GDCLASS(ResonanceAudioEffectInstance, AudioEffectInstance)

  public:
    ResonanceAudioEffectInstance(const ResonanceAudioEffectInstance&) = delete;
    ResonanceAudioEffectInstance(ResonanceAudioEffectInstance&&) = delete;
    // Assignment implicitly deleted when copy ctor is deleted; explicit decl conflicts with Godot base.

  private:
    void _reset_ipl_mixer_for_context_lifecycle();

    ResonanceMixerProcessor processor;
    bool initialized_processor = false;
    Ref<ResonanceAudioEffect> effect_ref;
    float prev_peak_ = 0.0f;

  public:
    ResonanceAudioEffectInstance() = default;
    ~ResonanceAudioEffectInstance();

    void set_effect(const Ref<ResonanceAudioEffect>& p_effect) { effect_ref = p_effect; }

    /// Main-thread IPL setup when ResonanceServer is ready (avoids alloc in first bus _process).
    bool try_prewarm_processor();

    /// Called by ResonanceServer before iplContextRelease (userdata = this).
    static void ipl_context_reinit_cleanup(void* userdata);

    virtual void _process(const void* src_buffer, AudioFrame* dst_buffer, int32_t frame_count) override;
    virtual bool _process_silence() const override { return true; } // Always process: wet from mixer may be non-silent

  protected:
    static void _bind_methods() {}
};

class ResonanceAudioEffect : public AudioEffect {
    GDCLASS(ResonanceAudioEffect, AudioEffect)

  private:
    // Future: Add Binaural Toggle, Gain, etc. here
    // Default -10 dB to keep the reverb bus from routinely clipping on hot IRs at gain_db=0.
    float gain_db = -10.0f;

  public:
    ResonanceAudioEffect();
    ~ResonanceAudioEffect() = default;

    void set_gain_db(float p_db);
    float get_gain_db() const;

    virtual Ref<AudioEffectInstance> _instantiate() override;

  protected:
    static void _bind_methods();
};

} // namespace godot

#endif