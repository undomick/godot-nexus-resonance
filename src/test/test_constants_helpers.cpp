#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_constants.h"
#include <cmath>

using namespace resonance;

TEST_CASE("is_valid_ambisonic_channel_count accepts first three orders", "[constants]") {
    REQUIRE(is_valid_ambisonic_channel_count(4));
    REQUIRE(is_valid_ambisonic_channel_count(9));
    REQUIRE(is_valid_ambisonic_channel_count(16));
}

TEST_CASE("is_valid_ambisonic_channel_count rejects other counts", "[constants]") {
    REQUIRE_FALSE(is_valid_ambisonic_channel_count(0));
    REQUIRE_FALSE(is_valid_ambisonic_channel_count(1));
    REQUIRE_FALSE(is_valid_ambisonic_channel_count(3));
    REQUIRE_FALSE(is_valid_ambisonic_channel_count(8));
    REQUIRE_FALSE(is_valid_ambisonic_channel_count(17));
}

TEST_CASE("ambisonic_num_channels_for_order matches (order+1)^2 for 1..3", "[constants]") {
    REQUIRE(ambisonic_num_channels_for_order(1) == 4);
    REQUIRE(ambisonic_num_channels_for_order(2) == 9);
    REQUIRE(ambisonic_num_channels_for_order(3) == 16);
}

TEST_CASE("ambisonic_num_channels_for_order clamps order to 1..3", "[constants]") {
    REQUIRE(ambisonic_num_channels_for_order(0) == 4);
    REQUIRE(ambisonic_num_channels_for_order(-99) == 4);
    REQUIRE(ambisonic_num_channels_for_order(99) == 16);
}

TEST_CASE("ambisonic and path EQ constants match documented values", "[constants]") {
    REQUIRE(kAmbisonicWChannelScale == Approx(1.0f / std::sqrt(2.0f)));
    REQUIRE(kPathEQCoeffMin == Approx(1e-6f));
    REQUIRE(kPathEQCoeffMax == Approx(1.0f));
}

TEST_CASE("spatial_audio_geometry_gate_allows_output warmup and commit", "[constants]") {
    REQUIRE_FALSE(spatial_audio_geometry_gate_allows_output(1, 100, true));
    REQUIRE_FALSE(spatial_audio_geometry_gate_allows_output(0, 100, false));
    REQUIRE(spatial_audio_geometry_gate_allows_output(0, 0, false));
    REQUIRE(spatial_audio_geometry_gate_allows_output(0, 100, true));
    REQUIRE(kSpatialAudioWarmupWorkerPasses > 0);
}

TEST_CASE("spatial_audio_geometry_notify_should_arm_gate only on empty-to-nonempty", "[constants]") {
    REQUIRE(spatial_audio_geometry_notify_should_arm_gate(0, 10));
    REQUIRE(spatial_audio_geometry_notify_should_arm_gate(-1, 1));
    REQUIRE_FALSE(spatial_audio_geometry_notify_should_arm_gate(10, 20));
    REQUIRE_FALSE(spatial_audio_geometry_notify_should_arm_gate(10, 5));
    REQUIRE_FALSE(spatial_audio_geometry_notify_should_arm_gate(0, 0));
    REQUIRE_FALSE(spatial_audio_geometry_notify_should_arm_gate(5, 0));
}

TEST_CASE("process priority chain listener before player before runtime", "[constants]") {
    REQUIRE(kResonanceListenerProcessPriority > kResonancePlayerProcessPriority);
    REQUIRE(kResonancePlayerProcessPriority > kResonanceRuntimeProcessPriority);
}
