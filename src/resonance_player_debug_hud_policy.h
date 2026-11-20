#ifndef RESONANCE_PLAYER_DEBUG_HUD_POLICY_H
#define RESONANCE_PLAYER_DEBUG_HUD_POLICY_H

#include "resonance_constants.h"

#include <cstdio>
#include <string>

namespace resonance {

/// With convolution, the HUD reverb level comes from the shared reverb bus RMS (pre-gain), not per-voice mix gain.
inline bool player_debug_hud_use_shared_reverb_bus_rms(bool reflections_debug_enabled, int reflection_type) {
    return reflections_debug_enabled && reflection_type == kReflectionConvolution;
}

inline bool player_debug_drawer_wants_visuals(bool show_occ_line, bool show_player_hud) {
    return show_occ_line || show_player_hud;
}

inline std::string format_player_debug_hud_distance_line(float distance_m) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "Dist: %.2f m\n", distance_m);
    return std::string(buf);
}

} // namespace resonance

#endif // RESONANCE_PLAYER_DEBUG_HUD_POLICY_H
