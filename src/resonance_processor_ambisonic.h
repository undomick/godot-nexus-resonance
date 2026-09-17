#ifndef RESONANCE_PROCESSOR_AMBISONIC_H
#define RESONANCE_PROCESSOR_AMBISONIC_H

#include "resonance_constants.h"
#include <cstddef>
#include <phonon.h>
#include <vector>

namespace godot {

enum class AmbisonicInitFlags : int {
    NONE = 0,
    ROTATION = 1 << 0,
    DECODE = 1 << 1,
    BUFFERS = 1 << 2,
};
inline AmbisonicInitFlags operator|(AmbisonicInitFlags a, AmbisonicInitFlags b) {
    return static_cast<AmbisonicInitFlags>(static_cast<int>(a) | static_cast<int>(b));
}
inline bool operator&(AmbisonicInitFlags a, AmbisonicInitFlags b) { return (static_cast<int>(a) & static_cast<int>(b)) != 0; }

class ResonanceAmbisonicProcessor {
  private:
    IPLContext context = nullptr;
    IPLAmbisonicsRotationEffect rotation_effect = nullptr;
    IPLAmbisonicsDecodeEffect ambisonics_dec_effect = nullptr;

    // Buffers
    IPLAudioBuffer sa_in_buffer{};      // Deinterleaved input (SN3D if input_sn3d, else N3D)
    IPLAudioBuffer sa_n3d_buffer{};     // After SN3D->N3D when input_sn3d
    IPLAudioBuffer sa_rotated_buffer{}; // Rotated Ambisonics (listener-space)

    AmbisonicInitFlags init_flags = AmbisonicInitFlags::NONE;
    int frame_size = resonance::kGodotDefaultFrameSize;
    int sample_rate = 48000;
    int ambisonic_order = 1;
    /// IPL Ambisonics rotation effect (skipped when using precomputed decode orientation from combined matrices).
    bool use_ip_ambisonics_rotation_effect = false;
    bool apply_hrtf = true;
    bool input_is_sn3d = true;
    bool apply_output_gain = true;
    bool last_used_rotation_effect_ = false;

  public:
    ResonanceAmbisonicProcessor() = default;
    ~ResonanceAmbisonicProcessor();

    ResonanceAmbisonicProcessor(const ResonanceAmbisonicProcessor&) = delete;
    ResonanceAmbisonicProcessor& operator=(const ResonanceAmbisonicProcessor&) = delete;
    ResonanceAmbisonicProcessor(ResonanceAmbisonicProcessor&&) = delete;
    ResonanceAmbisonicProcessor& operator=(ResonanceAmbisonicProcessor&&) = delete;

    void initialize(IPLContext p_context, int p_sample_rate, int p_frame_size, int p_order,
                    bool p_use_rotation_effect, bool p_apply_hrtf, bool p_input_is_sn3d, bool p_apply_output_gain,
                    IPLHRTF p_hrtf);
    void cleanup();

    bool matches_config(int p_order, bool p_use_rotation_effect, bool p_apply_hrtf, bool p_input_is_sn3d,
                        bool p_apply_output_gain) const;

    // Process N-channel Ambisonic input (N=(order+1)^2) -> 2-channel output based on orientation.
    /// runtime_hrtf: pass ResonanceServer::get_hrtf_handle() each block when apply_hrtf is enabled (may change after SOFA load).
    /// When combined_matrix_decode: decode_input is N3D HOA in world space; combined_decode_orientation is the decode frame.
    void process(const float* input_data, size_t sample_count, IPLAudioBuffer& out_buffer, bool combined_matrix_decode,
                 const IPLCoordinateSpace3& listener_orient, const IPLCoordinateSpace3& combined_decode_orientation,
                 IPLHRTF runtime_hrtf);
    void process(const std::vector<float>& input_data, IPLAudioBuffer& out_buffer, bool combined_matrix_decode,
                 const IPLCoordinateSpace3& listener_orient, const IPLCoordinateSpace3& combined_decode_orientation,
                 IPLHRTF runtime_hrtf);

    int get_tail_size_samples() const;
    /// EOS: drain rotation (if active) then decode internal tails into stereo out.
    bool process_tail(IPLAudioBuffer& out_buffer);
    void reset_for_new_playback();
};

} // namespace godot

#endif