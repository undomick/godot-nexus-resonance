#ifndef RESONANCE_PLAYER_REFLECTIONS_POLICY_H
#define RESONANCE_PLAYER_REFLECTIONS_POLICY_H

namespace resonance {

// ResonancePlayerConfig.reflections_type. Baked reverb belongs to the listener, not to a player.
// The static values keep their 1.1 numbers so saved configs load unchanged.
constexpr int kPlayerReflectionsRealtime = 0;
constexpr int kPlayerReflectionsBakedStaticSource = 2;
constexpr int kPlayerReflectionsBakedStaticListener = 3;

// Server baked_data_variation: -1 ray-traced, 1 baked static source, 2 baked static listener.
inline int player_reflections_baked_data_variation(int reflections_type) {
    if (reflections_type == kPlayerReflectionsBakedStaticSource)
        return 1;
    if (reflections_type == kPlayerReflectionsBakedStaticListener)
        return 2;
    return -1;
}

} // namespace resonance

#endif // RESONANCE_PLAYER_REFLECTIONS_POLICY_H
