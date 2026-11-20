#ifndef RESONANCE_CONSTANTS_H
#define RESONANCE_CONSTANTS_H

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace resonance {

/// Override with -DNEXUS_RESONANCE_VERSION at build time.
#ifndef NEXUS_RESONANCE_VERSION
#define NEXUS_RESONANCE_VERSION "1.2.0"
#endif
constexpr const char* kVersion = NEXUS_RESONANCE_VERSION;

constexpr int kGodotDefaultFrameSize = 512;
/// Largest supported mix frame; per-player scratch buffers are sized to it.
constexpr int kMaxAudioFrameSize = 2048;

/// Samples per channel.
constexpr int kRingBufferCapacity = 8192;
// Wait before a playback retries its IPL init after a buffer allocation failure.
constexpr uint64_t kPrewarmRetryIntervalMsec = 1000;
/// Crossfade at the seam between two mix_audio ring chunks.
constexpr int kInputRingChunkCrossfadeSamples = 48;
/// Caps the underrun tail fade so a held last sample does not become a long DC step.
constexpr int kSyntheticEosOutputFadeMaxSamples = 128;
/// Cosine taper on the last decoded samples, clamped to the last chunk length. 100 ms at 48 kHz.
constexpr int kEosInputEndTaperMaxSamples = 4800;
constexpr float kPlaybackHostFadeInMs = 8.7f;   // ~384 samples @ 44.1 kHz
constexpr float kPlaybackHostFadeOutMs = 17.4f; // ~768 samples @ 44.1 kHz
/// Extra host ramps on start and stop, on top of the EOS tapers.
constexpr bool kPlaybackHostFadeOutEnabled = false;
constexpr bool kPlaybackHostFadeInEnabled = false;

inline int host_fade_samples_from_ms(float ms, int sample_rate) {
    const int sr = (sample_rate > 0) ? sample_rate : 44100;
    const int n = static_cast<int>(std::llround(static_cast<double>(ms) * 0.001 * static_cast<double>(sr)));
    return (n < 1) ? 1 : n;
}
/// Godot runs lower process priorities first. Positive values run after user nodes at the default 0 have moved
/// cameras and sources; then listener pose -> players enqueue source updates -> runtime flushes and ticks.
constexpr int kResonanceListenerProcessPriority = 100;
constexpr int kResonancePlayerProcessPriority = 110;
constexpr int kResonanceRuntimeProcessPriority = 120;

/// True when the scene gets its first triangles.
inline bool spatial_audio_geometry_notify_should_arm_gate(int triangle_count_before, int triangle_count_after) {
    return triangle_count_before <= 0 && triangle_count_after > 0;
}

/// Input rings are not consumed while the geometry gate is closed.
inline bool spatial_audio_geometry_gate_should_hold_decode(int source_handle, bool output_ready) {
    return source_handle >= 0 && !output_ready;
}

/// irSize = sample_rate * duration.
constexpr float kDefaultReverbDurationSec = 2.0f;
constexpr int kConvolutionIrSamplesHardMax = 480000;
constexpr int kReflectionIrChannelsHardMax = 64;

constexpr float kBakerSimulatedDuration = 2.0f;
constexpr float kBakerIrradianceMinDistance = 0.5f;
constexpr int kBakerNumDiffuseSamples = 32;
constexpr float kBakerMinSpacing = 0.1f;
/// Static endpoint bake identifiers only, not probe grid spheres.
constexpr float kBakerStaticEndpointSphereRadius = 1.0f;
/// Used when a static endpoint bake has influence_radius <= 0.
constexpr float kBakerStaticEndpointInfluenceFallback = 10.0f;

constexpr const char* kProjectSettingsResonancePrefix = "nexus/resonance/";
/// The legacy key is read when the current one is unset.
constexpr const char* kProjectSettingsBakeDefaultOutputDirectory = "bake/default_output_directory";
constexpr const char* kProjectSettingsBakeOutputDirectoryLegacy = "bake/output_dir";
/// 0 = .tres, 1 = .res.
constexpr const char* kProjectSettingsProbeDataFormat = "export/probe_data_format";
/// Directories scanned for ResonanceMaterial files; the file basename is the preset name.
constexpr const char* kProjectSettingsPhysicsMaterialSearchPaths = "physics/material_search_paths";
constexpr const char* kDefaultPhysicsMaterialSearchPath = "res://addons/nexus_resonance/materials";
/// Player resource pool: ready sources and reflection effects to keep built, and the most kept after returns.
constexpr const char* kProjectSettingsPoolMinReady = "pool/min_ready";
constexpr const char* kProjectSettingsPoolMaxKept = "pool/max_kept";
constexpr int kDefaultPoolMinReady = 2;
constexpr int kDefaultPoolMaxKept = 8;

constexpr int kBakeDefaultNumRays = 4096;
constexpr int kBakeDefaultNumBounces = 4;
constexpr int kBakeDefaultNumThreads = 2;
constexpr int kBakeAmbisonicsOrderMin = 1;
constexpr int kBakeAmbisonicsOrderMax = 3;
constexpr int kBakeDefaultAmbisonicsOrder = 1;

/// Matches the ResonanceRuntimeConfig.realtime_rays default.
constexpr int kDefaultRealtimeRays = 0;
/// Runtime endpoint radius for STATICSOURCE/STATICLISTENER lookups whose bake sets none.
constexpr float kBakedEndpointRadius = 10000.0f;

/// Probe neighborhood for reading back STATICSOURCE baked energy.
constexpr float kStaticSourceProbeNeighborRadiusM = 4.0f;

inline int clamp_bake_ambisonics_order(int order) {
    if (order < kBakeAmbisonicsOrderMin)
        return kBakeAmbisonicsOrderMin;
    if (order > kBakeAmbisonicsOrderMax)
        return kBakeAmbisonicsOrderMax;
    return order;
}

/// Legacy probe data stores -1.
inline int effective_baked_ambisonics_order(int stored_order) {
    if (stored_order < 0)
        return kBakeDefaultAmbisonicsOrder;
    return clamp_bake_ambisonics_order(stored_order);
}

/// bake_config_setting <= 0 means Use Global.
inline int resolve_volume_bake_ambisonic_order(int bake_config_setting, int global_order) {
    if (bake_config_setting <= 0)
        return clamp_bake_ambisonics_order(global_order);
    return clamp_bake_ambisonics_order(bake_config_setting);
}

/// The lower of baked and runtime order, so playback never invents HOA channels.
inline int effective_ambisonics_order(int stored_baked_order, int runtime_order) {
    const int baked = effective_baked_ambisonics_order(stored_baked_order);
    const int runtime = clamp_bake_ambisonics_order(runtime_order);
    return baked < runtime ? baked : runtime;
}

inline bool ambisonics_order_mismatches_bake_setting(int stored_baked_order, int configured_bake_order) {
    return effective_baked_ambisonics_order(stored_baked_order) !=
           clamp_bake_ambisonics_order(configured_bake_order);
}

inline bool ambisonics_order_mismatches_runtime(int stored_baked_order, int runtime_order) {
    return effective_baked_ambisonics_order(stored_baked_order) != clamp_bake_ambisonics_order(runtime_order);
}
constexpr float kBakePathingDefaultVisRange = 1000.0f;
constexpr float kBakePathingDefaultPathRange = 1000.0f;
/// Default for bake numSamples and runtime IPLSimulationSettings::numVisSamples.
constexpr int kBakePathingDefaultNumSamples = 4;
constexpr int kRuntimePathingDefaultNumVisSamples = kBakePathingDefaultNumSamples;
constexpr float kBakePathingDefaultRadius = 1.0f;
constexpr float kBakePathingDefaultThreshold = 0.1f;

/// Fraction of CPU cores, 0-1.
constexpr float kDefaultSimulationCpuCoresPercent = 0.15f;

/// rayBatchSize for IPL_SCENETYPE_CUSTOM. Above 1 the batched trace callbacks are registered.
constexpr int kPhysicsRayBatchSizeMin = 1;
constexpr int kPhysicsRayBatchSizeMax = 256;
constexpr int kDefaultPhysicsRayBatchSize = 16;

inline int clamp_physics_ray_batch_size(int v) {
    if (v < kPhysicsRayBatchSizeMin)
        return kPhysicsRayBatchSizeMin;
    if (v > kPhysicsRayBatchSizeMax)
        return kPhysicsRayBatchSizeMax;
    return v;
}

constexpr float kSimulatorSharedInputsDuration = 2.0f;
constexpr float kSimulatorIrradianceMinDistance = 0.1f;

constexpr int kReverbBandCount = 3;
using AudioBands3 = std::array<float, kReverbBandCount>;
/// Steam Audio itself accepts any value >= 0.
constexpr float kPathEQCoeffMin = 1e-6f;
constexpr float kPathEQCoeffMax = 1.0f;

/// Linear RMS below which an output block counts as silent in dropout stats.
constexpr float kInstrumentationSilentBlockThreshold = 0.0001f;
/// Matches the perspective_correction_factor property hint.
constexpr float kPlayerPerspectiveFactorMin = 0.5f;
constexpr float kPlayerPerspectiveFactorMax = 2.0f;
/// Consecutive missing-reverb-params blocks before a player warns.
constexpr int kPlayerNoReverbWarnThreshold = 200;
/// 1/sqrt(2)
constexpr float kAmbisonicWChannelScale = 0.7071067811865475f;
/// 1/sqrt(4*pi), so a W-only field decodes at mono peak level. ResonanceMixerProcessor does not apply it.
constexpr float kAmbisonicDecoderOutputScalar = 0.28209479177387814f;
inline bool is_valid_ambisonic_channel_count(int n) { return n == 4 || n == 9 || n == 16; }
/// Order is clamped to 1-3.
inline int ambisonic_num_channels_for_order(int order) {
    int o = order;
    if (o < 1)
        o = 1;
    if (o > 3)
        o = 3;
    return (o + 1) * (o + 1);
}
constexpr float kDegenerateVectorEpsilon = 1e-8f;
constexpr float kDegenerateVectorEpsilonSq = 1e-16f;

/// Transform-only geometry updates go to the server only every Nth tick.
constexpr int kGeometryTransformCoalesceInterval = 4;

/// Caps main-thread cost of get_ray_debug_segments* when max_rays is high.
constexpr int kRayDebugVizMaxRaysPerCall = 512;
constexpr int kPathingVisMaxSegmentsPerTick = 4096;
constexpr int kPathingVisMaxSegmentsReturned = 2048;

constexpr float kRayDebugMaxDistance = 500.0f;
/// Per buffer of the double buffer.
constexpr int kRayDebugMaxSegments = 8192;
/// Drawn length of a ray that hits nothing.
constexpr float kRayDebugDefaultMissRayLength = 1000.0f;

constexpr int kAttenuationCurveSamples = 64;

/// Occlusion before the first readback; 1 = unoccluded.
constexpr float kOcclusionFetchDefaultVisible = 1.0f;
/// Meters past min_distance where custom-scene rays start, so they do not hit their own collider.
constexpr float kCustomSceneOcclusionRayStartEpsilon = 1e-2f;

constexpr int kMaxOcclusionSamples = 64;
constexpr int kMaxSimulationSources = 32;
constexpr int kMaxOcclusionSamplesUserMax = 128;
/// Also the size of the per-source-handle caches.
constexpr int kMaxSimulationSourcesUserMax = 128;
/// Simulator slots on top of max_simulation_sources, kept free for the listener reverb source.
constexpr int kListenerReverbSourceSlots = 1;
/// Enforced by ResonanceProbeBatchRegistry.
constexpr int kMaxProbeBatches = 1024;
/// Per ResonanceProbeVolume, to bound bake time and memory.
constexpr int kMaxProbesPerVolume = 65536;
constexpr int kMaxTransmissionRays = 256;
constexpr int kTransmissionFreqIndependent = 0;
constexpr int kTransmissionFreqDependent = 1;
constexpr int kDefaultOcclusionSamples = 64;
/// Surfaces counted for transmission, starting closest to the listener.
constexpr int kDefaultTransmissionRays = 1;
/// Matches the ResonancePlayerConfig.max_transmission_surfaces default.
constexpr int kDefaultPlayerConfigTransmissionRays = 1;

constexpr uint64_t kFNVOffsetBasis = 14695981039346656037ULL;
constexpr uint64_t kFNVPrime = 1099511628211ULL;
inline uint64_t fnv1a_hash(const uint8_t* data, size_t size) {
    uint64_t h = kFNVOffsetBasis;
    for (size_t i = 0; i < size; i++) {
        h ^= (uint64_t)data[i];
        h *= kFNVPrime;
    }
    return h;
}
/// Voices beyond this are missing from the audio thread's lock-free snapshot.
constexpr int kMaxPlayerPolyphonySnapshot = 32;

/// Worker ticks without RunPathing after a caught SEH fault on Windows.
constexpr int kPathingCrashCooldownTicks = 5;

/// Same values as IPLReflectionEffectType.
constexpr int kReflectionConvolution = 0;
constexpr int kReflectionParametric = 1;
constexpr int kReflectionHybrid = 2;
constexpr int kReflectionTan = 3;

constexpr int kBakedReflectionConvolution = 0;
constexpr int kBakedReflectionParametric = 1;
constexpr int kBakedReflectionHybrid = 2;

/// Runtime load retries while the instance count is 0.
constexpr int kProbeVolumeMaxRuntimeLoadRetries = 5;
/// Meters below each grid point that Uniform Floor placement searches for a floor.
constexpr float kProbeFloorRaycastDepth = 100.0f;
constexpr double kProbeVizRetryIntervalSec = 1.0;
constexpr const char* kProbeBakeOutputDir = "res://resonance_data/";
constexpr const char* kProbeBakeBatchesSubdir = "batches/";
constexpr double kProbeVizDebounceSec = 0.2;
constexpr float kProbeVizMeshRadius = 0.25f;
constexpr float kProbeVizMeshHeight = 0.5f;
/// 0 = gray (outdated), 1 = blue (up to date), 2 = red (mismatch).
constexpr int kProbeVizColorStateMin = 0;
constexpr int kProbeVizColorStateMax = 2;
constexpr float kProbeVizColorRedR = 1.0f;
constexpr float kProbeVizColorRedG = 0.2f;
constexpr float kProbeVizColorRedB = 0.2f;
constexpr float kProbeVizColorRedA = 1.0f;
constexpr float kProbeVizColorBlueR = 0.2f;
constexpr float kProbeVizColorBlueG = 0.5f;
constexpr float kProbeVizColorBlueB = 1.0f;
constexpr float kProbeVizColorBlueA = 1.0f;
constexpr float kProbeVizColorGrayR = 0.5f;
constexpr float kProbeVizColorGrayG = 0.5f;
constexpr float kProbeVizColorGrayB = 0.5f;
constexpr float kProbeVizColorGrayA = 0.8f;
constexpr float kProbeBakeInfluenceRadiusMin = 1.0f;
constexpr float kProbeRegionSizeMin = 0.1f;
constexpr float kProbeSpacingMin = 0.1f;
constexpr float kProbeSpacingMax = 100.0f;
constexpr float kProbeVizScaleMin = 0.1f;
constexpr float kProbeVizScaleMax = 3.0f;

/// Like Steam Audio's uniform floor placement, a probe's influence radius equals the grid spacing.
inline float probe_grid_influence_radius(float spacing) {
    if (spacing < kProbeSpacingMin)
        return kProbeSpacingMin;
    if (spacing > kProbeSpacingMax)
        return kProbeSpacingMax;
    return spacing;
}

/// Matches the ResonanceSOFAAsset.volume_db property hint.
constexpr float kHRTFVolumeDBMin = -60.0f;
constexpr float kHRTFVolumeDBMax = 24.0f;
/// db_to_gain returns 0 at or below this.
constexpr float kHRTFMinDB = -90.0f;

/// Seconds between label updates.
constexpr double kDebugDrawerLabelUpdateRate = 0.2;
/// Seconds the player debug label stays up with its last values after playback stops.
constexpr double kDebugOverlayGraceSeconds = 2.0;
constexpr float kDebugDrawerLabelPixelSize = 0.005f;
constexpr float kDebugDrawerLabelOffsetY = 0.5f;

/// Searched for the phonon_fmod plugin, relative to the executable or project.
constexpr const char* kFmodPluginPathWindows = "fmod_plugin/windows-x64/";
constexpr const char* kFmodPluginPathNexusLinux = "addons/nexus_resonance/bin/fmod_plugin/linux-x64/";
constexpr const char* kFmodPluginPathFmodLinux = "addons/fmod/lib/linux-x64/";
constexpr const char* kFmodPluginPathFmodLib = "addons/fmod/lib/";
constexpr const char* kFmodPluginPathNexusOsx = "addons/nexus_resonance/bin/fmod_plugin/osx/";
constexpr const char* kFmodPluginPathFmodOsx = "addons/fmod/lib/osx/";
constexpr int kFmodPluginMaxModulePathLen = 1024;

/// Editor-only color for geometry_override.
constexpr float kGeometryOverrideVizR = 0.2f;
constexpr float kGeometryOverrideVizG = 0.9f;
constexpr float kGeometryOverrideVizB = 0.2f;
constexpr float kGeometryOverrideVizA = 0.5f;

/// Default IPLMaterial for scene export and debug geometry.
constexpr float kSceneExportAbsorptionLow = 0.1f;
constexpr float kSceneExportAbsorptionMid = 0.2f;
constexpr float kSceneExportAbsorptionHigh = 0.1f;
constexpr float kSceneExportScattering = 0.5f;
constexpr float kSceneExportTransmission = 0.1f;

} // namespace resonance

#endif // RESONANCE_CONSTANTS_H
