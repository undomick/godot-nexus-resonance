#ifndef RESONANCE_PROCESSOR_PATH_H
#define RESONANCE_PROCESSOR_PATH_H

#include "resonance_constants.h"
#include <phonon.h>

namespace godot {

enum class PathInitFlags : int {
    NONE = 0,
    PATHEFFECT = 1 << 0,
    BUFFERS = 1 << 1,
};
inline PathInitFlags operator|(PathInitFlags a, PathInitFlags b) {
    return static_cast<PathInitFlags>(static_cast<int>(a) | static_cast<int>(b));
}
inline bool operator&(PathInitFlags a, PathInitFlags b) {
    return (static_cast<int>(a) & static_cast<int>(b)) != 0;
}

class ResonancePathProcessor {
  private:
    IPLContext context = nullptr;
    IPLPathEffect path_effect = nullptr;
    IPLAudioBuffer internal_mono_buffer{};

    PathInitFlags init_flags = PathInitFlags::NONE;
    int frame_size = resonance::kGodotDefaultFrameSize;
    int sample_rate = 48000;
    int ambisonic_order = 1;
    IPLHRTF bound_hrtf_ = nullptr;

  public:
    ResonancePathProcessor() = default;
    ~ResonancePathProcessor();

    ResonancePathProcessor(const ResonancePathProcessor&) = delete;
    ResonancePathProcessor& operator=(const ResonancePathProcessor&) = delete;
    ResonancePathProcessor(ResonancePathProcessor&&) = delete;
    ResonancePathProcessor& operator=(ResonancePathProcessor&&) = delete;

    void initialize(IPLContext p_context, int p_sample_rate, int p_frame_size, int p_ambisonic_order);
    void cleanup();

    /// Applies path_mix ramp on mono input after downmix, then path effect.
    void process(const IPLAudioBuffer& in_buffer, const IPLPathEffectParams& params, IPLAudioBuffer& out_buffer,
                 float path_mix_ramp_start, float path_mix_ramp_end);

    /// One frame of path tail (stereo spatialize out). Call while get_tail_size_samples() > 0.
    bool process_tail(IPLAudioBuffer& out_stereo);
    int get_tail_size_samples() const;
    void reset_effect();

    /// Main thread: recreate path effect when create-time HRTF identity no longer matches runtime.
    void ensure_hrtf_on_main(IPLHRTF runtime_hrtf);
    bool hrtf_needs_main_sync(IPLHRTF runtime_hrtf) const;

  private:
    bool create_path_effect(IPLHRTF hrtf);
};

} // namespace godot

#endif
