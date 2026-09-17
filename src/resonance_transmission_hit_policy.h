#ifndef RESONANCE_TRANSMISSION_HIT_POLICY_H
#define RESONANCE_TRANSMISSION_HIT_POLICY_H

#include <cmath>
#include <cstdint>
#include <cstring>

namespace resonance {

/// Built-in brick.tres transmission (addons/nexus_resonance/materials/brick.tres).
constexpr float kBrickTransmissionLow = 0.5f;
constexpr float kBrickTransmissionMid = 0.1f;
constexpr float kBrickTransmissionHigh = 0.028f;
/// Built-in wood.tres (high often unset; treat missing as 0.01 for classification).
constexpr float kWoodTransmissionLow = 0.06f;
constexpr float kWoodTransmissionMid = 0.03f;
constexpr float kWoodTransmissionHigh = 0.01f;
/// Built-in plaster.tres.
constexpr float kPlasterTransmissionLow = 0.04f;
constexpr float kPlasterTransmissionMid = 0.03f;
constexpr float kPlasterTransmissionHigh = 0.01f;
/// Scene-export / ResonanceGeometry fallback (kSceneExportTransmission).
constexpr float kDefaultExportTransmission = 0.1f;
/// Phonon / Custom physics "generic" preset.
constexpr float kGenericTransmissionLow = 0.100f;
constexpr float kGenericTransmissionMid = 0.050f;
constexpr float kGenericTransmissionHigh = 0.030f;

constexpr float kTransmissionMatchEpsilon = 0.002f;

enum class TransmissionMaterialLabel : int {
    None = 0,
    Brick = 1,
    DefaultExport = 2,
    Generic = 3,
    Wood = 4,
    Plaster = 5,
    Other = 6,
};

inline bool transmission_bands_match(float a0, float a1, float a2, float b0, float b1, float b2, float eps) {
    return std::fabs(a0 - b0) <= eps && std::fabs(a1 - b1) <= eps && std::fabs(a2 - b2) <= eps;
}

/// Classify 3-band transmission against known presets (Hit0 / Steam T diagnosis).
inline TransmissionMaterialLabel classify_transmission_material(float t0, float t1, float t2) {
    if (transmission_bands_match(t0, t1, t2, kBrickTransmissionLow, kBrickTransmissionMid, kBrickTransmissionHigh,
                                 kTransmissionMatchEpsilon))
        return TransmissionMaterialLabel::Brick;
    if (transmission_bands_match(t0, t1, t2, kWoodTransmissionLow, kWoodTransmissionMid, kWoodTransmissionHigh,
                                 kTransmissionMatchEpsilon))
        return TransmissionMaterialLabel::Wood;
    if (transmission_bands_match(t0, t1, t2, kPlasterTransmissionLow, kPlasterTransmissionMid, kPlasterTransmissionHigh,
                                 kTransmissionMatchEpsilon))
        return TransmissionMaterialLabel::Plaster;
    if (transmission_bands_match(t0, t1, t2, kDefaultExportTransmission, kDefaultExportTransmission,
                                 kDefaultExportTransmission, kTransmissionMatchEpsilon))
        return TransmissionMaterialLabel::DefaultExport;
    if (transmission_bands_match(t0, t1, t2, kGenericTransmissionLow, kGenericTransmissionMid, kGenericTransmissionHigh,
                                 kTransmissionMatchEpsilon))
        return TransmissionMaterialLabel::Generic;
    return TransmissionMaterialLabel::Other;
}

inline const char* transmission_material_label_cstr(TransmissionMaterialLabel label) {
    switch (label) {
    case TransmissionMaterialLabel::Brick:
        return "brick";
    case TransmissionMaterialLabel::Wood:
        return "wood";
    case TransmissionMaterialLabel::Plaster:
        return "plaster";
    case TransmissionMaterialLabel::DefaultExport:
        return "default";
    case TransmissionMaterialLabel::Generic:
        return "generic";
    case TransmissionMaterialLabel::Other:
        return "other";
    case TransmissionMaterialLabel::None:
    default:
        return "none";
    }
}

/// Mirrors Steam DirectSimulator::transmission: product of hit materials; sqrt when numHits > 1.
inline void steam_transmission_from_hits(const float* const* hit_transmission_lmh, int num_hits, float* out_lmh) {
    if (!out_lmh)
        return;
    out_lmh[0] = out_lmh[1] = out_lmh[2] = 1.0f;
    if (!hit_transmission_lmh || num_hits <= 0)
        return;
    for (int i = 0; i < num_hits; ++i) {
        const float* t = hit_transmission_lmh[i];
        if (!t)
            continue;
        out_lmh[0] *= t[0];
        out_lmh[1] *= t[1];
        out_lmh[2] *= t[2];
    }
    if (num_hits <= 1)
        return;
    out_lmh[0] = std::sqrt(out_lmh[0]);
    out_lmh[1] = std::sqrt(out_lmh[1]);
    out_lmh[2] = std::sqrt(out_lmh[2]);
}

/// First occurrence of each geometry id wins. Returns unique count (<= out_cap).
inline int unique_transmission_hits_by_id(const int64_t* ids, const float* const* hit_transmission_lmh, int in_count,
                                          const float** out_hits, int64_t* out_ids, int out_cap) {
    if (!out_hits || !out_ids || out_cap <= 0)
        return 0;
    if (!ids || !hit_transmission_lmh || in_count <= 0)
        return 0;
    int out_n = 0;
    for (int i = 0; i < in_count; ++i) {
        const int64_t id = ids[i];
        bool seen = false;
        for (int j = 0; j < out_n; ++j) {
            if (out_ids[j] == id) {
                seen = true;
                break;
            }
        }
        if (seen)
            continue;
        out_hits[out_n] = hit_transmission_lmh[i];
        out_ids[out_n] = id;
        out_n++;
        if (out_n >= out_cap)
            break;
    }
    return out_n;
}

/// Dedupe by geometry id, then Steam product+sqrt.
inline void steam_transmission_from_unique_ids(const int64_t* ids, const float* const* hit_transmission_lmh, int in_count,
                                               float* out_lmh) {
    if (!out_lmh)
        return;
    out_lmh[0] = out_lmh[1] = out_lmh[2] = 1.0f;
    if (!ids || !hit_transmission_lmh || in_count <= 0)
        return;
    const float* unique_hits[16];
    int64_t unique_ids[16];
    const int n =
        unique_transmission_hits_by_id(ids, hit_transmission_lmh, in_count, unique_hits, unique_ids, 16);
    steam_transmission_from_hits(unique_hits, n, out_lmh);
}

/// With numTransmissionRays == 1, Steam's reported T is the single closestHit material (before LOS clear).
inline void fill_hit0_from_single_transmission_ray(int num_transmission_rays, const float* steam_transmission_lmh,
                                                   float* out_hit0_lmh, bool* out_hit0_valid) {
    if (out_hit0_valid)
        *out_hit0_valid = false;
    if (!out_hit0_lmh)
        return;
    out_hit0_lmh[0] = out_hit0_lmh[1] = out_hit0_lmh[2] = 1.0f;
    if (!steam_transmission_lmh || num_transmission_rays != 1)
        return;
    std::memcpy(out_hit0_lmh, steam_transmission_lmh, 3 * sizeof(float));
    if (out_hit0_valid)
        *out_hit0_valid = true;
}

} // namespace resonance

#endif // RESONANCE_TRANSMISSION_HIT_POLICY_H
