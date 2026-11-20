#ifndef RESONANCE_REFLECTION_EFFECT_RECIPE_H
#define RESONANCE_REFLECTION_EFFECT_RECIPE_H

#include "resonance_constants.h"
#include <phonon.h>

namespace godot {

/// Settings for iplReflectionEffectCreate. Two effects built from matching recipes are interchangeable.
struct ReflectionEffectRecipe {
    IPLAudioSettings audio{};
    IPLReflectionEffectSettings settings{};
    float ir_duration_sec = resonance::kDefaultReverbDurationSec;
};

inline bool reflection_recipes_match(const ReflectionEffectRecipe& a, const ReflectionEffectRecipe& b) {
    return a.audio.samplingRate == b.audio.samplingRate && a.audio.frameSize == b.audio.frameSize &&
           a.settings.type == b.settings.type && a.settings.irSize == b.settings.irSize &&
           a.settings.numChannels == b.settings.numChannels;
}

} // namespace godot

#endif // RESONANCE_REFLECTION_EFFECT_RECIPE_H
