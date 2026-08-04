#ifndef RESONANCE_MIXER_PROCESSOR_H
#define RESONANCE_MIXER_PROCESSOR_H

#include "resonance_constants.h"
#include <cstdint>
#include <godot_cpp/classes/audio_frame.hpp>
#include <limits>
#include <phonon.h>
#include <vector>

namespace godot {

enum class MixerInitFlags : int {
    NONE = 0,
    BUFFERS = 1 << 0,
    DECODEEFFECT = 1 << 1,
    DECODEEFFECT_7_1 = 1 << 2,
    VIRTUALSURROUND = 1 << 3,
};
inline MixerInitFlags operator|(MixerInitFlags a, MixerInitFlags b) {
    return static_cast<MixerInitFlags>(static_cast<int>(a) | static_cast<int>(b));
}
inline MixerInitFlags& operator|=(MixerInitFlags& a, MixerInitFlags b) {
    a = a | b;
    return a;
}
inline bool operator&(MixerInitFlags a, MixerInitFlags b) { return (static_cast<int>(a) & static_cast<int>(b)) != 0; }

/// Reflection mixer HOA → stereo for the reverb bus; optional Virtual Surround decode chain.
class ResonanceMixerProcessor {
  private:
    IPLContext context = nullptr;
    IPLAmbisonicsDecodeEffect decode_effect = nullptr;
    // Optional: HOA → 7.1 → iplVirtualSurroundEffect → stereo
    IPLAmbisonicsDecodeEffect decode_effect_7_1 = nullptr;
    IPLVirtualSurroundEffect virtual_surround_effect = nullptr;

    IPLAudioBuffer sa_ambisonic_buffer{}; // `Apply` output, decoded same callback
    IPLAudioBuffer sa_stereo_buffer{};    // Decode -> Godot (or VirtualSurround -> Godot)
    IPLAudioBuffer sa_7_1_buffer{};       // Decode 7.1 intermediate when use_virtual_surround
    std::vector<float> last_stereo_left{};
    std::vector<float> last_stereo_right{};
    bool last_stereo_valid = false;
    uint64_t last_seen_mixer_feed_count_ = 0;
    bool have_seen_mixer_feed_count_ = false;
    /// When equal to `get_mixer_feed_count()`, hold-last is suppressed (one real Apply per stale plateau).
    uint64_t hold_last_suppression_feed_ = std::numeric_limits<uint64_t>::max();
    std::vector<float> pending_stereo_left{};
    std::vector<float> pending_stereo_right{};
    size_t pending_read_index = 0;
    size_t pending_len_ = 0; // Valid samples in pending_* (fixed capacity; no hotpath resize)

    void _write_stereo_to_audio_frames_with_carry(AudioFrame* out_frames, int frame_count);
    void _decode_ambisonic_to_stereo_buffer(IPLAudioBuffer* ambi_in, const IPLCoordinateSpace3& listener_coords);
    void _cache_last_stereo_block();
    bool _restore_last_stereo_block();
    bool _can_decode() const;

    MixerInitFlags init_flags = MixerInitFlags::NONE;
    int frame_size = resonance::kGodotDefaultFrameSize;
    int ambisonic_order = 1;

  public:
    ResonanceMixerProcessor() = default;
    ~ResonanceMixerProcessor();

    ResonanceMixerProcessor(const ResonanceMixerProcessor&) = delete;
    ResonanceMixerProcessor& operator=(const ResonanceMixerProcessor&) = delete;
    ResonanceMixerProcessor(ResonanceMixerProcessor&&) = delete;
    ResonanceMixerProcessor& operator=(ResonanceMixerProcessor&&) = delete;

    void initialize(IPLContext p_context, int p_sample_rate, int p_frame_size, int p_ambisonic_order);
    void cleanup();

    /// True if initialization succeeded and decode path is usable (for AudioEffect to avoid processing with invalid state).
    bool is_ready() const { return _can_decode(); }

    bool process_mixer_return(IPLReflectionMixer mixer_handle, const IPLCoordinateSpace3& listener_coords, AudioFrame* out_frames, int frame_count);

    /// Decode caller-owned HOA (no `iplReflectionMixerApply`; e.g. player convolution tap).
    bool decode_ambisonic_to_stereo(IPLAudioBuffer* ambi_buf, const IPLCoordinateSpace3& listener_coords, AudioFrame* out_frames, int frame_count);
};

} // namespace godot

#endif