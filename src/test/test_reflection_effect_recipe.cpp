#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_reflection_effect_recipe.h"

using namespace godot;

namespace {

ReflectionEffectRecipe convolution_recipe(IPLint32 ir_size) {
    ReflectionEffectRecipe r;
    r.audio.samplingRate = 48000;
    r.audio.frameSize = 512;
    r.settings.type = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
    r.settings.irSize = ir_size;
    r.settings.numChannels = 4;
    return r;
}

} // namespace

TEST_CASE("reflection recipes with equal settings match", "[reflection_recipe]") {
    REQUIRE(reflection_recipes_match(convolution_recipe(96000), convolution_recipe(96000)));
}

TEST_CASE("reflection recipes differ by IR size, channels, type or audio settings", "[reflection_recipe]") {
    const ReflectionEffectRecipe base = convolution_recipe(96000);
    REQUIRE_FALSE(reflection_recipes_match(base, convolution_recipe(48000)));

    ReflectionEffectRecipe other = base;
    other.settings.numChannels = 9;
    REQUIRE_FALSE(reflection_recipes_match(base, other));

    other = base;
    other.settings.type = IPL_REFLECTIONEFFECTTYPE_HYBRID;
    REQUIRE_FALSE(reflection_recipes_match(base, other));

    other = base;
    other.audio.frameSize = 1024;
    REQUIRE_FALSE(reflection_recipes_match(base, other));

    other = base;
    other.audio.samplingRate = 44100;
    REQUIRE_FALSE(reflection_recipes_match(base, other));
}
