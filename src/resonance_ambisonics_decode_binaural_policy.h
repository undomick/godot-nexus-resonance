#ifndef RESONANCE_AMBISONICS_DECODE_BINAURAL_POLICY_H
#define RESONANCE_AMBISONICS_DECODE_BINAURAL_POLICY_H

#include <phonon.h>

namespace resonance {

/// Tri-state override (-1 use global, 0 off, 1 on) for per-source reverb/pathing binaural.
inline bool resolve_binaural_override(int tri_state, bool global_on) {
    if (tri_state == -1)
        return global_on;
    return tri_state == 1;
}

/// Virtual-surround reverb decode always applies HRTF in the VS step; skip that chain when binaural is off.
inline bool ambisonics_decode_use_virtual_surround_chain(bool server_use_virtual_surround, bool apply_binaural) {
    return server_use_virtual_surround && apply_binaural;
}

inline void ambisonics_decode_effect_hrtf(bool apply_binaural, IPLHRTF hrtf_handle, IPLHRTF& out_hrtf, IPLbool& out_binaural) {
    if (apply_binaural && hrtf_handle) {
        out_hrtf = hrtf_handle;
        out_binaural = IPL_TRUE;
    } else {
        out_hrtf = nullptr;
        out_binaural = IPL_FALSE;
    }
}

} // namespace resonance

#endif
