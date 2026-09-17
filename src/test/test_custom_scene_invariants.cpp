#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_constants.h"
#include "../resonance_physics_ray_math.h"
#include "../resonance_transmission_fetch_policy.h"
#include "../resonance_transmission_hit_policy.h"
#include <cmath>

namespace {

// Mirrors Steam Audio direct_effect.cpp: overallGain *= occlusion + (1 - occlusion) * averageTransmissionFactor
float steam_direct_occlusion_transmission_gain(float occlusion, float trans_l, float trans_m, float trans_h) {
    const float t_avg = (trans_l + trans_m + trans_h) / 3.0f;
    return occlusion + (1.0f - occlusion) * t_avg;
}

} // namespace

TEST_CASE("Occlusion fetch default is visible (Steam LOS = 1)", "[custom_scene][occlusion]") {
    REQUIRE(resonance::kOcclusionFetchDefaultVisible == 1.0f);
}

TEST_CASE("Custom scene occlusion ray epsilon matches constant", "[custom_scene][ray]") {
    REQUIRE(resonance::kCustomSceneOcclusionRayStartEpsilon == 1e-2f);
}

TEST_CASE("custom_scene_occlusion_ray_start_t nudges open segment", "[custom_scene][ray]") {
    using resonance::custom_scene_occlusion_ray_start_t;
    const float eps = resonance::kCustomSceneOcclusionRayStartEpsilon;
    REQUIRE(custom_scene_occlusion_ray_start_t(0.0f, 1.0f) == Approx(eps).margin(1e-7f));
    REQUIRE(custom_scene_occlusion_ray_start_t(0.0f, 0.5f * eps) == Approx(0.0f).margin(1e-7f));
    REQUIRE(custom_scene_occlusion_ray_start_t(0.5f, 0.52f) == Approx(0.5f + eps).margin(1e-7f));
    REQUIRE(custom_scene_occlusion_ray_start_t(1.0f, 1.0f) == Approx(1.0f).margin(1e-7f));
    REQUIRE(custom_scene_occlusion_ray_start_t(2.0f, 1.0f) == Approx(2.0f).margin(1e-7f));
}

TEST_CASE("Steam direct gain: full occlusion with low transmission damps strongly", "[custom_scene][steam_ref]") {
    const float g = steam_direct_occlusion_transmission_gain(0.0f, 0.1f, 0.05f, 0.03f);
    REQUIRE(g == Approx((0.1f + 0.05f + 0.03f) / 3.0f).margin(1e-6f));
    REQUIRE(g < 0.11f);
}

TEST_CASE("Steam direct gain: line of sight ignores transmission", "[custom_scene][steam_ref]") {
    const float g = steam_direct_occlusion_transmission_gain(1.0f, 0.1f, 0.05f, 0.03f);
    REQUIRE(g == Approx(1.0f).margin(1e-6f));
}

TEST_CASE("Steam direct gain: partial occlusion blends", "[custom_scene][steam_ref]") {
    const float g = steam_direct_occlusion_transmission_gain(0.5f, 1.0f, 1.0f, 1.0f);
    REQUIRE(g == Approx(1.0f).margin(1e-6f));
}

TEST_CASE("Default numTransmissionRays is nearest surface only", "[custom_scene][transmission]") {
    REQUIRE(resonance::kDefaultTransmissionRays == 1);
    REQUIRE(resonance::kDefaultPlayerConfigTransmissionRays == 1);
}

// Mirrors Steam DirectSimulator::transmission: product of material T; sqrt when numHits > 1.
TEST_CASE("Transmission product: one hit keeps material T", "[custom_scene][transmission]") {
    const float t = 0.1f;
    REQUIRE(t == Approx(0.1f).margin(1e-6f));
    const float gain = steam_direct_occlusion_transmission_gain(0.0f, t, t, t);
    REQUIRE(gain == Approx(0.1f).margin(1e-6f));
    REQUIRE(gain > 0.05f);
}

TEST_CASE("Transmission product: many soft hits silence via sqrt(product)", "[custom_scene][transmission]") {
    float product = 1.0f;
    constexpr float per_hit = 0.1f;
    for (int i = 0; i < 16; ++i)
        product *= per_hit;
    const float factors = std::sqrt(product);
    REQUIRE(factors < 0.005f);
    const float gain = steam_direct_occlusion_transmission_gain(0.0f, factors, factors, factors);
    REQUIRE(gain < 0.005f);
}

TEST_CASE("LOS clears plastic-like transmission to 1,1,1", "[custom_scene][transmission]") {
    float tx[3] = {0.04f, 0.025f, 0.015f};
    resonance::clear_transmission_on_line_of_sight(1.0f, tx);
    REQUIRE(tx[0] == Approx(1.0f).margin(1e-7f));
    REQUIRE(tx[1] == Approx(1.0f).margin(1e-7f));
    REQUIRE(tx[2] == Approx(1.0f).margin(1e-7f));
}

TEST_CASE("Occluded transmission keeps material coefficients", "[custom_scene][transmission]") {
    float tx[3] = {0.04f, 0.025f, 0.015f};
    resonance::clear_transmission_on_line_of_sight(0.0f, tx);
    REQUIRE(tx[0] == Approx(0.04f).margin(1e-7f));
    REQUIRE(tx[1] == Approx(0.025f).margin(1e-7f));
    REQUIRE(tx[2] == Approx(0.015f).margin(1e-7f));
}

TEST_CASE("Partial occlusion keeps transmission for Direct Effect blend", "[custom_scene][transmission]") {
    float tx[3] = {0.779f, 0.852f, 0.879f};
    resonance::clear_transmission_on_line_of_sight(0.5f, tx);
    REQUIRE(tx[0] == Approx(0.779f).margin(1e-7f));
    REQUIRE(tx[1] == Approx(0.852f).margin(1e-7f));
    REQUIRE(tx[2] == Approx(0.879f).margin(1e-7f));
}

TEST_CASE("Steam transmission: one brick hit keeps brick T", "[custom_scene][transmission]") {
    const float brick[3] = {resonance::kBrickTransmissionLow, resonance::kBrickTransmissionMid,
                            resonance::kBrickTransmissionHigh};
    const float* hits[] = {brick};
    float out[3] = {};
    resonance::steam_transmission_from_hits(hits, 1, out);
    REQUIRE(out[0] == Approx(resonance::kBrickTransmissionLow).margin(1e-6f));
    REQUIRE(out[1] == Approx(resonance::kBrickTransmissionMid).margin(1e-6f));
    REQUIRE(out[2] == Approx(resonance::kBrickTransmissionHigh).margin(1e-6f));
    REQUIRE(resonance::classify_transmission_material(out[0], out[1], out[2]) ==
            resonance::TransmissionMaterialLabel::Brick);
}

TEST_CASE("Steam transmission: two brick hits sqrt keep brick T", "[custom_scene][transmission]") {
    const float brick[3] = {resonance::kBrickTransmissionLow, resonance::kBrickTransmissionMid,
                            resonance::kBrickTransmissionHigh};
    const float* hits[] = {brick, brick};
    float out[3] = {};
    resonance::steam_transmission_from_hits(hits, 2, out);
    REQUIRE(out[0] == Approx(resonance::kBrickTransmissionLow).margin(1e-5f));
    REQUIRE(out[1] == Approx(resonance::kBrickTransmissionMid).margin(1e-5f));
    REQUIRE(out[2] == Approx(resonance::kBrickTransmissionHigh).margin(1e-5f));
    REQUIRE(resonance::classify_transmission_material(out[0], out[1], out[2]) ==
            resonance::TransmissionMaterialLabel::Brick);
}

TEST_CASE("Steam transmission: one default-export hit keeps 0.1", "[custom_scene][transmission]") {
    const float def[3] = {resonance::kDefaultExportTransmission, resonance::kDefaultExportTransmission,
                          resonance::kDefaultExportTransmission};
    const float* hits[] = {def};
    float out[3] = {};
    resonance::steam_transmission_from_hits(hits, 1, out);
    REQUIRE(out[0] == Approx(0.1f).margin(1e-6f));
    REQUIRE(out[1] == Approx(0.1f).margin(1e-6f));
    REQUIRE(out[2] == Approx(0.1f).margin(1e-6f));
    REQUIRE(resonance::classify_transmission_material(out[0], out[1], out[2]) ==
            resonance::TransmissionMaterialLabel::DefaultExport);
}

TEST_CASE("Hit0 fill only when numTransmissionRays is 1", "[custom_scene][transmission]") {
    const float steam[3] = {0.03f, 0.02f, 0.008f};
    float hit0[3] = {};
    bool valid = false;
    resonance::fill_hit0_from_single_transmission_ray(1, steam, hit0, &valid);
    REQUIRE(valid);
    REQUIRE(hit0[0] == Approx(0.03f).margin(1e-7f));
    REQUIRE(resonance::classify_transmission_material(hit0[0], hit0[1], hit0[2]) ==
            resonance::TransmissionMaterialLabel::Other);

    valid = true;
    resonance::fill_hit0_from_single_transmission_ray(2, steam, hit0, &valid);
    REQUIRE_FALSE(valid);
}
