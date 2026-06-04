#ifndef RESONANCE_CONSTANTS_H
#define RESONANCE_CONSTANTS_H

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace resonance {

/// Version string (centralized; override via NEXUS_RESONANCE_VERSION when building)
#ifndef NEXUS_RESONANCE_VERSION
#define NEXUS_RESONANCE_VERSION "0.9.19"
#endif
constexpr const char* kVersion = NEXUS_RESONANCE_VERSION;

/// Godot’s default output buffer size (must match engine / IPL integration)
constexpr int kGodotDefaultFrameSize = 512;
/// Max supported frame size (for stack buffers; 2048 = lowest CPU, highest latency)
constexpr int kMaxAudioFrameSize = 2048;

/// Ring buffer capacity for audio playback (ResonancePlayer, ResonanceAmbisonicPlayer)
constexpr int kRingBufferCapacity = 8192;
/// Crossfade width when splicing `mix_audio` ring chunks (reduces chunk seams).
constexpr int kInputRingChunkCrossfadeSamples = 48;
/// Cap on underrun tail fade: avoids a long flat DC “hump” when the last sample is held for a full mix frame.
constexpr int kSyntheticEosOutputFadeMaxSamples = 128;
/// End-of-stream taper on the last real decoder output (1→0 cosine; clamped to last chunk length).
constexpr int kEosInputEndTaperMaxSamples = 4800; // ~100ms @ 48k
/// Host-side fades (ms → samples via `host_fade_samples_from_ms`); scale with sample rate.
constexpr float kPlaybackHostFadeInMs = 8.7f;   // ~384 samples @ 44.1 kHz
constexpr float kPlaybackHostFadeOutMs = 17.4f; // ~768 samples @ 44.1 kHz
/// Optional extra ramps on stop/start (default off-stack with EOS tapers and repeated `play()`).
constexpr bool kPlaybackHostFadeOutEnabled = false;
constexpr bool kPlaybackHostFadeInEnabled = false;

inline int host_fade_samples_from_ms(float ms, int sample_rate) {
    const int sr = (sample_rate > 0) ? sample_rate : 44100;
    const int n = static_cast<int>(std::llround(static_cast<double>(ms) * 0.001 * static_cast<double>(sr)));
    return (n < 1) ? 1 : n;
}
/// Warm-up: mute spatial output until this many `iplSimulatorRunDirect` ticks (avoids early unoccluded direct).
constexpr int kSpatialAudioWarmupWorkerPasses = 6;

/// Godot process priority (higher = earlier). Chain: listener -> players enqueue source -> runtime flush + tick.
constexpr int kResonanceListenerProcessPriority = 101;
constexpr int kResonancePlayerProcessPriority = 90;
constexpr int kResonanceRuntimeProcessPriority = 0;

/// Lock-free gate for player spatial output: warmup done and Phonon scene committed when triangles exist.
inline bool spatial_audio_geometry_gate_allows_output(int warmup_passes_remaining, int triangle_count,
                                                      bool phonon_scene_audio_ready) {
    if (warmup_passes_remaining > 0)
        return false;
    if (triangle_count > 0 && !phonon_scene_audio_ready)
        return false;
    return true;
}

/// Default reverb/IR duration in seconds (used for irSize = sample_rate * duration)
constexpr float kDefaultReverbDurationSec = 2.0f;

/// Baker defaults for probe reflections bake
constexpr float kBakerSimulatedDuration = 2.0f;
constexpr float kBakerIrradianceMinDistance = 0.5f;
constexpr int kBakerNumDiffuseSamples = 32;                    // IPL diffuse bands (low/mid/high)
constexpr float kBakerMinSpacing = 0.1f;                       // Floor for probe spacing in generate_manual_grid
constexpr float kBakerStaticEndpointSphereRadius = 1.0f;       // IPLSphere.radius when adding probes manually
constexpr float kBakerStaticEndpointInfluenceFallback = 10.0f; // Fallback when influence_radius <= 0 for static endpoint bake

/// Godot ProjectSettings path prefix for this addon (see EditorPlugin registration).
constexpr const char* kProjectSettingsResonancePrefix = "nexus/resonance/";
/// Default baked audio / probe data directory (Project Settings; legacy `bake/output_dir` still read as fallback).
constexpr const char* kProjectSettingsBakeDefaultOutputDirectory = "bake/default_output_directory";
constexpr const char* kProjectSettingsBakeOutputDirectoryLegacy = "bake/output_dir";
/// Editor: probe batch file format (`0` = .tres, `1` = .res). See `resonance_probe_data_save_extension_from_settings()`.
constexpr const char* kProjectSettingsProbeDataFormat = "export/probe_data_format";

/// Baker default parameters (ProjectSettings: bake_num_* / pathing / reflection type; ambisonics order comes from ResonanceBakeConfig or set_bake_params only)
constexpr int kBakeDefaultNumRays = 4096;
constexpr int kBakeDefaultNumBounces = 4;
constexpr int kBakeDefaultNumThreads = 2;
/// IPLReflectionsBakeParams::order (HOA order for baked convolution IRs; typically 1–3).
constexpr int kBakeAmbisonicsOrderMin = 1;
constexpr int kBakeAmbisonicsOrderMax = 3;
constexpr int kBakeDefaultAmbisonicsOrder = 1;

/// STATICSOURCE cliff audit: probe neighborhood radius for baked-energy readback.
constexpr float kStaticSourceProbeNeighborRadiusM = 4.0f;

inline int clamp_bake_ambisonics_order(int order) {
    if (order < kBakeAmbisonicsOrderMin)
        return kBakeAmbisonicsOrderMin;
    if (order > kBakeAmbisonicsOrderMax)
        return kBakeAmbisonicsOrderMax;
    return order;
}
constexpr float kBakePathingDefaultVisRange = 500.0f;
constexpr float kBakePathingDefaultPathRange = 100.0f;
constexpr int kBakePathingDefaultNumSamples = 16;
/// Default for IPLSimulationSettings::numVisSamples at runtime when pathing is on (1–16). Bake uses kBakePathingDefaultNumSamples.
constexpr int kRuntimePathingDefaultNumVisSamples = 4;
constexpr float kBakePathingDefaultRadius = 0.5f;
constexpr float kBakePathingDefaultThreshold = 0.1f;

/// Default CPU fraction for IPL simulation worker threads (`ResonanceRuntimeConfig.simulation_cpu_cores_percent`).
constexpr float kDefaultSimulationCpuCoresPercent = 0.15f;

/// IPLSimulationSettings::rayBatchSize for IPL_SCENETYPE_CUSTOM (Godot physics). >1 registers batched trace callbacks.
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

/// Simulator shared inputs (duration and irradianceMinDistance)
constexpr float kSimulatorSharedInputsDuration = 2.0f;
constexpr float kSimulatorIrradianceMinDistance = 0.1f;

/// Parametric reverb / pathing EQ band count (low/mid/high).
constexpr int kReverbBandCount = 3;
using AudioBands3 = std::array<float, kReverbBandCount>;
/// Path EQ coefficient clamp (IPL allows [0, ∞)).
constexpr float kPathEQCoeffMin = 1e-6f;
constexpr float kPathEQCoeffMax = 1.0f;

/// Instrumentation: RMS threshold below which output is considered "silent" for dropout stats
constexpr float kInstrumentationSilentBlockThreshold = 0.0001f;
/// ResonancePlayer perspective_correction_factor clamp (matches Property hint)
constexpr float kPlayerPerspectiveFactorMin = 0.5f;
constexpr float kPlayerPerspectiveFactorMax = 2.0f;
/// ResonancePlayer "no reverb params" warning: log at most after this many consecutive misses (then counter resets)
constexpr int kPlayerNoReverbWarnThreshold = 200;
/// Ambisonics W-channel normalization (1/sqrt(2))
constexpr float kAmbisonicWChannelScale = 0.7071067811865475f;
/// Nominal Ambisonic decoder output scaling (1/sqrt(4*pi)) so a constant omnidirectional (W-only) field matches mono peak level.
constexpr float kAmbisonicDecoderOutputScalar = 0.28209479177387814f;
/// Valid Ambisonic channel counts: 4 (1st order), 9 (2nd), 16 (3rd)
inline bool is_valid_ambisonic_channel_count(int n) { return n == 4 || n == 9 || n == 16; }
/// HOA channel count (order+1)² for order 1..3.
inline int ambisonic_num_channels_for_order(int order) {
    int o = order;
    if (o < 1)
        o = 1;
    if (o > 3)
        o = 3;
    return (o + 1) * (o + 1);
}
/// Epsilon for degenerate vector check (avoid division by near-zero)
constexpr float kDegenerateVectorEpsilon = 1e-8f;
/// Squared epsilon for length_sq comparisons (kDegenerateVectorEpsilon^2)
constexpr float kDegenerateVectorEpsilonSq = 1e-16f;

/// Transform-only geometry notifies + dynamic instanced-mesh enqueue share this cadence so scene_dirty / queues do not fire every frame (matches former geometry_update_throttle default of 4).
constexpr int kGeometryTransformCoalesceInterval = 4;

/// Max rays traced per get_ray_debug_segments* call (caps main-thread cost when max_rays is high).
constexpr int kRayDebugVizMaxRaysPerCall = 512;
/// Pathing debug: max segments stored per RunPathing tick (callback push_back).
constexpr int kPathingVisMaxSegmentsPerTick = 4096;
/// Pathing debug: max segments copied into Godot Array per get_pathing_visualization_segments call.
constexpr int kPathingVisMaxSegmentsReturned = 2048;

/// Ray debug visualization max distance for reflection ray tracing
constexpr float kRayDebugMaxDistance = 500.0f;
/// RayTraceDebugContext: max segments in double-buffer (per buffer)
constexpr int kRayDebugMaxSegments = 8192;
/// RayTraceDebugContext: default ray length when no hit (miss)
constexpr float kRayDebugDefaultMissRayLength = 1000.0f;

/// Default baked endpoint influence radius for update_source (Reflection baked_var)
constexpr float kBakedEndpointRadius = 10000.0f;

/// Number of samples in attenuation callback curve (linear/custom modes)
constexpr int kAttenuationCurveSamples = 64;

/// If `get_source_occlusion_data` has no readback yet, treat as unoccluded (1 = line-of-sight in IPL).
constexpr float kOcclusionFetchDefaultVisible = 1.0f;
/// Custom-scene occlusion: ray start offset along direction (meters), same order as IPL direct_simulator ray epsilon.
constexpr float kCustomSceneOcclusionRayStartEpsilon = 1e-2f;

/// IPL sim: default max occlusion samples per source
constexpr int kMaxOcclusionSamples = 64;
/// IPL sim: default max concurrent sources
constexpr int kMaxSimulationSources = 32;
/// Upper clamp for user-configured max occlusion samples (ResonanceRuntimeConfig)
constexpr int kMaxOcclusionSamplesUserMax = 128;
/// Upper clamp for user-configured max simulation sources (ResonanceRuntimeConfig / TAN)
constexpr int kMaxSimulationSourcesUserMax = 128;
/// API limit: max probe batches (HandleManager overflow protection; documented for user)
constexpr int kMaxProbeBatches = 1024;
/// API limit: max probes per ResonanceProbeVolume (prevents excessive bake time/memory)
constexpr int kMaxProbesPerVolume = 65536;
/// IPL: max transmission rays per source
constexpr int kMaxTransmissionRays = 256;
/// Direct effect transmission type: frequency-independent
constexpr int kTransmissionFreqIndependent = 0;
/// Direct effect transmission type: frequency-dependent
constexpr int kTransmissionFreqDependent = 1;
/// Default occlusion samples for new sources
constexpr int kDefaultOcclusionSamples = 64;
/// Default transmission rays for new sources (server / create_source_handle)
constexpr int kDefaultTransmissionRays = 32;
/// Default [code]max_transmission_surfaces[/code] when reading ResonancePlayerConfig (inspector default)
constexpr int kDefaultPlayerConfigTransmissionRays = 16;

/// FNV-1a 64-bit offset basis (for probe data hashing)
constexpr uint64_t kFNVOffsetBasis = 14695981039346656037ULL;
/// FNV-1a 64-bit prime multiplier
constexpr uint64_t kFNVPrime = 1099511628211ULL;
/// FNV-1a hash for probe data / geometry (testable without Godot)
inline uint64_t fnv1a_hash(const uint8_t* data, size_t size) {
    uint64_t h = kFNVOffsetBasis;
    for (size_t i = 0; i < size; i++) {
        h ^= (uint64_t)data[i];
        h *= kFNVPrime;
    }
    return h;
}
/// Inter-callback time threshold (us) above which _mix is counted as "late" for dropout diagnostics
constexpr uint64_t kLateMixThresholdUs = 15000;

/// Max ResonancePlayer polyphony voices in the lock-free snapshot read by the audio thread.
constexpr int kMaxPlayerPolyphonySnapshot = 32;

/// Worker ticks to skip iplSimulatorRunPathing after a caught SEH fault (Windows; reduces repeated AV)
constexpr int kPathingCrashCooldownTicks = 5;

/// `ResonanceServer::reflection_type` ↔ IPL mapping
constexpr int kReflectionConvolution = 0;
constexpr int kReflectionParametric = 1;
constexpr int kReflectionHybrid = 2;
constexpr int kReflectionTan = 3;

/// Default reflections mode for players with reflections_type Use Global (config default_reflections_mode).
constexpr int kDefaultReflectionsBaked = 0;
/// Ray-traced reflections for Use Global sources (baked_data_variation -1).
constexpr int kDefaultReflectionsRealtime = 1;

/// Baked reflection types (ResonanceProbeData baked_reflection_type)
constexpr int kBakedReflectionConvolution = 0;
constexpr int kBakedReflectionParametric = 1;
constexpr int kBakedReflectionHybrid = 2;

/// ResonanceProbeVolume: max retries for runtime load when instance count is 0
constexpr int kProbeVolumeMaxRuntimeLoadRetries = 5;
/// ResonanceProbeVolume: raycast depth (world units) for Uniform Floor probe placement
constexpr float kProbeFloorRaycastDepth = 100.0f;
/// ResonanceProbeVolume: retry interval (seconds) before re-attempting viz update when instance count is 0
constexpr double kProbeVizRetryIntervalSec = 1.0;
/// ResonanceProbeVolume: default output directory for baked probe data
constexpr const char* kProbeBakeOutputDir = "res://audio_data/";
/// ResonanceProbeVolume: debounce time (seconds) for deferred viz updates
constexpr double kProbeVizDebounceSec = 0.2;
/// ResonanceProbeVolume: SphereMesh radius/height for probe viz
constexpr float kProbeVizMeshRadius = 0.25f;
constexpr float kProbeVizMeshHeight = 0.5f;
/// ResonanceProbeVolume: viz color state clamp (0=gray, 1=blue, 2=red)
constexpr int kProbeVizColorStateMin = 0;
constexpr int kProbeVizColorStateMax = 2;
/// ResonanceProbeVolume: probe viz colors (R,G,B,A) - red=mismatch, blue=up-to-date, gray=outdated
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
/// ResonanceProbeVolume: bake influence radius min, spacing min/max, viz scale min/max, region size min
constexpr float kProbeBakeInfluenceRadiusMin = 1.0f;
constexpr float kProbeRegionSizeMin = 0.1f;
constexpr float kProbeSpacingMin = 0.1f;
constexpr float kProbeSpacingMax = 100.0f;
constexpr float kProbeVizScaleMin = 0.1f;
constexpr float kProbeVizScaleMax = 3.0f;

/// ResonanceSOFAAsset: volume_db clamp range (matches Property hint -24..24, allows up to -60 for attenuation)
constexpr float kHRTFVolumeDBMin = -60.0f;
constexpr float kHRTFVolumeDBMax = 24.0f;
/// ResonanceSOFAAsset::db_to_gain: floor below which gain is 0 (avoids log(0))
constexpr float kHRTFMinDB = -90.0f;

/// ResonanceDebugDrawer: label update rate (5x per second), pixel size, offset, color
constexpr double kDebugDrawerLabelUpdateRate = 0.2;
/// After playback/sim pipeline stops, keep the player debug HUD (label + occlusion line) visible this long using last samples.
constexpr double kDebugOverlayGraceSeconds = 2.0;
constexpr float kDebugDrawerLabelPixelSize = 0.005f;
constexpr float kDebugDrawerLabelOffsetY = 0.5f;

/// ResonanceFMODBridge: paths to search for phonon_fmod plugin (relative to executable/project)
constexpr const char* kFmodPluginPathWindows = "fmod_plugin/windows-x64/";
constexpr const char* kFmodPluginPathNexusLinux = "addons/nexus_resonance/bin/fmod_plugin/linux-x64/";
constexpr const char* kFmodPluginPathFmodLinux = "addons/fmod/lib/linux-x64/";
constexpr const char* kFmodPluginPathFmodLib = "addons/fmod/lib/";
constexpr const char* kFmodPluginPathNexusOsx = "addons/nexus_resonance/bin/fmod_plugin/osx/";
constexpr const char* kFmodPluginPathFmodOsx = "addons/fmod/lib/osx/";
constexpr int kFmodPluginMaxModulePathLen = 1024;

/// ResonanceGeometry: editor-only viz for geometry_override (semi-transparent green)
constexpr float kGeometryOverrideVizR = 0.2f;
constexpr float kGeometryOverrideVizG = 0.9f;
constexpr float kGeometryOverrideVizB = 0.2f;
constexpr float kGeometryOverrideVizA = 0.5f;

/// ResonanceListener: reflection ray debug viz (green lines)
constexpr float kListenerReflectionRayR = 0.2f;
constexpr float kListenerReflectionRayG = 1.0f;
constexpr float kListenerReflectionRayB = 0.2f;

/// Default IPLMaterial values for scene export and debug geometry (single source)
constexpr float kSceneExportAbsorptionLow = 0.1f;
constexpr float kSceneExportAbsorptionMid = 0.2f;
constexpr float kSceneExportAbsorptionHigh = 0.1f;
constexpr float kSceneExportScattering = 0.5f;
constexpr float kSceneExportTransmission = 0.1f;

} // namespace resonance

#endif // RESONANCE_CONSTANTS_H
