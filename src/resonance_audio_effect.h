#ifndef RESONANCE_AUDIO_EFFECT_H
#define RESONANCE_AUDIO_EFFECT_H

#include "resonance_exit_pins.h"
#include "resonance_listener_reverb_bus.h"
#include "resonance_mixer_processor.h"
#include <godot_cpp/classes/audio_effect.hpp>
#include <godot_cpp/classes/audio_effect_instance.hpp>
#include <godot_cpp/classes/ref.hpp>

namespace godot {

class ResonanceAudioEffect;

/// Wet only: the bus input is the dry send for the listener reverb, never passed through.
class ResonanceAudioEffectInstance : public AudioEffectInstance {
    GDCLASS(ResonanceAudioEffectInstance, AudioEffectInstance)

  public:
    ResonanceAudioEffectInstance(const ResonanceAudioEffectInstance&) = delete;
    ResonanceAudioEffectInstance(ResonanceAudioEffectInstance&&) = delete;
    // No explicit deleted assignment: the declaration conflicts with the Godot base.

  private:
    void _reset_ipl_mixer_for_context_lifecycle();

    ResonanceMixerProcessor processor;
    ResonanceListenerReverbBus listener_reverb_;
    bool initialized_processor = false;
    Ref<ResonanceAudioEffect> effect_ref;
    float prev_peak_ = 0.0f;
    resonance::ExitPinned exit_pin_{this};

  public:
    ResonanceAudioEffectInstance() = default;
    ~ResonanceAudioEffectInstance();

    void set_effect(const Ref<ResonanceAudioEffect>& p_effect) { effect_ref = p_effect; }

    /// Main thread. Allocates the IPL state up front so the first bus _process does not.
    bool try_prewarm_processor();
    /// Main thread.
    static void try_prewarm_all_live_instances();

    /// Called by ResonanceServer before iplContextRelease (userdata = this).
    static void ipl_context_reinit_cleanup(void* userdata);

    virtual void _process(const void* src_buffer, AudioFrame* dst_buffer, int32_t frame_count) override;
    virtual bool _process_silence() const override { return true; } // The mixer wet can be non-silent on silent input.

  protected:
    static void _bind_methods() {}
};

class ResonanceAudioEffect : public AudioEffect {
    GDCLASS(ResonanceAudioEffect, AudioEffect)

  private:
    // Default -10 dB to keep the reverb bus from routinely clipping on hot IRs at gain_db=0.
    float gain_db = -10.0f;
    resonance::ExitPinned exit_pin_{this};

  public:
    ResonanceAudioEffect();
    ~ResonanceAudioEffect() = default;

    void set_gain_db(float p_db);
    float get_gain_db() const;

    virtual Ref<AudioEffectInstance> _instantiate() override;

  protected:
    static void _bind_methods();
};

/// Removes every ResonanceAudioEffect from the AudioServer buses so their instances die now.
void detach_resonance_bus_effects();

} // namespace godot

#endif