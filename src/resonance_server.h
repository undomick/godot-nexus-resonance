#ifndef RESONANCE_SERVER_H
#define RESONANCE_SERVER_H

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <godot_cpp/classes/audio_frame.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <memory>
#include <mutex>
#include <phonon.h>
#include <thread>
#include <unordered_map>
#include <vector>

#include "handle_manager.h"
#include "ray_trace_debug_context.h"
#include "resonance_baker.h"
#include "resonance_constants.h"
#include "resonance_geometry_asset.h"
#include "resonance_godot_physics_scene_bridge.h"
#include "resonance_probe_batch_registry.h"
#include "resonance_probe_data.h"
#include "resonance_scene_manager.h"
#include "resonance_server_config.h"
#include "resonance_sofa_asset.h"
#include "resonance_steam_audio_context.h"
#include "resonance_utils.h"

#include <godot_cpp/classes/world3d.hpp>

namespace godot {

class ResonanceGeometry;

// --- DATA STRUCTURES ---
struct OcclusionData {
    float occlusion;
    float transmission[3];
    float air_absorption[3];    // From simulation when IPL_DIRECTSIMULATIONFLAGS_AIRABSORPTION enabled
    float directivity;          // From simulation when IPL_DIRECTSIMULATIONFLAGS_DIRECTIVITY enabled
    float distance_attenuation; // From simulation when distanceAttenuationModel set
};

// --- RESONANCE SERVER ---
class ResonanceServer : public Object {
    GDCLASS(ResonanceServer, Object)

  public:
    /// Used by distance attenuation callback; layout must match usage in resonance_server_sources.cpp
    struct AttenuationCallbackData {
        int mode = 0; // 0=inverse (unused), 1=linear, 2=curve
        float min_distance = 1.0f;
        float max_distance = 500.0f;
        float curve_samples[resonance::kAttenuationCurveSamples] = {}; // For curve mode: normalized dist 0..1 -> attenuation
        int num_curve_samples = resonance::kAttenuationCurveSamples;
    };

    /// Thread-safe context for attenuation callback: mutex + data pointer for worker thread access
    struct AttenuationCallbackContext {
        std::recursive_mutex* mutex = nullptr;
        const AttenuationCallbackData* data = nullptr;
    };

    /// Per-source simulation inputs for update_source / try_update_source / enqueue_source_update.
    struct SourceUpdateParams {
        Vector3 position{};
        float radius = 1.0f;
        Vector3 source_forward{0, 0, -1};
        Vector3 source_up{0, 1, 0};
        float directivity_weight = 0.0f;
        float directivity_power = 1.0f;
        bool air_absorption_enabled = true;
        bool use_sim_distance_attenuation = false;
        float min_distance = 1.0f;
        bool path_validation_enabled = false;
        bool find_alternate_paths = false;
        int occlusion_samples = 64;
        int num_transmission_rays = 32;
        int baked_data_variation = 0;
        Vector3 baked_endpoint_center{};
        float baked_endpoint_radius = 0.0f;
        int32_t pathing_probe_batch_handle = -1;
        int reflections_enabled_override = -1;
        int pathing_enabled_override = -1;
        /// -1 = use global ResonanceServer occlusion_type; 0 = raycast; 1 = volumetric.
        int occlusion_type_override = -1;
        bool simulation_occlusion_enabled = true;
        bool simulation_transmission_enabled = true;
        float direct_mix_level = 1.0f;
        float reflections_mix_level = 1.0f;
        float pathing_mix_level = 1.0f;
    };

  private:
    struct SourceUpdateRecord {
        SourceUpdateParams params;
        bool valid = false;
    };

    struct AttenuationEntry {
        std::unique_ptr<AttenuationCallbackData> data;
        AttenuationCallbackContext ctx{};
    };

    // Steam Audio Context (owns context, embree, opencl, radeon rays, TAN, HRTF)
    std::unique_ptr<ResonanceSteamAudioContext> steam_audio_context_;
    ResonanceGodotPhysicsSceneBridge godot_physics_bridge_;
    /// Custom-scene ray excludes: merged user + listener + auto-registered CollisionObject3D RIDs (see _rebuild_and_apply_physics_ray_excludes_unlocked).
    std::mutex physics_ray_excludes_mutex_;
    TypedArray<RID> physics_ray_exclude_rids_user_;
    TypedArray<RID> listener_physics_ray_exclude_rids_;
    std::unordered_map<int64_t, int> physics_ray_auto_exclude_refcount_;
    std::vector<RID> physics_ray_auto_exclude_active_;
    void _rebuild_and_apply_physics_ray_excludes_unlocked();
    void _clear_physics_ray_excludes_state();
    IPLScene scene = nullptr;
    /// One Steam Audio pack per ResonanceStaticScene ObjectID (always InstancedMesh).
    struct RuntimeStaticPack {
        IPLScene sub_scene = nullptr;
        IPLStaticMesh mesh_in_sub = nullptr;
        IPLInstancedMesh instanced = nullptr;
        IPLMatrix4x4 transform{};
        int debug_id = -1;
        int tri_count = 0;
    };
    std::unordered_map<uint64_t, RuntimeStaticPack> _runtime_static_packs;
    uint64_t _next_ephemeral_static_pack_id = 1; // For APIs without a Node ObjectID
    int _runtime_static_triangle_count = 0;      // Sum of triangles across _runtime_static_packs
    void _release_static_pack_assume_locked(RuntimeStaticPack& pack);
    void _clear_static_packs_assume_locked();
    bool _create_instanced_static_pack_assume_locked(const Ref<ResonanceGeometryAsset>& asset, const Transform3D& transform,
                                                     RuntimeStaticPack& out);
    std::unordered_map<int32_t, std::unique_ptr<AttenuationEntry>> _source_attenuation_entries;
    std::unordered_map<int32_t, SourceUpdateRecord> _source_update_snapshot_;
    /// Per-source override for baked_reverb_use_listener_probe. Encodes -1 (use global), 0 (off), 1 (on)
    /// as int8_t per source slot. Lock-free so the player can update it from the main thread without touching
    /// simulation_mutex (the global flag rarely flips, but we still want zero overhead in the hot path).
    /// Sized via resonance::kMaxSimulationSourcesUserMax (kMaxCacheHandles isn't declared yet in this scope).
    std::array<std::atomic<int8_t>, resonance::kMaxSimulationSourcesUserMax> _source_baked_reverb_listener_probe_override_{};
    std::recursive_mutex _attenuation_callback_mutex;
    IPLSimulator simulator = nullptr;

    // Mixer (shared reflection mixer): must be safe in audio callbacks without blocking.
    // We use a lock-free reader guard (atomic reader count + atomic pointer) so the mixer can be swapped/released
    // on the main/worker threads without ever taking a mutex in the audio thread.
    std::atomic<IPLReflectionMixer> reflection_mixer_{nullptr};
    mutable std::atomic<int> reflection_mixer_readers_{0};

    struct MixerReadGuard {
        const ResonanceServer* srv = nullptr;
        IPLReflectionMixer mixer = nullptr;

        MixerReadGuard() = default;
        explicit MixerReadGuard(const ResonanceServer* p_srv) : srv(p_srv) {
            if (!srv)
                return;
            srv->reflection_mixer_readers_.fetch_add(1, std::memory_order_acq_rel);
            mixer = srv->reflection_mixer_.load(std::memory_order_acquire);
        }
        MixerReadGuard(const MixerReadGuard&) = delete;
        MixerReadGuard& operator=(const MixerReadGuard&) = delete;
        MixerReadGuard(MixerReadGuard&& other) noexcept {
            srv = other.srv;
            mixer = other.mixer;
            other.srv = nullptr;
            other.mixer = nullptr;
        }
        MixerReadGuard& operator=(MixerReadGuard&& other) noexcept {
            if (this == &other)
                return *this;
            release();
            srv = other.srv;
            mixer = other.mixer;
            other.srv = nullptr;
            other.mixer = nullptr;
            return *this;
        }
        ~MixerReadGuard() { release(); }

        void release() {
            if (!srv)
                return;
            srv->reflection_mixer_readers_.fetch_sub(1, std::memory_order_acq_rel);
            srv = nullptr;
            mixer = nullptr;
        }
        IPLReflectionMixer get() const { return mixer; }
        explicit operator bool() const { return mixer != nullptr; }
    };

    void _set_reflection_mixer(IPLReflectionMixer new_mixer);
    void _release_reflection_mixer_when_unused(IPLReflectionMixer mixer) const;

    // Reverb Bus instrumentation (updated from audio thread; read from main)
    std::atomic<uint64_t> reverb_effect_process_calls{0};
    std::atomic<uint64_t> reverb_effect_mixer_null{0};
    std::atomic<uint64_t> reverb_effect_success{0};
    std::atomic<uint64_t> reverb_effect_frames_written{0};
    std::atomic<float> reverb_effect_output_peak{0.0f};
    std::atomic<float> reverb_effect_output_rms{0.0f};
    /// MixerReturn output levels before applying ResonanceAudioEffect.gain_db.
    std::atomic<float> reverb_effect_output_peak_pre_gain{0.0f};
    std::atomic<float> reverb_effect_output_rms_pre_gain{0.0f};
    /// ResonanceAudioEffect: peak-collapse linear duck triggered (wet-tail debug).
    std::atomic<uint64_t> reverb_effect_click_guard_triggers{0};
    /// ResonanceMixerProcessor: hold-last stereo block used (no new mixer feed this bus tick).
    std::atomic<uint64_t> reverb_mixer_return_hold_last_count{0};
    /// Ablation / A-B: when false, skip click guard ramp (default true).
    std::atomic<bool> reverb_bus_click_guard_enabled_{true};
    /// Ablation: when true, ResonancePlayer split-reverb underrun uses zeros instead of hold x cosine taper.
    std::atomic<bool> reverb_bus_wet_ring_underrun_zero_fill_{false};
    std::atomic<uint64_t> reverb_mixer_feed_count{0};
    /// Convolution debug: fetch_reverb_params returned true with valid IR (reflection_type==0)
    std::atomic<uint64_t> reverb_convolution_valid_fetches{0};
    /// Reflection fetch: served from epoch cache (audio thread, lock-free)
    std::atomic<uint64_t> instrumentation_fetch_cache_hit{0};
    /// Crackling debug: try_lock missed, cache empty, returned false
    std::atomic<uint64_t> instrumentation_fetch_cache_miss{0};
    /// Epoch mismatch but entry still held last IR/params (short tones before next worker refresh)
    std::atomic<uint64_t> instrumentation_fetch_refl_stale_epoch_fallback{0};
    /// Fetch was skipped intentionally (disabled or no data expected yet) - do not count as a miss.
    std::atomic<uint64_t> instrumentation_fetch_cache_skip{0};
    /// Pathing: fetch_pathing_params returned immediately (bad handle, no context, or pathing off)
    std::atomic<uint64_t> instrumentation_pathing_fetch_early_exit{0};
    /// Pathing: worker sync skipped pathing GetOutputs (source had no IPLSource) - legacy counter name
    std::atomic<uint64_t> instrumentation_pathing_fetch_src_null{0};
    /// Pathing: lock ok, copied valid SH pathing coefficients
    std::atomic<uint64_t> instrumentation_pathing_fetch_sh_ok{0};
    /// Pathing: lock ok, iplSourceGetOutputs PATHING but shCoeffs null
    std::atomic<uint64_t> instrumentation_pathing_fetch_sh_null{0};
    /// Pathing: lock ok, shCoeffs set but order/sh_count invalid (unstable pointer path skipped)
    std::atomic<uint64_t> instrumentation_pathing_fetch_sh_bad_order{0};
    /// Pathing: try_lock failed, served from pathing cache
    std::atomic<uint64_t> instrumentation_pathing_fetch_cache_hit{0};
    /// Pathing: try_lock failed, no cache entry for handle
    std::atomic<uint64_t> instrumentation_pathing_fetch_cache_miss{0};
    /// Pathing: worker entered RunPathing (before SEH-wrapped or direct iplSimulatorRunPathing)
    std::atomic<uint64_t> instrumentation_pathing_sim_attempt{0};
    /// Pathing: iplSimulatorRunPathing completed successfully this worker tick
    std::atomic<uint64_t> instrumentation_pathing_sim_ran{0};
    /// Pathing: Windows SEH caught a fault inside iplSimulatorRunPathing (cooldown applied)
    std::atomic<uint64_t> instrumentation_pathing_sim_seh_fail{0};
    /// Pathing: heavy tick skipped RunPathing - pending_listener_valid false
    std::atomic<uint64_t> instrumentation_pathing_sim_skip_listener{0};
    /// Pathing: heavy tick skipped RunPathing - crash cooldown active
    std::atomic<uint64_t> instrumentation_pathing_sim_skip_cooldown{0};
    /// ResonancePlayer: entered pathing mix branch (global pathing on, pathing_mix > 0)
    std::atomic<uint64_t> instrumentation_pathing_player_gate{0};
    /// ResonancePlayer: fetch_pathing_params succeeded and path effect applied
    std::atomic<uint64_t> instrumentation_pathing_player_applied{0};
    /// ResonancePlayer: gate passed but fetch_pathing_params false (no audio this block)
    std::atomic<uint64_t> instrumentation_pathing_player_fetch_miss{0};
    /// Last simulation worker tick only: microseconds (profiling). Cleared when worker idle.
    std::atomic<uint64_t> instrumentation_worker_us_run_direct{0};
    std::atomic<uint64_t> instrumentation_worker_us_run_reflections{0};
    std::atomic<uint64_t> instrumentation_worker_us_run_pathing{0};
    std::atomic<uint64_t> instrumentation_worker_us_sync_fetch{0};
    /// Last tick: effective sharedInputs.numRays (0 when no realtime reflection sources).
    std::atomic<int32_t> instrumentation_worker_last_num_rays_{0};
    /// Last tick: adaptive realtime ray target before clamping to max_rays (debug).
    std::atomic<int32_t> instrumentation_worker_last_adaptive_num_rays_target_{0};
    /// Last tick: number of sources currently flagged for reflections / realtime reflections.
    std::atomic<int32_t> instrumentation_worker_active_reflection_sources_{0};
    std::atomic<int32_t> instrumentation_worker_active_realtime_reflection_sources_{0};
    /// Last tick: time in iplSimulatorCommit (Steam Audio requires this after SetSharedInputs; excludes scene graph commit).
    std::atomic<uint64_t> instrumentation_worker_us_simulator_commit{0};
    /// Last tick: iplSceneCommit(scene) + iplSimulatorSetScene when scene was dirty (geometry / instanced mesh updates).
    std::atomic<uint64_t> instrumentation_worker_us_scene_graph_commit{0};
    /// Last tick: wall time in _apply_queued_dynamic_instanced_mesh_transforms_assume_locked (queue drain + optional iplInstancedMeshUpdateTransform).
    std::atomic<uint64_t> instrumentation_worker_us_dynamic_instanced_apply{0};
    /// Last completed simulation pass: true if reflections/pathing interval requested a "heavy" tick (r/p timings apply).
    std::atomic<bool> instrumentation_worker_last_wake_was_heavy{false};
    /// Last tick: portions of _worker_sync_fetch_caches (subset of us_sync_fetch; profiling only).
    std::atomic<uint64_t> instrumentation_worker_us_sync_fetch_occlusion{0};
    std::atomic<uint64_t> instrumentation_worker_us_sync_fetch_reflections{0};
    std::atomic<uint64_t> instrumentation_worker_us_sync_fetch_pathing{0};

    /// Audio thread: last iplReflectionEffectApply (convolution path to mixer) block, microseconds.
    std::atomic<uint64_t> instrumentation_audio_conv_refl_apply_last_us_{0};
    /// Audio thread: last reverb bus iplReflectionMixerApply + decode block, microseconds.
    std::atomic<uint64_t> instrumentation_audio_conv_reverb_bus_last_us_{0};
    /// Audio thread: last reverb bus _sanitize_audio_buffer on ambisonic mixer output (channels = order²), microseconds.
    std::atomic<uint64_t> instrumentation_audio_mixer_sanitize_ambi_last_us_{0};
    /// Audio thread: last reverb bus _sanitize_audio_buffer on stereo after decode, microseconds.
    std::atomic<uint64_t> instrumentation_audio_mixer_sanitize_stereo_last_us_{0};
    /// Convolution debug: process_mix called for convolution with ir==null (should not happen)
    std::atomic<uint64_t> reverb_convolution_feed_ir_null{0};
    /// Convolution debug: min mixer-feed gain seen (reflections_mix_level * source linear volume)
    std::atomic<float> reverb_convolution_gain_min{1.0f};
    /// Convolution debug: max reverb_gain seen when feeding
    std::atomic<float> reverb_convolution_gain_max{0.0f};
    /// Convolution debug: max RMS of mono input to convolution (before reverb_gain scale)
    std::atomic<float> reverb_convolution_input_rms_max{0.0f};

    /// Runtime frame_size detection: reverb bus reports actual Godot frame_count; main thread performs reinit
    std::atomic<int> pending_reinit_frame_size_{0};
    /// True when last init used Auto (audio_frame_size 0). Effect only requests reinit when Auto to avoid overriding user choice.
    std::atomic<bool> audio_frame_size_was_auto_{true};
    /// 0 = spatialized player output allowed; >0 = suppress until worker completes this many RunDirect ticks after reset.
    std::atomic<int> spatial_audio_warmup_passes_remaining_{0};
    /// False after triangle geometry is registered until the worker runs iplSceneCommit for that edit.
    std::atomic<bool> phonon_scene_audio_ready_{true};

    // Helpers
    ResonanceBaker baker;
    SourceManager source_manager;
    /// Parsed config (Dictionary via apply). Member fields in the "Configuration (Defaults)" block are copied in _apply_config for hot paths; keep both in sync when adding keys.
    ResonanceServerConfig config_;

    // Simulation State
    IPLSimulationSettings simulation_settings{};
    std::atomic<int> global_triangle_count{0};

    /// FMOD Bridge: IPLSource at listener position for iplFMODSetReverbSource. -1 when not created.
    int32_t fmod_reverb_source_handle_ = -1;
    /// Bumped on Steam Audio shutdown/reinit so clients can detect recycled source handles.
    /// Never 0 (see resonance_source_handle_policy.h).
    std::atomic<uint32_t> source_lifecycle_epoch_{1};

    // Configuration (Defaults)
    int current_sample_rate = 48000;
    int frame_size = resonance::kGodotDefaultFrameSize; // Steam Audio block size (256/512/1024). Matched to Godot mix callback for best perf.
    int ambisonic_order = 1;
    float max_reverb_duration = 2.0f;
    int simulation_threads = 1;                                                        // Computed from simulation_cpu_cores_percent
    float simulation_cpu_cores_percent = resonance::kDefaultSimulationCpuCoresPercent; // 0-1 fraction of CPU cores for simulation
    int max_rays = 4096;
    int max_bounces = 4;
    float reverb_influence_radius = 10000.0f;
    float reverb_transmission_amount = 1.0f; // 0 = no transmission damping on reverb, 1 = full damping
    /// Baked-REVERB only: when true (default), the reflection effect input gain is multiplied by the direct-path
    /// occlusion/transmission factor so walls also damp the wet signal - the baked IR cannot encode the
    /// source/listener geometry the way realtime ray-traced convolution does. Disable for stylised, always-on
    /// reverb beds or scenes where direct-line occlusion would over-dampen plausible corner leakage.
    bool apply_occlusion_to_baked_reflections = true;
    /// Baked-REVERB only: when true (default), the reflection-side iplSourceSetInputs is fed the listener position
    /// instead of the source position so Steam Audio looks up the probe nearest the listener - IPL_BAKEDDATAVARIATION_REVERB
    /// assumes source==listener at the probe, so the listener's room is the correct IR to play back. Disable when the
    /// source's room should win even with a distant listener (e.g. one giant cathedral probe and tiny side rooms you
    /// want to keep dry).
    bool baked_reverb_use_listener_probe = true;

    // Reflection type: 0 = Convolution, 1 = Parametric, 2 = Hybrid
    int reflection_type = 0;
    /// When player reflections_type is Use Global: 0=Baked, 1=Realtime
    int default_reflections_mode = 0;
    float hybrid_reverb_transition_time = 1.0f;
    float hybrid_reverb_overlap_percent = 0.25f; // 0.0-1.0 (25% from config)
    // Transmission type: 0 = FreqIndependent, 1 = FreqDependent
    int transmission_type = 1;
    /// Default Steam Audio numTransmissionRays for new sources; player config falls back here when unset.
    int max_transmission_surfaces = resonance::kDefaultPlayerConfigTransmissionRays;
    // Occlusion type: 0 = Raycast, 1 = Volumetric
    int occlusion_type = 1;
    int max_occlusion_samples = resonance::kMaxOcclusionSamples;
    int max_simulation_sources = resonance::kMaxSimulationSources;
    float hrtf_volume_db = 0.0f;
    /// 0=None, 1=RMS for embedded default HRTF; SOFA uses asset norm_type.
    int hrtf_normalization_type = 0;
    // Custom HRTF: ResonanceSOFAAsset with volume/norm. Null = default embedded HRTF.
    Ref<ResonanceSOFAAsset> hrtf_sofa_asset;
    // Headphone HRTF toggles (see ResonanceRuntimeConfig; require loaded HRTF to take effect)
    bool direct_binaural = true;
    bool reverb_binaural = true;
    bool pathing_binaural = true;
    // HRTF interpolation: false = Nearest (faster), true = Bilinear (smoother for moving sources)
    bool hrtf_interpolation_bilinear = false;
    // Virtual Surround: decode reverb Ambisonics to 7.1, then iplVirtualSurroundEffect -> stereo (for speaker layouts without HRTF)
    bool use_virtual_surround = false;
    /// IPLSpeakerLayout channel count for direct IPLPanningEffect / optional Ambisonics panning (1,2,4,6,8).
    int direct_speaker_channels = 2;
    // Enable pathing simulation (multi-path sound propagation around obstacles)
    bool pathing_enabled = false;
    /// True when the live simulator was created with IPL_SIMULATIONFLAGS_PATHING.
    /// Enabling pathing at runtime without recreate cannot allocate PathSimulator internals.
    bool simulator_created_with_pathing_ = false;
    // Pathing visibility params (bakingVisibilityRadius/Threshold/Range)
    float pathing_vis_radius = 0.5f;
    float pathing_vis_threshold = 0.1f;
    float pathing_vis_range = 100.0f;
    bool pathing_normalize_eq = true;
    int pathing_num_vis_samples = resonance::kRuntimePathingDefaultNumVisSamples;
    /// Defaults when ResonancePlayerConfig path validation / find-alternate uses Use Global (-1).
    bool path_validation_enabled = true;
    bool find_alternate_paths = false;
    // Optional custom deviation model for pathing (freq-dependent attenuation around corners). nullptr = default (UTD).
    IPLDeviationModel _pathing_deviation_model{};
    bool _pathing_deviation_callback_enabled = false;
    std::mutex _pathing_deviation_mutex;
    // Ray tracer: 0=Default (built-in), 1=Embree (Intel), 2=Radeon Rays (GPU), 3=Custom (Godot physics)
    int scene_type = 0;
    /// Requested IPLSimulationSettings::rayBatchSize for Custom scene; clamped at init. Non-Custom always uses 1.
    int physics_ray_batch_size = resonance::kDefaultPhysicsRayBatchSize;
    // OpenCL device selection when scene_type=2 or TAN: type 0=GPU, 1=CPU, 2=Any; index = device index in list
    int opencl_device_type = 0; // IPL_OPENCLDEVICETYPE_GPU
    int opencl_device_index = 0;
    // Context: validation (debug), SIMD level (0=AVX512, 1=AVX2, 2=AVX, 3=SSE4, 4=SSE2; -1=default)
    bool context_validation = false;
    int context_simd_level = -1; // -1 = use phonon default (auto)
    // Realtime reflection quality (when max_rays > 0)
    float realtime_irradiance_min_distance = 0.1f;
    float realtime_simulation_duration = 2.0f;
    int realtime_num_diffuse_samples = 32;

    // Flags (atomic: written from main/Godot, read from audio/worker threads)
    std::atomic<bool> output_direct_enabled{true};
    std::atomic<bool> output_reverb_enabled{true};
    std::atomic<bool> debug_occlusion{false};
    std::atomic<bool> debug_reflections{false};
    std::atomic<bool> debug_pathing{false};

    // Perspective Correction (non-VR: spatialize from on-screen position for better localization)
    std::atomic<bool> perspective_correction_enabled{false};
    std::atomic<float> perspective_correction_factor{1.0f};

    // Pathing visualization (callback stores segments for debug drawing)
    struct PathVisSegment {
        Vector3 from;
        Vector3 to;
        bool occluded;
    };
    std::vector<PathVisSegment> pathing_vis_segments;
    std::mutex pathing_vis_mutex;
    /// userData is this; must remain valid while pathingVisCallback is set on shared inputs (see shutdown order).
    static void IPLCALL _pathing_vis_callback(IPLVector3 from, IPLVector3 to, IPLbool occluded, void* userData);

    RayTraceDebugContext ray_trace_debug_context_;
    std::atomic<int> ray_debug_bounce_index_{0};

    // --- Audio-thread caches (lock-free double buffer) ---
    // Worker writes into back slot, publishes by flipping the front index.
    static constexpr int kCacheSlots = 2;
    static constexpr int kMaxPathingSHCoeffs = 16; // max HOA channels for order 3: (3+1)^2
    static constexpr int kMaxCacheHandles = resonance::kMaxSimulationSourcesUserMax;

    // Parametric reverb cache: worker publishes; audio reads lock-free by epoch.
    struct CachedParametricReverb {
        float reverbTimes[3] = {0};
        float eq[3] = {0};
        uint32_t epoch = 0;
    };
    std::array<std::array<CachedParametricReverb, kMaxCacheHandles>, kCacheSlots> reverb_param_cache_{};
    std::atomic<int> reverb_param_cache_front_{0};
    uint32_t reverb_param_cache_epoch_[kCacheSlots] = {1, 1};

    // Convolution/Hybrid/TAN reflection cache: worker publishes; audio reads lock-free by epoch.
    // ir pointer (TripleBuffer) is stable; caching full IPLReflectionEffectParams is safe.
    struct CachedReflectionParams {
        IPLReflectionEffectParams params{};
        uint32_t epoch = 0;
    };
    std::array<std::array<CachedReflectionParams, kMaxCacheHandles>, kCacheSlots> reflection_param_cache_{};
    std::atomic<int> reflection_param_cache_front_{0};
    uint32_t reflection_param_cache_epoch_[kCacheSlots] = {1, 1};
    /// Worker-only: last convolution IR params per handle when fetch_ok (null-IR fallback).
    std::array<IPLReflectionEffectParams, kMaxCacheHandles> last_good_reflection_params_{};
    std::array<std::atomic<uint8_t>, kMaxCacheHandles> last_good_reflection_valid_{};
    /// Worker-only: last interpolated STATICSOURCE baked energy (repro trace / debug).
    float reflection_baked_energy_last_[kMaxCacheHandles]{};

    /// Last known result of a live reflection fetch (worker sync or fetch_reverb_params with simulation_mutex).
    /// Lets the main thread avoid calling fetch_reverb_params only for has_reverb UI/state (see peek_reverb_params_likely_available).
    mutable std::mutex reverb_params_likely_available_mutex_;
    std::unordered_map<int32_t, bool> reverb_params_likely_available_;
    void _set_reverb_params_likely_available_hint(int32_t handle, bool likely);
    void _clear_reverb_params_likely_available_hints();

    // Pathing cache: worker copies SH coeffs into slot; audio reads lock-free by epoch.
    // shCoeffs points to source's single buffer (overwritten each RunPathing); must copy SH data.
    struct CachedPathingParams {
        float eqCoeffs[3] = {0};
        std::array<float, kMaxPathingSHCoeffs> shCoeffs{};
        int order = 1;
        uint32_t epoch = 0;
    };
    std::array<std::array<CachedPathingParams, kMaxCacheHandles>, kCacheSlots> pathing_param_cache_{};
    std::atomic<int> pathing_param_cache_front_{0};
    uint32_t pathing_param_cache_epoch_[kCacheSlots] = {1, 1};

    // Direct simulation outputs (occlusion, etc.): ResonancePlayer::_process calls get_source_occlusion_data every frame
    // from the main thread. It must not block on simulation_mutex while the worker holds it during RunReflections.
    struct CachedOcclusionData {
        OcclusionData data{};
        uint32_t epoch = 0;
    };
    std::array<std::array<CachedOcclusionData, kMaxCacheHandles>, kCacheSlots> occlusion_cache_{};
    std::atomic<int> occlusion_cache_front_{0};
    uint32_t occlusion_cache_epoch_[kCacheSlots] = {1, 1};

    // Threading: simulation_mutex serializes IPL scene/simulator mutations, bake, geometry notify, and worker phonon tick.
    // probe_batch_registry_.mutex_ is independent (load/remove/revalidate). When both are needed, take simulation_mutex
    // first, then the registry mutex (worker already holds simulation_mutex when calling get_pathing_batch).
    // pending_source_lifecycle_mutex_, worker_mutex, pathing_vis_mutex, _attenuation_callback_mutex: domain-specific, no nesting with simulation_mutex unless documented at call site.
    // Audio thread: fetch_* / is_spatial_audio_output_ready read lock-free caches and atomics (global_triangle_count, phonon_scene_audio_ready_).
    // Teardown: ipl_teardown_active_; join worker; AudioServer::lock and drain IPL clients; probe registry then simulation_mutex for IPL release.
    std::mutex simulation_mutex;
    std::thread worker_thread;
    std::mutex worker_mutex;
    std::condition_variable worker_cv;
    std::atomic<bool> thread_running = false;
    std::atomic<bool> simulation_requested = false;
    std::atomic<bool> scene_dirty = false;
    /// Shared with consume_geometry_transform_coalesce_tick (former geometry_update_throttle counter).
    std::atomic<uint32_t> geometry_transform_coalesce_counter_{0};
    float dynamic_scene_commit_min_interval_ = 0.0f;
    std::chrono::steady_clock::time_point last_dynamic_scene_commit_time_{};
    std::mutex dynamic_instanced_transform_queue_mutex_;
    std::unordered_map<IPLInstancedMesh, IPLMatrix4x4> dynamic_instanced_transform_queue_;
    std::atomic<uint64_t> instrumentation_main_us_dynamic_transform_enqueue_{0};
    /// Microseconds of the most recent enqueue_dynamic_instanced_mesh_transform call (main thread).
    std::atomic<uint64_t> instrumentation_main_us_last_dynamic_transform_enqueue_{0};
    std::atomic<uint64_t> instrumentation_dynamic_transform_enqueue_events_{0};
    // Simulation update interval: Direct runs every tick; reflection/pathing heavy ticks use separate cadences when configured.
    float reflections_sim_interval = 0.1f; // Min seconds between iplSimulatorRunReflections scheduling
    float pathing_sim_interval = 0.1f;     // Min seconds between RunPathing scheduling
    float reflections_interval_elapsed = 0.0f;
    float pathing_interval_elapsed = 0.0f;
    float realtime_reflection_max_distance_m = 0.0f;
    std::atomic<bool> reflection_sim_heavy_requested{false};
    std::atomic<bool> pathing_sim_heavy_requested{false};
    // Per-handle flags (lock-free; used from worker/audio threads without data races).
    // 0 = false, 1 = true. Index is the user handle (0..kMaxCacheHandles-1).
    std::array<std::atomic<uint8_t>, kMaxCacheHandles> source_outputs_reflections_{};
    std::array<std::atomic<uint8_t>, kMaxCacheHandles> source_outputs_realtime_reflections_{};
    std::array<std::atomic<uint8_t>, kMaxCacheHandles> source_outputs_pathing_{};
    /// When > 0, worker may skip RunDirect on non-heavy ticks until this much time has passed (see tick()).
    float direct_sim_interval = 0.0f;
    float direct_sim_time_elapsed = 0.0f;
    std::atomic<bool> worker_run_direct_next{true};
    /// When set by worker (defer RunReflections), tick() re-arms reflection heavy on next frame.
    std::atomic<bool> reflection_force_heavy_next_tick_{false};
    /// Extra seconds added to reflection-heavy interval when reflections_adaptive_budget_us_ > 0 (see tick()).
    float reflections_adaptive_extra_interval_ = 0.0f;
    uint32_t reflections_adaptive_budget_us_ = 0;
    int reflections_adaptive_ray_min_ = 128;
    float reflections_adaptive_ray_recover_frac_ = 0.125f;
    int reflections_adaptive_ray_recover_cap_ = 512;
    float reflections_adaptive_step_sec_ = 0.02f;
    float reflections_adaptive_max_extra_interval_ = 0.2f;
    float reflections_adaptive_decay_per_sec_ = 0.05f;
    uint32_t reflections_defer_after_scene_commit_us_ = 0;
    int convolution_ir_max_samples_ = 0;

    // Adaptive realtime reflections rays (worker-only; protected by simulation_mutex).
    int _adaptive_realtime_num_rays_ = 0;
    bool _adaptive_realtime_num_rays_initialized_ = false;

    bool batch_source_updates = true;
    std::mutex source_update_batch_mutex_;
    std::unordered_map<int32_t, SourceUpdateParams> source_update_batch_;

    /// Phase 1: deferred source lifecycle ops so main thread never blocks on simulation_mutex
    /// for iplSourceAdd / iplSourceRemove / iplSimulatorCommit.
    /// Drained at the start of every worker tick (assume_locked helper below).
    struct PendingSourceAdd {
        int32_t handle = -1;
        SourceUpdateParams initial;
    };
    std::mutex pending_source_lifecycle_mutex_;
    std::vector<PendingSourceAdd> pending_source_adds_;
    /// Retained IPLSource handles waiting for iplSourceRemove + iplSimulatorCommit + iplSourceRelease on worker.
    std::vector<IPLSource> pending_source_removes_;
    /// Per-handle cache-cleanup list drained after iplSourceRemove.
    std::vector<int32_t> pending_source_post_remove_cleanup_;
    /// 1 while handle is in source_manager but not yet iplSourceAdd'd. Audio fetch_* reads lock-free.
    std::array<std::atomic<uint8_t>, resonance::kMaxSimulationSourcesUserMax> source_attach_pending_{};

    // Listener: main thread writes `listener_coords_latest_` under a seqlock (odd seq = write in progress).
    // Audio and simulation read a consistent snapshot - never "consume" so the reverb bus and ResonancePlayer
    // always see the same basis in a given read (avoids comb filtering / wet cancellation vs dry).
    IPLCoordinateSpace3 listener_coords_latest_{};
    std::atomic<uint32_t> listener_seq_{0};
    std::atomic<bool> pending_listener_valid{true};
    /// True when iplSimulatorRunPathing ran this simulation tick (avoids using stale pathing when skipped)
    std::atomic<bool> pathing_ran_this_tick{false};
    /// True after first iplSimulatorRunReflections (avoids Steam Audio reverbTimes=0 validation warning at game start)
    std::atomic<bool> reflections_have_run_once_{false};
    /// Per-handle: true for sources added before their first RunReflections (Parametric/Hybrid gate).
    /// Must be lock-free for the audio thread.
    std::array<std::atomic<bool>, resonance::kMaxSimulationSourcesUserMax> reflections_pending_{};
    /// Ticks to skip RunPathing after Windows SEH catch (reduces repeated access violations)
    std::atomic<int> pathing_crash_cooldown{0};
    /// Extra retains from iplProbeBatchRetain in _update_source_internal; released after iplSimulatorRunPathing.
    /// Requires simulation_mutex (same lock as SetInputs / worker sim).
    std::vector<IPLProbeBatch> pathing_probe_batches_pending_release_;
    /// One-time warning when pathing enabled but no pathing data (avoids log spam)
    bool pathing_no_data_warned = false;

    /// When non-empty, bake uses these assets (with transforms) instead of live geometry. Set by editor before bake.
    std::vector<Ref<ResonanceGeometryAsset>> _bake_static_scene_assets;
    std::vector<Transform3D> _bake_static_scene_transforms;

    // Bake param overrides from ResonanceRuntimeConfig (DRY; replaces ProjectSettings)
    int _bake_num_rays = -1;
    int _bake_num_bounces = -1;
    int _bake_reflection_type = -1;
    float _bake_pathing_vis_range = -1.0f;
    float _bake_pathing_path_range = -1.0f;
    int _bake_pathing_num_samples = -1;
    float _bake_pathing_radius = -1.0f;
    float _bake_pathing_threshold = -1.0f;
    int _bake_num_threads = -1;
    int _bake_ambisonics_order = -1;
    bool _bake_pipeline_pathing = false;
    std::atomic<float> bake_progress_{0.0f};

    ResonanceProbeBatchRegistry probe_batch_registry_;
    ResonanceSceneManager scene_manager_;
    static uint64_t _hash_probe_data(const PackedByteArray& pba);
    static uint64_t _hash_probe_data(const uint8_t* ptr, size_t size);
    static std::atomic<bool> is_shutting_down_flag;
    /// Set for entire _shutdown_steam_audio (including reinit path) so audio thread skips IPL before handles are destroyed.
    std::atomic<bool> ipl_teardown_active_{false};

    /// Requires simulation_mutex. Used during shutdown when is_shutting_down blocks destroy_source_handle.
    void _destroy_source_handle_under_simulation_lock(int32_t handle);
    /// Requires simulation_mutex. Drains pending_source_adds_ / pending_source_removes_ queues:
    /// iplSourceAdd / iplSourceRemove + one batched iplSimulatorCommit when anything changed.
    /// Runs at the start of every worker tick so [method create_source_handle] / [method destroy_source_handle]
    /// can return immediately on the main thread without blocking on simulation_mutex.
    void _drain_pending_source_lifecycle_assume_locked();
    /// Requires simulation_mutex. Applies [member source_update_batch_] before RunDirect/RunReflections.
    void _flush_pending_source_updates_assume_locked();
    /// Requires simulation_mutex.
    void _drain_pathing_probe_batch_releases();
    /// Thread-safe: true when handle was created but the worker hasn't yet run iplSourceAdd on the simulator.
    bool _is_source_attach_pending(int32_t handle) const;
    /// Requires simulation_mutex. Pushes iplSourceGetOutputs into reverb/pathing/occlusion caches so the audio thread
    /// can serve fetch_* without acquiring simulation_mutex (try_lock was always failing during long RunReflections).
    /// refresh_direct_outputs: false skips DIRECT GetOutputs (occlusion cache keeps last values; same as audio try_lock miss).
    /// refresh_reflection_outputs: false skips REFLECTIONS GetOutputs unless RunReflections ran this tick.
    void _worker_sync_fetch_caches(bool refresh_direct_outputs, bool refresh_reflection_outputs);
    /// Shared gate for fetch_reverb_params and worker reflection sync (probe/rays, RunReflections, pending).
    bool _source_reflection_fetch_allowed(int32_t handle, bool reflections_have_run) const;
    uint64_t _worker_fetch_occlusion_into_back(IPLSource src, int32_t handle, int occ_back);
    bool _worker_fetch_reflection_into_back(IPLSource src, int32_t handle, int reverb_back, int refl_back, bool reflections_have_run,
                                            uint64_t& out_microseconds);
    float _static_source_interpolated_baked_energy(const SourceUpdateParams& params, const Vector3& listener_pos) const;
    static bool _pathing_copy_sh_coeffs(std::array<float, kMaxPathingSHCoeffs>& dst, const float* src, int sh_count);

    // Internal Methods
    void _apply_config(Dictionary config);
    void _worker_thread_func();
    void _worker_note_direct_sim_pass_completed();
    /// Steam Audio Run* + cache sync. Caller must hold simulation_mutex. Used by worker thread or main thread (CUSTOM).
    void _run_phonon_simulation_locked(const IPLCoordinateSpace3& current_listener, bool run_direct, bool run_reflection_sim, bool run_pathing_sim);
    /// Main-thread tick: reflection/pathing intervals, adaptive backoff, direct cadence. Returns whether this wake should run RunDirect.
    bool _tick_schedule_simulation(float delta, const std::vector<int32_t>& handles);
    struct ReflectionSourceCounts {
        int32_t active_reflection_sources = 0;
        int32_t active_realtime_sources = 0;
    };
    ReflectionSourceCounts _count_reflection_source_flags(const std::vector<int32_t>& handles) const;
    /// Lock-free: reads per-handle source_outputs_* atomics (safe from tick without simulation_mutex).
    bool _any_source_has_reflection_outputs(const std::vector<int32_t>& handles) const;
    bool _any_source_has_realtime_reflection_outputs(const std::vector<int32_t>& handles) const;
    void _compute_adaptive_eff_num_rays(bool any_realtime_reflections, bool run_reflection_sim, int& eff_num_rays, int& adaptive_target);
    uint64_t _commit_simulator_scene_graph_if_dirty_assume_locked();
    uint64_t _ipl_simulator_commit_assume_locked();
    uint64_t _run_pathing_sim_assume_locked(bool run_pathing_sim);
    IPLCoordinateSpace3 _snapshot_listener_for_simulation();
    /// Seqlock read of `listener_coords_latest_` - all callers see a consistent basis without stealing updates.
    IPLCoordinateSpace3 _read_listener_coords_seqlock() const;
    bool _uses_main_thread_phonon_simulation() const;
    /// IPL scene type for static mesh load / bake temp scenes (never CUSTOM).
    IPLSceneType _tracer_type_for_mesh_operations() const;
    void _init_internal();
    void _init_context_and_devices();
    bool _init_scene_and_simulator();
    void _deferred_refresh_all_geometry_after_scene_load();
    void _start_worker_thread();
    void _shutdown_steam_audio();
    /// Call registered IPL clients while AudioServer is locked (caller must hold AudioServer::lock).
    void _drain_ipl_context_clients_assume_audio_locked();
    /// AudioServer::lock, drain clients, unlock.
    void _drain_ipl_context_clients_before_context_destroy();

    struct IplContextClient {
        void* key = nullptr;
        void (*cleanup)(void*) = nullptr;
    };
    std::mutex ipl_context_clients_mutex_;
    std::vector<IplContextClient> ipl_context_clients_;

    SourceUpdateParams _default_new_source_params() const;
    void _update_source_internal(IPLSource source, int32_t handle, const SourceUpdateParams& params);
    /// Requires simulation_mutex. Clears PATHING inputs on one source (selector includes PATHING).
    void _clear_source_pathing_inputs_assume_locked(IPLSource source, int32_t handle);
    /// Requires simulation_mutex. Clears pathing on sources that may reference removing_handle.
    void _clear_pathing_for_probe_batch_assume_locked(int32_t removing_handle);
    int _apply_source_update_batch(const std::vector<std::pair<int32_t, SourceUpdateParams>>& batch);
    void _maybe_apply_baked_reverb_listener_reflection_inputs(IPLSource src, int32_t handle, const IPLSimulationInputs& inputs,
                                                              const SourceUpdateParams& params, IPLSimulationFlags sim_flags,
                                                              bool enable_reflections);

    int _get_bake_num_rays() const;
    int _get_bake_num_bounces() const;
    int _get_bake_num_threads() const;
    int _get_bake_ambisonics_order() const;
    int _get_bake_reflection_type() const;
    float _get_bake_pathing_param(const char* key, float default_val) const;
    int _get_bake_pathing_num_samples() const;
    /// Isolated bake scene (never the live simulator scene). Caller must `_release_bake_scene_scratch` after bake.
    struct BakeSceneScratch {
        IPLScene scene = nullptr;
        std::vector<IPLStaticMesh> meshes;
        std::vector<IPLScene> sub_scenes;
        std::vector<IPLInstancedMesh> instanced;
        /// Parallel to sub_scenes: StaticMesh retains loaded into each sub-scene.
        std::vector<IPLStaticMesh> meshes_in_sub;
    };
    /// Requires simulation_mutex. Builds a temp IPLScene from bake assets, runtime static meshes, or a live-scene snapshot.
    bool _prepare_bake_scene(BakeSceneScratch& out);
    static void _release_bake_scene_scratch(BakeSceneScratch& scratch);
    bool _has_bake_static_scene_assets() const;
    /// Runs bake_fn on an isolated bake scene (simulation_mutex only held during prepare, not during Phonon bake).
    bool _with_bake_scene(std::function<bool(IPLScene bake_scene)> bake_fn);
    /// Call under simulation_mutex before scene commit: apply queued [code]iplInstancedMeshUpdateTransform[/code] when due.
    /// Returns true if at least one transform was applied to the IPL scene (not when batching defers back to the queue).
    bool _apply_queued_dynamic_instanced_mesh_transforms_assume_locked();
    /// Returns probe batch for pathing: preferred_handle if valid and has pathing, else first with pathing.
    /// IMPORTANT: Return value is retained (iplProbeBatchRetain). Caller MUST call iplProbeBatchRelease when done;
    /// failure to release causes IPL handle leaks.
    IPLProbeBatch _get_pathing_batch_for_source(int32_t preferred_handle);
    /// Clears reverb, reflection, and pathing param caches (call after probe batch changes).
    void _clear_all_param_caches();
    bool _is_batch_compatible_with_config(int32_t handle) const;

    /// True when reflection type uses IR (Convolution, Hybrid, or TAN)
    bool _uses_convolution_or_hybrid_or_tan() const;
    /// True when reflection type uses parametric reverb (Parametric or Hybrid)
    bool _uses_parametric_or_hybrid() const;
    /// True when baked_type is compatible with current reflection_type
    bool _is_reflection_type_compatible(int baked_type) const;

    IPLContext _ctx() const { return steam_audio_context_ ? steam_audio_context_->get_context() : nullptr; }
    IPLEmbreeDevice _embree() const { return steam_audio_context_ ? steam_audio_context_->get_embree_device() : nullptr; }
    IPLOpenCLDevice _opencl() const { return steam_audio_context_ ? steam_audio_context_->get_opencl_device() : nullptr; }
    IPLRadeonRaysDevice _radeon() const { return steam_audio_context_ ? steam_audio_context_->get_radeon_rays_device() : nullptr; }
    IPLTrueAudioNextDevice _tan() const { return steam_audio_context_ ? steam_audio_context_->get_tan_device() : nullptr; }
    IPLHRTF _hrtf() const { return steam_audio_context_ ? steam_audio_context_->get_hrtf() : nullptr; }
    IPLSceneType _scene_type() const { return steam_audio_context_ ? steam_audio_context_->get_scene_type() : IPL_SCENETYPE_DEFAULT; }

  protected:
    static void _bind_methods();

  public:
    ResonanceServer();
    ~ResonanceServer();

    static ResonanceServer* get_singleton();
    /// Called from module uninitialize; ensures clean teardown order before destructor.
    void shutdown();

    // API for GDScript
    void init_audio_engine(Dictionary config);
    /// Re-initialize with new config (e.g. when ResonanceRuntimeConfig overrides toolbar init). Shuts down first.
    void reinit_audio_engine(Dictionary config);

    /// Clients that allocate IPL audio buffers / effects on the current context register for teardown before
    /// iplContextRelease (reinit/shutdown). Cleanup is invoked under AudioServer::lock (see _shutdown_steam_audio).
    using IplContextClientCleanup = void (*)(void* userdata);
    void register_ipl_context_client(void* key, IplContextClientCleanup cleanup);
    void unregister_ipl_context_client(void* key);

    // Getters
    IPLContext get_context_handle() const { return steam_audio_context_ ? steam_audio_context_->get_context() : nullptr; }
    IPLScene get_scene_handle() const { return scene; }
    IPLSimulator get_simulator_handle() const { return simulator; }
    IPLEmbreeDevice get_embree_device_handle() const { return steam_audio_context_ ? steam_audio_context_->get_embree_device() : nullptr; }
    IPLOpenCLDevice get_opencl_device_handle() const { return steam_audio_context_ ? steam_audio_context_->get_opencl_device() : nullptr; }
    IPLRadeonRaysDevice get_radeon_rays_device_handle() const { return steam_audio_context_ ? steam_audio_context_->get_radeon_rays_device() : nullptr; }
    IPLHRTF get_hrtf_handle() const { return steam_audio_context_ ? steam_audio_context_->get_hrtf() : nullptr; }
    IPLReflectionMixer get_reflection_mixer_handle() const;

    void fill_reflection_mixer_apply_params(IPLReflectionEffectParams* out_params) const;
    IPLCoordinateSpace3 get_current_listener_coords();
    /// FMOD Bridge: Handle for reverb source (listener position). -1 if not created.
    int32_t get_fmod_reverb_source_handle() const { return fmod_reverb_source_handle_; }
    /// FMOD Bridge: lazily creates the listener reverb IPL source for iplFMODSetReverbSource. No-op if already created.
    void ensure_fmod_reverb_source();
    /// FMOD Bridge: Pointer to simulation settings (valid while server initialized). For C++ bridge use.
    const IPLSimulationSettings* get_simulation_settings_for_fmod() const { return _ctx() ? &simulation_settings : nullptr; }
    IPLSceneType get_scene_type() const { return steam_audio_context_ ? steam_audio_context_->get_scene_type() : IPL_SCENETYPE_DEFAULT; }
    /// Scene type for iplStaticMeshLoad / sub-scenes / bake temp scenes. When runtime uses CUSTOM, returns Default (meshes are not added to the global CUSTOM scene).
    IPLSceneType get_phonon_mesh_scene_type() const { return _tracer_type_for_mesh_operations(); }
    /// Queue dynamic instanced-mesh transform for worker (no simulation_mutex on main thread). Coalesces to latest matrix per mesh.
    void enqueue_dynamic_instanced_mesh_transform(IPLInstancedMesh mesh, const IPLMatrix4x4& transform);
    /// Every Nth call returns true (shared counter with transform-only notify_geometry_changed); interval matches [code]kGeometryTransformCoalesceInterval[/code]. Does not apply to [code]triangle_delta != 0[/code] notifies or unconditional enqueue from flush.
    bool consume_geometry_transform_coalesce_tick();
    /// Remove pending transforms for mesh (e.g. before releasing instanced mesh).
    void cancel_pending_dynamic_instanced_mesh_transform(IPLInstancedMesh mesh);

    /// Godot World3D for physics raycasts when scene_type is Custom. Call from main thread (e.g. each frame from ResonanceRuntime).
    void set_physics_world(const Ref<World3D>& world);
    /// User RID excludes for Custom-scene ray queries. Merged each tick with listener and auto source excludes before applying to the bridge.
    void set_physics_ray_exclude_rids(const TypedArray<RID>& exclude);
    /// Listener-side CollisionObject3D RIDs (camera parent chain, [code]resonance_listener[/code] group, etc.). Replaces the previous listener list; merged with user + auto excludes.
    void set_listener_physics_ray_exclude_rids(const TypedArray<RID>& rids);
    /// Register a CollisionObject3D RID to ignore in Custom-scene rays (refcounted; used by ResonancePlayer for descendant colliders).
    void register_physics_ray_auto_exclude_rid(RID rid);
    void unregister_physics_ray_auto_exclude_rid(RID rid);

    // Thread Safety
    /// Lock-free RAII guard for reading the shared reflection mixer in audio callbacks.
    MixerReadGuard scoped_mixer_read() const { return MixerReadGuard(this); }
    /// Manual lock; prefer scoped_simulation_lock() for RAII. Main/worker only - never from the audio thread (blocks on RunReflections).
    void lock_simulation() { simulation_mutex.lock(); }
    void unlock_simulation() { simulation_mutex.unlock(); }
    /// RAII guard for simulation mutex; prefer scoped_simulation_lock() over lock_simulation/unlock_simulation for exception safety
    std::unique_lock<std::mutex> scoped_simulation_lock() { return std::unique_lock<std::mutex>(simulation_mutex); }
    // Status
    String get_version();
    bool is_initialized() const;
    bool is_simulating() const;
    /// When false, ResonancePlayer/Ambisonic spatial output is silent (warmup after scene changes or runtime start).
    bool is_spatial_audio_output_ready() const;
    /// Restart warmup counter (e.g. after loading static geometry). Call from main thread.
    void reset_spatial_audio_warmup_passes();
    int get_sample_rate() const { return current_sample_rate; }
    int get_audio_frame_size() const { return frame_size; }
    /// Channel count for direct-path speaker panning when not using HRTF (Steam Audio standard layouts).
    int get_direct_speaker_channels() const { return direct_speaker_channels; }
    int get_ambisonic_order() const { return ambisonic_order; }
    /// Request reinit with detected Godot frame_count (from reverb bus). Call from audio thread. Only applies when Auto was used.
    void request_reinit_with_frame_size(int detected_frame_count);
    /// Get and clear pending reinit frame size. Returns 0 if none. Call from main thread.
    int consume_pending_reinit_frame_size();
    bool get_audio_frame_size_was_auto() const { return audio_frame_size_was_auto_.load(std::memory_order_acquire); }
    /// Reverb Bus debugging: mixer feeds, effect process stats, output levels.
    Dictionary get_reverb_bus_instrumentation() const;
    /// Reset reverb bus instrumentation counters for crackling debug. Call to clear and re-observe.
    void reset_reverb_bus_instrumentation();
    /// Pathing pipeline counters: worker RunPathing, fetch_pathing_params, player mix gate. See get_pathing_instrumentation().
    Dictionary get_pathing_instrumentation() const;
    void reset_pathing_instrumentation();
    /// Last worker tick: microseconds in RunDirect / RunReflections / RunPathing / _worker_sync_fetch_caches / commit (profiling).
    /// If us_run_reflections stays large after Nexus integration tweaks, the bottleneck is inside Phonon, not this wrapper.
    Dictionary get_simulation_worker_timing() const;
    /// Lock-free approximation of the currently registered [code]IPLSource[/code] count (one per active
    /// [code]ResonanceSource[/code] / [code]ResonancePlayer[/code] with a Resonance config). Safe to poll
    /// per frame from Performance monitors.
    int32_t get_active_source_count() const;
    /// Lock-free approximation of the currently registered [code]IPLProbeBatch[/code] count (one per
    /// loaded [code]ResonanceProbeVolume[/code] bake). Safe to poll per frame from Performance monitors.
    int32_t get_active_probe_batch_count() const;
    /// Convolution: last audio-thread timings (microseconds). Compare to [method get_simulation_worker_timing] to split worker vs audio cost.
    Dictionary get_convolution_audio_timing() const;
    void record_convolution_reflection_apply_usec(uint64_t us);
    void record_convolution_reverb_bus_usec(uint64_t us);
    /// Reverb bus mixer path: sanitize passes inside ResonanceMixerProcessor (profiling only).
    void record_mixer_sanitize_ambi_usec(uint64_t us);
    void record_mixer_sanitize_stereo_usec(uint64_t us);
    /// For profiling: configured scene_type, effective rayBatchSize, whether Custom uses batched Godot trace callbacks (Phonon BatchedReflectionSimulator when batch > 1).
    Dictionary get_simulation_tracer_profile() const;
    void record_pathing_player_gate_enter();
    void record_pathing_player_applied();
    void record_pathing_player_fetch_miss();
    /// Called by ResonancePlayer when it feeds the reflection mixer (Convolution only).
    void record_mixer_feed() { reverb_mixer_feed_count.fetch_add(1, std::memory_order_relaxed); }
    /// Audio-thread safe: monotonic counter of mixer feeds.
    uint64_t get_mixer_feed_count() const { return reverb_mixer_feed_count.load(std::memory_order_relaxed); }
    /// Call when feeding mixer for convolution; ir_non_null, reverb_gain, input_rms (mono before gain)
    void record_convolution_feed(bool ir_non_null, float reverb_gain, float input_rms);
    /// Called by ResonanceAudioEffectInstance each _process. `*_pre_gain` are measured right after mixer apply + decode,
    /// before `ResonanceAudioEffect.gain_db` is applied. `*_post_gain` are after gain + clip.
    void update_reverb_effect_instrumentation(bool mixer_null, bool success, int32_t frames_written,
                                              float output_peak_post_gain, float output_rms_post_gain,
                                              float output_peak_pre_gain, float output_rms_pre_gain);
    /// Lightweight accessor for debug overlays: decoded reverb bus output RMS before bus gain_db.
    /// Note: This is a global bus metric (not per-source).
    float get_reverb_bus_output_rms_pre_gain() const { return reverb_effect_output_rms_pre_gain.load(std::memory_order_relaxed); }
    /// Wet-tail debug: ResonanceAudioEffect silent-threshold duck fired this callback.
    void record_reverb_effect_click_guard_trigger();
    /// Wet-tail debug: process_mixer_return repeated last decoded block.
    void record_mixer_return_hold_last();
    /// Enable/disable bus click guard (linear block duck on peak collapse). Default true; set false to A/B tail pumping.
    void set_reverb_bus_click_guard_enabled(bool p_enabled);
    bool is_reverb_bus_click_guard_enabled() const;
    void set_reverb_bus_wet_ring_underrun_zero_fill(bool p_enabled);
    bool is_reverb_bus_wet_ring_underrun_zero_fill() const;
    float get_max_reverb_duration() const { return max_reverb_duration; }
    /// 0 = no cap. When > 0, convolution apply clamps IR length to min(this, effect allocation).
    int get_convolution_ir_max_samples() const { return convolution_ir_max_samples_; }
    int get_num_channels_for_order() const { return (ambisonic_order + 1) * (ambisonic_order + 1); }
    static bool is_shutting_down() { return is_shutting_down_flag.load(std::memory_order_acquire); }
    /// True during process exit or while Steam Audio is being torn down for reinit - audio-thread IPL callbacks must bail out.
    static bool ipl_audio_teardown_active();

    // IO / Baking
    /// Set bake params from ResonanceRuntimeConfig.get_bake_params(). Call before baking when not using ProjectSettings.
    void set_bake_params(Dictionary params);
    /// Set static scene asset for bake (from ResonanceStaticScene). When set, bake uses this instead of live geometry.
    /// Convenience wrapper for a single pack at identity; prefer [method set_bake_static_scenes_from_assets] for multi-pack levels.
    void set_bake_static_scene_asset(const Ref<ResonanceGeometryAsset>& p_asset);
    /// Set all static scene packs for bake (assets + world transforms), matching runtime additive load.
    void set_bake_static_scenes_from_assets(const TypedArray<ResonanceGeometryAsset>& assets,
                                            const TypedArray<Transform3D>& transforms);
    /// Load static scene from asset into the server's scene (runtime). Replaces any existing static packs.
    void load_static_scene_from_asset(const Ref<ResonanceGeometryAsset>& p_asset, const Transform3D& p_transform = Transform3D());
    /// Add static scene from asset (additive). Prefer [method add_or_replace_static_pack] when tied to a ResonanceStaticScene node.
    void add_static_scene_from_asset(const Ref<ResonanceGeometryAsset>& p_asset, const Transform3D& p_transform = Transform3D());
    /// Incremental: add or replace one pack keyed by ResonanceStaticScene ObjectID (always InstancedMesh).
    void add_or_replace_static_pack(uint64_t object_id, const Ref<ResonanceGeometryAsset>& p_asset, const Transform3D& p_transform);
    /// Incremental: remove one pack by ResonanceStaticScene ObjectID.
    void remove_static_pack(uint64_t object_id);
    /// Clear all loaded static scene packs (e.g. before full replace).
    void clear_static_scenes();
    /// Clear then add all assets under one simulation_mutex. Empty arrays clear only (no stale meshes).
    void replace_static_scenes_from_assets(const TypedArray<ResonanceGeometryAsset>& assets,
                                           const TypedArray<Transform3D>& transforms);
    /// Hint for bake log: pathing will run after reflections in this bake pipeline. Call before bake_probes.
    void set_bake_pipeline_pathing(bool p_pathing);
    void save_scene_data(String filename);
    void load_scene_data(String filename);
    /// Walks the active SceneTree and calls ResonanceGeometry::refresh_geometry on each geometry node. Main thread; use after load_scene_data replaces the Phonon scene so instanced meshes and handles are rebuilt.
    void refresh_all_geometry_from_scene_tree();
    /// Export static ResonanceGeometry (dynamic=false) under scene_root to merged asset. Returns OK on success. Standalone; no server required.
    Error export_static_scene_to_asset(Node* scene_root, const String& p_path);
    /// Export static ResonanceGeometry from scene to OBJ+MTL (Editor or Runtime). Path without extension.
    Error export_static_scene_to_obj(Node* scene_root, const String& file_base_name);
    /// Hash of static geometry for change detection. Same input as export; use to skip re-export when unchanged.
    int64_t get_static_scene_hash(Node* scene_root);
    /// Hash of geometry asset data; use to detect when static scene changed and probe bake must re-run.
    int64_t get_geometry_asset_hash(const Ref<ResonanceGeometryAsset>& p_asset) const;
    /// Export scene geometry to OBJ+MTL for debug (iplSceneSaveOBJ). Pass path without extension, e.g. "res://debug/scene".
    void save_scene_obj(String file_base_name);
    PackedVector3Array generate_manual_grid(const Transform3D& volume_transform, Vector3 extents, float spacing,
                                            int generation_type = 2, float height_above_floor = 1.5f);
    /// Scene-aware probe placement (Steam Audio iplProbeArrayGenerateProbes). For GEN_UNIFORM_FLOOR/GEN_CENTROID.
    /// Returns empty if no scene/0 probes; caller should fall back to generate_manual_grid.
    PackedVector3Array generate_probes_scene_aware(const Transform3D& volume_transform, Vector3 extents, float spacing,
                                                   int generation_type, float height_above_floor);
    bool bake_manual_grid(const PackedVector3Array& points, Ref<ResonanceProbeData> probe_data_res);
    /// Bake reflections: generate candidates, apply [param exclusion_boxes], then bake_manual_grid.
    /// [param exclusion_boxes]: Array of Dictionary { xform: Transform3D, size: Vector3 }.
    bool bake_probes_for_volume(const Transform3D& volume_transform, Vector3 extents, float spacing,
                                int generation_type, float height_above_floor, Ref<ResonanceProbeData> probe_data_res,
                                const Array& exclusion_boxes = Array());
    bool bake_pathing(Ref<ResonanceProbeData> probe_data_res);
    bool bake_static_source(Ref<ResonanceProbeData> probe_data_res, Vector3 endpoint_position, float influence_radius);
    bool bake_static_listener(Ref<ResonanceProbeData> probe_data_res, Vector3 endpoint_position, float influence_radius);
    /// Stores progress atomically (safe from bake thread). Does not emit Godot signals.
    void emit_bake_progress(float progress);
    /// Main-thread / UI poll of last bake progress in [0, 1].
    float get_bake_progress() const;
    /// Cancel a reflections bake in progress. Call from another thread (e.g. main) while bake runs in a worker thread.
    void cancel_reflections_bake();
    /// Cancel a pathing bake in progress. Call from another thread (e.g. main) while bake runs in a worker thread.
    void cancel_pathing_bake();
    /// Loads probe data and adds to simulator. Returns handle >= 0 on success, -1 on failure. Call remove_probe_batch when volume exits.
    int32_t load_probe_batch(Ref<ResonanceProbeData> probe_data_res);
    /// Removes a specific probe batch by handle (call from ResonanceProbeVolume._exit_tree).
    void remove_probe_batch(int32_t handle);
    /// Clears all loaded probe batches (e.g. on plugin disable).
    void clear_probe_batches();
    /// Removes probe batches incompatible with current reflection_type/pathing_enabled. Returns count removed.
    int revalidate_probe_batches_with_config();

    /// Editor/tools: probe count in serialized [param data], or -1 if context/data invalid.
    int32_t editor_probe_data_get_num_probes(Ref<ResonanceProbeData> data) const;
    /// Editor/tools: remove probe by index; updates [param data] in memory. Reload probe batch on volumes using this resource.
    bool editor_probe_data_remove_probe(Ref<ResonanceProbeData> data, int32_t index);
    /// Editor/tools: remove a baked layer (reflections/pathing). See ResonanceBaker::probe_data_remove_baked_data_layer.
    bool editor_probe_data_remove_baked_layer(Ref<ResonanceProbeData> data, int baked_data_type, int variation, Vector3 endpoint,
                                              float influence_radius);

    // Runtime Control
    void set_debug_occlusion(bool p_enabled);
    bool is_debug_occlusion_enabled() const;
    void set_debug_reflections(bool p_enabled);
    bool is_debug_reflections_enabled() const;
    void set_debug_pathing(bool p_enabled);
    bool is_debug_pathing_enabled() const;
    /// Returns array of {from: Vector3, to: Vector3, occluded: bool} for path visualization (only populated when debug_pathing enabled)
    Array get_pathing_visualization_segments();
    /// Returns array of {from: Vector3, to: Vector3, bounce: int} for reflection ray debug (only when debug_reflections + realtime_rays)
    Array get_ray_debug_segments();
    /// Same as get_ray_debug_segments but uses the given origin instead of listener_coords_. Use for viz to avoid 1-frame delay.
    Array get_ray_debug_segments_at(Vector3 origin);
    bool uses_custom_ray_tracer() const;
    bool wants_debug_reflection_viz() const { return debug_reflections.load(std::memory_order_relaxed) && max_rays > 0; }
    int register_debug_mesh(const std::vector<IPLVector3>& vertices, const std::vector<IPLTriangle>& triangles,
                            const IPLint32* material_indices, const IPLMatrix4x4* transform, const IPLMaterial* material);
    void unregister_debug_mesh(int mesh_id);
    void set_output_direct_enabled(bool p_enabled);
    bool is_output_direct_enabled() const;
    void set_output_reverb_enabled(bool p_enabled);
    bool is_output_reverb_enabled() const;
    void set_reverb_influence_radius(float p_radius);
    float get_reverb_influence_radius() const;
    void set_reverb_transmission_amount(float p_amount);
    float get_reverb_transmission_amount() const;
    void set_apply_occlusion_to_baked_reflections(bool p_enabled);
    bool get_apply_occlusion_to_baked_reflections() const;
    void set_baked_reverb_use_listener_probe(bool p_enabled);
    bool get_baked_reverb_use_listener_probe() const;
    void set_perspective_correction_enabled(bool p_enabled);
    bool is_perspective_correction_enabled() const;
    void set_perspective_correction_factor(float p_factor);
    float get_perspective_correction_factor() const;
    int get_reflection_type() const { return reflection_type; }
    int get_realtime_rays() const { return max_rays; }
    int get_default_reflections_mode() const { return default_reflections_mode; }
    int get_transmission_type() const { return transmission_type; }
    int get_max_transmission_surfaces() const { return max_transmission_surfaces; }
    int get_occlusion_type() const { return occlusion_type; }
    bool use_direct_binaural() const;
    /// Reflection mixer / Ambisonics decode to stereo (wet path).
    bool use_reverb_binaural() const;
    bool use_pathing_binaural() const;
    bool use_virtual_surround_output() const { return use_virtual_surround; }
    bool get_hrtf_interpolation_bilinear() const { return hrtf_interpolation_bilinear; }
    bool is_pathing_enabled() const { return pathing_enabled; }
    /// Runtime defaults when ResonancePlayerConfig uses Use Global (-1).
    bool get_default_path_validation_enabled() const { return path_validation_enabled; }
    bool get_default_find_alternate_paths() const { return find_alternate_paths; }
    /// Set custom pathing deviation model (C++ only). For default/UTD pass nullptr. Call clear_pathing_deviation_callback() to reset.
    void set_pathing_deviation_callback(IPLDeviationCallback callback, void* userData);
    void clear_pathing_deviation_callback();
    void set_pathing_enabled(bool p_enabled);
    /// True when RunPathing ran this tick; false when skipped (listener invalid / cooldown). Uncaught fault in Phonon still terminates the process.
    bool did_pathing_run_this_tick() const { return pathing_ran_this_tick.load(); }

    // Calculations
    float calculate_distance_attenuation(Vector3 source_pos, Vector3 listener_pos, float min_dist, float max_dist);
    Vector3 calculate_air_absorption(Vector3 source_pos, Vector3 listener_pos);
    float calculate_directivity(Vector3 source_pos, Vector3 source_forward, Vector3 source_up, Vector3 source_right, Vector3 listener_pos, float weight, float power);

    // Updates
    void notify_geometry_changed(int triangle_delta);
    /// Same as notify_geometry_changed but caller already holds simulation_mutex (e.g. _clear_meshes_impl).
    void notify_geometry_changed_assume_locked(int triangle_delta);
    /// Mark that iplSceneCommit is required before the next simulation step. Caller must hold simulation_mutex.
    /// Used for dynamic instanced-mesh transform updates (no triangle count change).
    void mark_scene_commit_pending_assume_locked();
    /// Thread-safe variant: flips the atomic scene_dirty flag without requiring simulation_mutex.
    /// Used by main-thread dynamic geometry flush paths that intentionally avoid blocking on the worker tick.
    void mark_scene_commit_pending();
    void update_listener(Vector3 pos, Vector3 dir, Vector3 up);
    void set_listener_valid(bool valid);
    /// Notify that the audio listener has changed. Call when listener is created or swapped (e.g. Splitscreen, VR).
    /// No-op when using ResonanceRuntime's default camera-based listener; use when you update listener manually.
    void notify_listener_changed();
    /// Set listener to a specific Node3D's transform. Call each frame or when the node moves.
    void notify_listener_changed_to(Node* listener_node);
    void tick(float delta);
    /// Default path: coalesce into batch; ResonanceRuntime calls flush_pending_source_updates each frame.
    void update_source(int32_t handle, const SourceUpdateParams& params);
    /// Immediate apply when simulation_mutex is free; false if worker holds the lock.
    bool try_update_source(int32_t handle, const SourceUpdateParams& params);
    /// Queue for flush_pending_source_updates (one simulation_mutex lock per frame).
    void enqueue_source_update(int32_t handle, const SourceUpdateParams& params);
    /// Apply all [method enqueue_source_update] entries (try_lock; re-queues on failure). Worker also drains the batch at sim tick start.
    void flush_pending_source_updates();
    bool uses_batch_source_updates() const { return batch_source_updates; }
    /// Set attenuation callback data for Linear/Curve modes. Call before update_source when attenuation_mode is 1 or 2.
    void set_source_attenuation_callback_data(int32_t handle, int attenuation_mode, float min_distance, float max_distance, const PackedFloat32Array& curve_samples);
    /// Clear attenuation callback data when switching to Inverse mode.
    void clear_source_attenuation_callback_data(int32_t handle);
    /// Per-source override for [member baked_reverb_use_listener_probe]. -1 = use global flag, 0 = disabled, 1 = enabled.
    /// Cleared automatically on [method destroy_source_handle].
    void set_source_baked_reverb_use_listener_probe_override(int32_t handle, int override_value);

    // Handles
    int32_t create_source_handle(Vector3 position, float radius);
    void destroy_source_handle(int32_t handle);
    IPLSource get_source_from_handle(int32_t handle);
    /// Epoch for source/probe-batch handle validity across reinit (see resonance_source_handle_policy.h).
    uint32_t get_source_lifecycle_epoch() const { return source_lifecycle_epoch_.load(std::memory_order_acquire); }
    /// Same counter as [method get_source_lifecycle_epoch]; probe batches recycle in the same shutdown.
    uint32_t get_probe_batch_lifecycle_epoch() const { return get_source_lifecycle_epoch(); }

    // Data Fetch
    OcclusionData get_source_occlusion_data(int32_t handle);
    /// Lock-free linear gain from occlusion cache (no Dictionary marshaling). For bridges on the main thread.
    float get_source_occlusion_linear_gain(int32_t handle);
    /// GDScript-facing occlusion readback (Dictionary with occlusion, transmission, etc.).
    Dictionary get_source_occlusion_data_dict(int32_t handle);
    /// Position/radius-only [method update_source] path for external emitters (FMOD, Coda).
    void update_source_position(int32_t handle, Vector3 position, float radius,
                                bool use_sim_distance_attenuation = false, float min_distance = 1.0f);
    /// True if the last worker reflection sync (or a successful locked fetch_reverb_params) had usable reflection outputs for this handle.
    /// Does not acquire simulation_mutex; may be false briefly after source create until the next sync. Use fetch_reverb_params when params are required.
    bool peek_reverb_params_likely_available(int32_t handle) const;
    bool fetch_reverb_params(int32_t handle, IPLReflectionEffectParams& out_params);
    bool fetch_pathing_params(int32_t handle, IPLPathEffectParams& out_params);
    uint16_t get_reflection_baked_energy_q16(int32_t handle) const;
    float probe_data_static_source_energy_at(Ref<ResonanceProbeData> data, Vector3 endpoint, Vector3 listener, float influence_radius,
                                             float neighbor_radius = 0.0f);
};

} // namespace godot

#endif