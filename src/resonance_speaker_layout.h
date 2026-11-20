#ifndef RESONANCE_SPEAKER_LAYOUT_H
#define RESONANCE_SPEAKER_LAYOUT_H

#include <phonon.h>

namespace resonance {

inline IPLSpeakerLayout speaker_layout_for_channel_count(int num_channels) {
    IPLSpeakerLayout layout{};
    layout.numSpeakers = num_channels;
    layout.speakers = nullptr;

    if (num_channels == 1) {
        layout.type = IPL_SPEAKERLAYOUTTYPE_MONO;
    } else if (num_channels == 2) {
        layout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
    } else if (num_channels == 4) {
        layout.type = IPL_SPEAKERLAYOUTTYPE_QUADRAPHONIC;
    } else if (num_channels == 6) {
        layout.type = IPL_SPEAKERLAYOUTTYPE_SURROUND_5_1;
    } else if (num_channels == 8) {
        layout.type = IPL_SPEAKERLAYOUTTYPE_SURROUND_7_1;
    } else {
        layout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
        layout.numSpeakers = 2;
    }
    return layout;
}

inline int clamp_direct_speaker_channels(int n) {
    if (n == 1 || n == 2 || n == 4 || n == 6 || n == 8)
        return n;
    return 2;
}

} // namespace resonance

#endif
