#ifndef RESONANCE_PLAYER_DEBUG_HUD_POLICY_H
#define RESONANCE_PLAYER_DEBUG_HUD_POLICY_H

#include "resonance_constants.h"

#include <cstdio>
#include <string>

namespace resonance {

/// Player 3D HUD: Convolution wet level is read from the shared reverb bus RMS (pre-gain), not per-voice mix gain.
inline bool player_debug_hud_use_shared_reverb_bus_rms(bool reflections_debug_enabled, int reflection_type) {
    return reflections_debug_enabled && reflection_type == kReflectionConvolution;
}

/// ResonanceDebugDrawer::process runs when either server debug_occlusion or debug_reflections is on.
inline bool player_debug_drawer_wants_visuals(bool show_occ_line, bool show_player_hud) {
    return show_occ_line || show_player_hud;
}

/// Label3D distance row (meters). Kept ASCII for tests and drawer text.
inline std::string format_player_debug_hud_distance_line(float distance_m) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "Dist: %.2f m\n", distance_m);
    return std::string(buf);
}

} // namespace resonance

#endif // RESONANCE_PLAYER_DEBUG_HUD_POLICY_H
