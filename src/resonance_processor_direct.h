#ifndef RESONANCE_PROCESSOR_DIRECT_H
#define RESONANCE_PROCESSOR_DIRECT_H

#include "resonance_constants.h"
#include <atomic>
#include <phonon.h>

namespace godot {

enum class DirectInitFlags : int {
    NONE = 0,
    DIRECT_EFFECT = 1 << 0,
    BINAURAL_EFFECT = 1 << 1,
    PANNING_EFFECT = 1 << 2,
    BUFFERS = 1 << 3,
    BINAURAL_STEREO_SCRATCH = 1 << 6,
};
inline DirectInitFlags operator|(DirectInitFlags a, DirectInitFlags b) {
    return static_cast<DirectInitFlags>(static_cast<int>(a) | static_cast<int>(b));
}
inline DirectInitFlags operator|=(DirectInitFlags& a, DirectInitFlags b) {
    a = a | b;
    return a;
}
inline bool operator&(DirectInitFlags a, DirectInitFlags b) { return (static_cast<int>(a) & static_cast<int>(b)) != 0; }

class ResonanceDirectProcessor {
  private:
    /// Complete when encode is set: encode, binaural and ambi_buffer exist together or not at all.
    struct AmbisonicsEffects {
        IPLAmbisonicsEncodeEffect encode = nullptr;
        IPLAmbisonicsBinauralEffect binaural = nullptr;
        IPLAmbisonicsPanningEffect panning = nullptr;
        IPLAudioBuffer ambi_buffer{};
        IPLAudioBuffer hoa_stereo_scratch{};
    };

    struct HrtfBoundEffects {
        IPLHRTF hrtf = nullptr;
        IPLBinauralEffect binaural = nullptr;
        IPLAudioBuffer binaural_stereo_out{};
        AmbisonicsEffects ambisonics;
        DirectInitFlags flags = DirectInitFlags::NONE;
    };

    IPLContext context = nullptr;
    IPLHRTF hrtf_handle = nullptr;

    IPLDirectEffect direct_effect = nullptr;
    IPLBinauralEffect binaural_effect = nullptr;
    IPLPanningEffect panning_effect = nullptr;
    IPLAmbisonicsEncodeEffect ambisonics_encode_effect = nullptr;
    IPLAmbisonicsBinauralEffect ambisonics_binaural_effect = nullptr;
    IPLAmbisonicsPanningEffect ambisonics_panning_effect = nullptr;

    IPLAudioBuffer internal_mono_buffer{};
    IPLAudioBuffer internal_direct_output{}; // kept apart from the Apply input, as in the Steam Audio reference
    IPLAudioBuffer internal_ambi_buffer{};
    /// Binaural effects write stereo here first; copied to FL/FR when output layout has >2 channels.
    IPLAudioBuffer internal_binaural_stereo_out{};
    /// HOA encode + binaural output, blended with the plain binaural output by spatial_blend.
    IPLAudioBuffer internal_hoa_stereo_scratch{};

    // Settings of the last block, reused to spatialize the tail.
    IPLVector3 last_direction = {0.0f, 0.0f, -1.0f};
    bool last_hrtf_bilinear = false;
    float last_spatial_blend = 1.0f;
    bool last_use_ambisonics_encode_path = false;
    bool last_use_binaural = true;

    DirectInitFlags init_flags = DirectInitFlags::NONE;
    int frame_size = resonance::kGodotDefaultFrameSize;
    int sample_rate = 48000;
    int ambisonic_order = 1;
    bool use_ambisonics_encode = false;
    int speaker_channels = 2;
    IPLHRTF bound_hrtf_ = nullptr;
    /// Published after the ambisonics effects and buffers exist; the audio thread takes that route only then.
    std::atomic<bool> ambisonics_ready_{false};

  public:
    ResonanceDirectProcessor() = default;
    ~ResonanceDirectProcessor();

    ResonanceDirectProcessor(const ResonanceDirectProcessor&) = delete;
    ResonanceDirectProcessor& operator=(const ResonanceDirectProcessor&) = delete;
    ResonanceDirectProcessor(ResonanceDirectProcessor&&) = delete;
    ResonanceDirectProcessor& operator=(ResonanceDirectProcessor&&) = delete;

    /// \param p_speaker_channels Steam Audio layout channel count (1,2,4,6,8); others clamp to stereo.
    void initialize(IPLContext p_context, int p_sample_rate, int p_frame_size, int p_ambisonic_order = 1, bool p_use_ambisonics_encode = false,
                    int p_speaker_channels = 2);
    void cleanup();

    void process(
        bool use_ambisonics_encode_path,
        const IPLAudioBuffer& in_buffer,
        IPLAudioBuffer& out_buffer,
        float attenuation,
        float occlusion,
        const float* transmission,
        const float* air_absorption,
        bool apply_air_absorption,
        float directivity_value,
        bool apply_directivity,
        bool apply_effect,
        bool use_binaural,
        int transmission_type,
        bool hrtf_interpolation_bilinear,
        float spatial_blend,
        const IPLCoordinateSpace3& listener_coords,
        const IPLVector3& source_pos);

    /// Remaining tail samples across the direct, binaural and HOA stages.
    int get_tail_size_samples() const;

    /// Call repeatedly after the input stopped until it returns false. Spatializes with the last block's direction and settings.
    bool process_tail(IPLAudioBuffer& out_buffer);

    /// Clears any pending tail.
    void reset_for_new_playback();

    /// Main thread: create or recreate HRTF-bound effects when HRTF arrives or identity changes.
    void ensure_hrtf_effects_on_main(IPLHRTF runtime_hrtf);
    void ensure_ambisonics_encode_on_main(bool wanted);

  private:
    /// Success when binaural is set; partial results still need release_hrtf_bound_effects.
    HrtfBoundEffects build_hrtf_bound_effects(IPLHRTF hrtf) const;
    AmbisonicsEffects build_ambisonics_effects(IPLHRTF hrtf) const;
    void release_hrtf_bound_effects(HrtfBoundEffects& effects) const;
    void release_ambisonics_effects(AmbisonicsEffects& effects) const;
    /// Swaps the live HRTF-bound effects with effects; the caller owns what comes back.
    void exchange_hrtf_bound_effects(HrtfBoundEffects& effects);
    void exchange_ambisonics_effects(AmbisonicsEffects& effects);
    void swap_in_hrtf_bound_effects_while_audio_idle(HrtfBoundEffects& effects);
    void apply_spatialization(const IPLVector3& dir, const IPLAudioBuffer& direct_out, IPLAudioBuffer& out,
                              bool use_ambi_path, bool use_binaural, bool hrtf_bilinear, float spatial_blend);
    /// End of stream only: drains the binaural and HOA tails via GetTail, never Apply.
    bool apply_spatialization_tail(IPLAudioBuffer& out);
    void copy_binaural_stereo_to_output(IPLAudioBuffer& out);
    /// When stereo binaural effects write into a surround output buffer (no stereo scratch), clear channels 2+.
    void clear_surround_tail_after_direct_stereo_effect(IPLAudioBuffer& out, const IPLAudioBuffer* stereo_effect_destination);
};
} // namespace godot

#endif