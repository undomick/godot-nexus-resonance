#ifndef RESONANCE_AMBISONIC_INPUT_CHUNK_POLICY_H
#define RESONANCE_AMBISONIC_INPUT_CHUNK_POLICY_H

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace resonance {

/// Whole interleaved frames that fit in free_floats, capped at frames_left. 0 means stop: no full frame fits.
inline int32_t ambisonic_input_chunk_frames(size_t free_floats, int num_channels, int32_t frames_left) {
    if (num_channels <= 0 || frames_left <= 0)
        return 0;
    const size_t fitting_frames = free_floats / static_cast<size_t>(num_channels);
    return static_cast<int32_t>(std::min(fitting_frames, static_cast<size_t>(frames_left)));
}

} // namespace resonance

#endif // RESONANCE_AMBISONIC_INPUT_CHUNK_POLICY_H
