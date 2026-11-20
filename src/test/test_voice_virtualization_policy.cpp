#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_voice_virtualization_policy.h"

using namespace resonance;

namespace {

VoiceAudibility beyond_max_distance() {
    VoiceAudibility a;
    a.attenuation_uses_max_distance = true;
    a.distance = 80.0f;
    a.max_distance = 30.0f;
    a.attenuation = 0.0f;
    return a;
}

} // namespace

TEST_CASE("Direct-only voice beyond max_distance is inaudible", "[virtualization]") {
    REQUIRE(voice_is_inaudible(beyond_max_distance()));
}

TEST_CASE("Voice inside max_distance stays audible", "[virtualization]") {
    VoiceAudibility a = beyond_max_distance();
    a.distance = 29.0f;
    REQUIRE_FALSE(voice_is_inaudible(a));
}

TEST_CASE("Curve that stays above zero at max_distance keeps the voice audible", "[virtualization]") {
    VoiceAudibility a = beyond_max_distance();
    a.attenuation = 0.05f;
    REQUIRE_FALSE(voice_is_inaudible(a));
}

TEST_CASE("Inverse attenuation never virtualizes", "[virtualization]") {
    VoiceAudibility a = beyond_max_distance();
    a.attenuation_uses_max_distance = false;
    REQUIRE_FALSE(voice_is_inaudible(a));
}

TEST_CASE("Reflections outside the distance curve keep the voice audible", "[virtualization]") {
    VoiceAudibility a = beyond_max_distance();
    a.reflections_simulated = true;
    REQUIRE_FALSE(voice_is_inaudible(a));
    a.reflections_follow_distance_curve = true;
    REQUIRE(voice_is_inaudible(a));
}

TEST_CASE("Enter delay covers the reverb tail", "[virtualization]") {
    REQUIRE(voice_virtual_enter_delay_sec(false, 2.0f) == Approx(kVoiceVirtualEnterDelaySec));
    REQUIRE(voice_virtual_enter_delay_sec(true, 2.0f) == Approx(2.0f));
    REQUIRE(voice_virtual_enter_delay_sec(true, 0.1f) == Approx(kVoiceVirtualEnterDelaySec));
}

TEST_CASE("Voice goes virtual only after the enter delay", "[virtualization]") {
    VoiceVirtualState s;
    s = next_voice_virtual_state(s, true, 0.1, 0.25f);
    REQUIRE_FALSE(s.virtualized);
    s = next_voice_virtual_state(s, true, 0.1, 0.25f);
    REQUIRE_FALSE(s.virtualized);
    s = next_voice_virtual_state(s, true, 0.1, 0.25f);
    REQUIRE(s.virtualized);
}

TEST_CASE("Becoming audible wakes the voice at once and restarts the delay", "[virtualization]") {
    VoiceVirtualState s;
    s = next_voice_virtual_state(s, true, 1.0, 0.25f);
    REQUIRE(s.virtualized);
    s = next_voice_virtual_state(s, false, 0.016, 0.25f);
    REQUIRE_FALSE(s.virtualized);
    REQUIRE(s.inaudible_sec == Approx(0.0));
    s = next_voice_virtual_state(s, true, 0.1, 0.25f);
    REQUIRE_FALSE(s.virtualized);
}

TEST_CASE("Enter delay accumulates over every frame, not over full pushes", "[virtualization]") {
    constexpr double frame_sec = 1.0 / 64.0;
    constexpr int frames_to_virtual = 16;
    VoiceVirtualState s;
    for (int i = 0; i < frames_to_virtual - 1; ++i)
        s = next_voice_virtual_state(s, true, frame_sec, kVoiceVirtualEnterDelaySec);
    REQUIRE_FALSE(s.virtualized);
    s = next_voice_virtual_state(s, true, frame_sec, kVoiceVirtualEnterDelaySec);
    REQUIRE(s.virtualized);
}
