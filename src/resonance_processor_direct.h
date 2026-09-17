#ifndef RESONANCE_PROCESSOR_DIRECT_H
#define RESONANCE_PROCESSOR_DIRECT_H

#include "resonance_constants.h"
#include <phonon.h>

namespace godot {

enum class DirectInitFlags : int {
    NONE = 0,
    DIRECT_EFFECT = 1 << 0,
    BINAURAL_EFFECT = 1 << 1,
    PANNING_EFFECT = 1 << 2,
    BUFFERS = 1 << 3,
    AMBISONICS_ENCODE = 1 << 4,
    AMBISONICS_PANNING = 1 << 5,
    BINAURAL_STEREO_SCRATCH = 1 << 6,
    HOA_BINAURAL_STEREO_SCRATCH = 1 << 7,
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
    IPLContext context = nullptr;
    IPLHRTF hrtf_handle = nullptr;

    // Effects chain
    IPLDirectEffect direct_effect = nullptr;
    IPLBinauralEffect binaural_effect = nullptr;
    IPLPanningEffect panning_effect = nullptr;
    IPLAmbisonicsEncodeEffect ambisonics_encode_effect = nullptr;
    IPLAmbisonicsBinauralEffect ambisonics_binaural_effect = nullptr;
    IPLAmbisonicsPanningEffect ambisonics_panning_effect = nullptr;

    // Intermediate buffers
    IPLAudioBuffer internal_mono_buffer{};   // Downmix input for direct effect
    IPLAudioBuffer internal_direct_output{}; // Output of iplDirectEffectApply (separate from input per Steam Audio ref)
    IPLAudioBuffer internal_ambi_buffer{};
    /// Binaural effects write stereo here first; copied to FL/FR when output layout has >2 channels.
    IPLAudioBuffer internal_binaural_stereo_out{};
    /// HOA encode+binaural stereo before blending with standard `iplBinauralEffectApply` (spatial_blend).
    IPLAudioBuffer internal_hoa_stereo_scratch{};

    // Stored for tail processing when source stops
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

    /// Remaining EOS tail samples across direct/binaural/HOA spatial stages (0 when inactive).
    int get_tail_size_samples() const;

    /// Get tail samples when input has stopped. Returns true if tail was output, false if no tail remaining.
    /// Output is stereo (spatialized with last direction). Call repeatedly until returns false.
    bool process_tail(IPLAudioBuffer& out_buffer);

    /// Reset internal state (e.g. when starting new playback). Clears any pending tail.
    void reset_for_new_playback();

    /// Main thread: create or recreate HRTF-bound effects when HRTF arrives or identity changes.
    void ensure_hrtf_effects_on_main(IPLHRTF runtime_hrtf);
    bool hrtf_effects_need_main_sync(IPLHRTF runtime_hrtf) const;

  private:
    void release_hrtf_bound_effects();
    bool create_hrtf_bound_effects(IPLHRTF hrtf);
    void apply_spatialization(const IPLVector3& dir, const IPLAudioBuffer& direct_out, IPLAudioBuffer& out,
                              bool use_ambi_path, bool use_binaural, bool hrtf_bilinear, float spatial_blend);
    /// EOS only: drain binaural/HOA internal tails via GetTail (never Apply).
    bool apply_spatialization_tail(IPLAudioBuffer& out);
    void copy_binaural_stereo_to_output(IPLAudioBuffer& out);
    /// When stereo binaural effects write into a surround output buffer (no stereo scratch), clear channels 2+.
    void clear_surround_tail_after_direct_stereo_effect(IPLAudioBuffer& out, const IPLAudioBuffer* stereo_effect_destination);
};
} // namespace godot

#endif