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
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <memory>
#include <mutex>
#include <phonon.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "handle_manager.h"
#include "ray_trace_debug_context.h"
#include "resonance_baker.h"
#include "resonance_constants.h"
#include "resonance_geometry_asset.h"
#include "resonance_godot_physics_scene_bridge.h"
#include "resonance_ipl_resource_pool_backend.h"
#include "resonance_listener_reverb_policy.h"
#include "resonance_pathing_deviation_policy.h"
#include "resonance_pathing_fetch_policy.h"
#include "resonance_pathing_skip_policy.h"
#include "resonance_probe_batch_registry.h"
#include "resonance_probe_data.h"
#include "resonance_processor_reflection.h"
#include "resonance_reflection_mixer_policy.h"
#include "resonance_reflection_skip_policy.h"
#include "resonance_reflections_adaptive_budget_policy.h"
#include "resonance_reflections_age_policy.h"
#include "resonance_reflections_pass_gate.h"
#include "resonance_scene_manager.h"
#include "resonance_server_config.h"
#include "resonance_sofa_asset.h"
#include "resonance_source_simulation_table.h"
#include "resonance_source_update_policy.h"
#include "resonance_spatial_output_gate.h"
#include "resonance_steam_audio_context.h"
#include "resonance_utils.h"
#include "resonance_writer_preferring_shared_mutex.h"

#include <godot_cpp/classes/world3d.hpp>

namespace godot {

class ResonanceGeometry;

struct OcclusionData {
    float occlusion;
    float transmission[3];
    float air_absorption[3];    // From simulation when IPL_DIRECTSIMULATIONFLAGS_AIRABSORPTION enabled
    float directivity;          // From simulation when IPL_DIRECTSIMULATIONFLAGS_DIRECTIVITY enabled
    float distance_attenuation; // From simulation when distanceAttenuationModel set
    /// Transmission of the first surface between listener and source.
    float hit0_transmission[3] = {1.0f, 1.0f, 1.0f};
    bool hit0_valid = false;
    int num_transmission_rays = 1;
};

/// params.shCoeffs points into this struct's own shCoeffs, never into the server cache.
struct FetchedPathingParams {
    IPLPathEffectParams params{};
    std::array<float, resonance::kPathingFetchedShCoeffs> shCoeffs{};
};

class ResonanceServer : public Object {
    GDCLASS(ResonanceServer, Object)

  public:
    struct AttenuationCallbackData {
        int mode = 0; // 1 = linear, 2 = curve; other values get no callback
        float min_distance = 1.0f;
        float max_distance = 500.0f;
        float curve_samples[resonance::kAttenuationCurveSamples] = {}; // Attenuation over distance mapped 0..1 from min to max
        int num_curve_samples = resonance::kAttenuationCurveSamples;
    };

    /// Userdata for the Steam Audio callback, which runs on simulation threads and reads data under mutex.
    struct AttenuationCallbackContext {
        std::recursive_mutex* mutex = nullptr;
        const AttenuationCallbackData* data = nullptr;
    };

    struct SourceUpdateParams {
        Vector3 position{};
        float radius = 1.0f;
        Vector3 source_forward{0, 0, -1};
        Vector3 source_up{0, 1, 0};
        float directivity_weight = 0.0f;
        float directivity_power = 1.0f;
        bool air_absorption_enabled = true;
        /// Sets the direct DISTANCEATTENUATION flag. Reflections and pathing follow attenuation_mode instead.
        bool use_sim_distance_attenuation = false;
        float min_distance = 1.0f;
        /// False disables Steam Audio distance attenuation for direct, reflections and pathing.
        bool distance_attenuation = true;
        /// ResonancePlayerConfig.attenuation_mode (0 Inverse, 1 Linear, 2 Curve, 3 Disabled).
        int attenuation_mode = 0;
        /// With Linear or Curve, reflections use the attenuation callback too.
        bool use_distance_curve_for_reflections = false;
        bool path_validation_enabled = false;
        bool find_alternate_paths = false;
        int occlusion_samples = 64;
        int num_transmission_rays = resonance::kDefaultTransmissionRays;
        int baked_data_variation = 0;
        Vector3 baked_endpoint_center{};
        float baked_endpoint_radius = 0.0f;
        int32_t pathing_probe_batch_handle = -1;
        int reflections_enabled_override = -1;
        int pathing_enabled_override = -1;
        /// -1 = server occlusion_type, 0 = raycast, 1 = volumetric.
        int occlusion_type_override = -1;
        bool simulation_occlusion_enabled = true;
        bool simulation_transmission_enabled = true;
        float direct_mix_level = 1.0f;
        float reflections_mix_level = 1.0f;
        float pathing_mix_level = 1.0f;
        /// False while the owner is silent: the source stays added but gets no direct, reflection or pathing work.
        bool simulation_active = true;
    };

  private:
    struct ReflectionsPassRequest {
        IPLCoordinateSpace3 listener{};
        bool run_reflections = false;
        bool run_pathing = false;
        bool any() const { return run_reflections || run_pathing; }
    };

    struct AttenuationEntry {
        std::unique_ptr<AttenuationCallbackData> data;
        AttenuationCallbackContext ctx{};
        bool curve_dirty = false;
    };

    // Owns the IPL context, Embree, OpenCL, Radeon Rays, TAN and HRTF.
    std::unique_ptr<ResonanceSteamAudioContext> steam_audio_context_;
    ResonanceGodotPhysicsSceneBridge godot_physics_bridge_;
    /// Custom-scene ray excludes: user RIDs, listener RIDs and auto-registered CollisionObject3D RIDs.
    std::mutex physics_ray_excludes_mutex_;
    TypedArray<RID> physics_ray_exclude_rids_user_;
    TypedArray<RID> listener_physics_ray_exclude_rids_;
    std::unordered_map<int64_t, int> physics_ray_auto_exclude_refcount_;
    std::vector<RID> physics_ray_auto_exclude_active_;
    void _rebuild_and_apply_physics_ray_excludes_unlocked();
    void _clear_physics_ray_excludes_state();
    IPLScene scene = nullptr;
    /// One instanced sub-scene per ResonanceStaticScene ObjectID.
    struct RuntimeStaticPack {
        IPLScene sub_scene = nullptr;
        IPLStaticMesh mesh_in_sub = nullptr;
        IPLInstancedMesh instanced = nullptr;
        IPLMatrix4x4 transform{};
        ObjectID asset_id;
        Transform3D godot_transform;
        int debug_id = -1;
        int tri_count = 0;
    };
    std::unordered_map<uint64_t, RuntimeStaticPack> _runtime_static_packs;
    uint64_t _next_ephemeral_static_pack_id = 1; // For APIs without a Node ObjectID
    int _runtime_static_triangle_count = 0;
    void _release_static_pack_assume_locked(RuntimeStaticPack& pack);
    void _clear_static_packs_assume_locked();
    bool _create_instanced_static_pack_assume_locked(const Ref<ResonanceGeometryAsset>& asset, const Transform3D& transform,
                                                     RuntimeStaticPack& out);
    /// A null asset removes the pack.
    struct StaticPackChange {
        Ref<ResonanceGeometryAsset> asset;
        Transform3D transform;
    };
    std::mutex static_pack_queue_mutex_;
    /// Latest change per pack id; applied by the worker between passes so the main thread never waits
    /// for a reflections pass.
    std::unordered_map<uint64_t, StaticPackChange> static_pack_queue_;
    void _queue_static_pack_change(uint64_t object_id, StaticPackChange change);
    bool _has_pending_static_pack_changes();
    void _apply_pending_static_pack_changes_assume_locked();
    void _apply_static_pack_change_assume_locked(uint64_t object_id, const StaticPackChange& change);
    std::unordered_map<int32_t, std::unique_ptr<AttenuationEntry>> _source_attenuation_entries;
    std::recursive_mutex _attenuation_callback_mutex;
    IPLSimulator simulator = nullptr;

    // The audio thread reads the mixer through MixerReadGuard (atomic reader count, no mutex). Main and worker
    // threads swap or release it only once no reader holds it.
    std::atomic<IPLReflectionMixer> reflection_mixer_{nullptr};
    mutable std::atomic<int> reflection_mixer_readers_{0};
    /// Releases deferred because a reader still held the mixer after the wait timeout.
    mutable std::atomic<uint64_t> reflection_mixer_release_deferred_{0};
    mutable std::atomic<uint64_t> reflection_mixer_deferred_enqueued_{0};
    mutable std::atomic<uint64_t> reflection_mixer_deferred_retired_{0};
    mutable std::atomic<uint64_t> reflection_mixer_deferred_overflow_{0};
    mutable std::atomic<uint64_t> reflection_mixer_deferred_soft_cap_exceeded_{0};
    /// Set by the audio thread when the last reader drops; main or worker thread then drains the deferred releases.
    mutable std::atomic<bool> reflection_mixer_deferred_release_pending_{false};
    mutable std::vector<IPLReflectionMixer> deferred_reflection_mixer_releases_;
    mutable std::mutex deferred_reflection_mixer_releases_mutex_;

    struct MixerReadGuard {
        const ResonanceServer* srv = nullptr;
        IPLReflectionMixer mixer = nullptr;
        bool holds_reader_count = false;

        MixerReadGuard() = default;
        explicit MixerReadGuard(const ResonanceServer* p_srv) : srv(p_srv) {
            if (!srv)
                return;
            const bool count_reader = !srv->ipl_teardown_active_.load(std::memory_order_acquire);
            if (count_reader) {
                srv->reflection_mixer_readers_.fetch_add(1, std::memory_order_acq_rel);
                holds_reader_count = true;
            }
            mixer = srv->reflection_mixer_.load(std::memory_order_acquire);
        }
        MixerReadGuard(const MixerReadGuard&) = delete;
        MixerReadGuard& operator=(const MixerReadGuard&) = delete;
        MixerReadGuard(MixerReadGuard&& other) noexcept {
            srv = other.srv;
            mixer = other.mixer;
            holds_reader_count = other.holds_reader_count;
            other.srv = nullptr;
            other.mixer = nullptr;
            other.holds_reader_count = false;
        }
        MixerReadGuard& operator=(MixerReadGuard&& other) noexcept {
            if (this == &other)
                return *this;
            release();
            srv = other.srv;
            mixer = other.mixer;
            holds_reader_count = other.holds_reader_count;
            other.srv = nullptr;
            other.mixer = nullptr;
            other.holds_reader_count = false;
            return *this;
        }
        ~MixerReadGuard() { release(); }

        void release() {
            if (!srv)
                return;
            if (holds_reader_count) {
                const int readers_before_sub =
                    srv->reflection_mixer_readers_.fetch_sub(1, std::memory_order_acq_rel);
                if (resonance::reflection_mixer_should_request_deferred_drain(readers_before_sub))
                    srv->reflection_mixer_deferred_release_pending_.store(true, std::memory_order_release);
            }
            srv = nullptr;
            mixer = nullptr;
            holds_reader_count = false;
        }
        IPLReflectionMixer get() const { return mixer; }
        explicit operator bool() const { return mixer != nullptr; }
    };

    void _set_reflection_mixer(IPLReflectionMixer new_mixer);
    void _release_reflection_mixer_when_unused(IPLReflectionMixer mixer) const;
    void _try_release_deferred_reflection_mixers() const;
    void _drain_deferred_reflection_mixers_if_pending() const;
    void _defer_reflection_mixer_release(IPLReflectionMixer mixer) const;
    void _flush_deferred_reflection_mixers_on_shutdown() const;

    // Reverb bus counters: written on the audio thread, read on the main thread.
    std::atomic<uint64_t> reverb_effect_process_calls{0};
    std::atomic<uint64_t> reverb_effect_mixer_null{0};
    std::atomic<uint64_t> reverb_effect_success{0};
    std::atomic<uint64_t> reverb_effect_frames_written{0};
    std::atomic<float> reverb_effect_output_peak{0.0f};
    std::atomic<float> reverb_effect_output_rms{0.0f};
    /// Before ResonanceAudioEffect.gain_db.
    std::atomic<float> reverb_effect_output_peak_pre_gain{0.0f};
    std::atomic<float> reverb_effect_output_rms_pre_gain{0.0f};
    /// Times the ResonanceAudioEffect click guard ducked a collapsing peak.
    std::atomic<uint64_t> reverb_effect_click_guard_triggers{0};
    /// Bus ticks that repeated the last stereo block because the mixer got no new feed.
    std::atomic<uint64_t> reverb_mixer_return_hold_last_count{0};
    std::atomic<bool> reverb_bus_click_guard_enabled_{true};
    /// Debug switch: split-reverb underrun outputs zeros instead of a held, cosine-tapered sample.
    std::atomic<bool> reverb_bus_wet_ring_underrun_zero_fill_{false};
    std::atomic<uint64_t> reverb_mixer_feed_count{0};
    /// fetch_reverb_params calls that returned an entry with a non-null IR (convolution, hybrid, TAN).
    std::atomic<uint64_t> reverb_convolution_valid_fetches{0};
    /// Reflection fetches served from the lock-free cache, stale fallbacks included.
    std::atomic<uint64_t> instrumentation_fetch_cache_hit{0};
    /// Reflection fetches where the source takes part in reflections but has no usable cache entry.
    std::atomic<uint64_t> instrumentation_fetch_cache_miss{0};
    /// Reflection fetches that reused a stale entry's IR or params until the next worker refresh.
    std::atomic<uint64_t> instrumentation_fetch_refl_stale_epoch_fallback{0};
    /// Reflection fetches for sources that do not take part in reflections; not a miss.
    std::atomic<uint64_t> instrumentation_fetch_cache_skip{0};
    /// fetch_pathing_params returned early: bad handle, no context, pathing off, or source not attached.
    std::atomic<uint64_t> instrumentation_pathing_fetch_early_exit{0};
    /// Never incremented; still reported as fetch_src_null.
    std::atomic<uint64_t> instrumentation_pathing_fetch_src_null{0};
    /// Worker sync copied pathing SH coefficients into the cache.
    std::atomic<uint64_t> instrumentation_pathing_fetch_sh_ok{0};
    /// Never incremented; still reported in the debug dictionary.
    std::atomic<uint64_t> instrumentation_pathing_fetch_sh_null{0};
    /// Worker sync got shCoeffs but could not copy them for the current order.
    std::atomic<uint64_t> instrumentation_pathing_fetch_sh_bad_order{0};
    /// fetch_pathing_params found a current cache entry.
    std::atomic<uint64_t> instrumentation_pathing_fetch_cache_hit{0};
    /// fetch_pathing_params found no current, valid cache entry.
    std::atomic<uint64_t> instrumentation_pathing_fetch_cache_miss{0};
    std::atomic<uint64_t> instrumentation_pathing_sim_attempt{0};
    std::atomic<uint64_t> instrumentation_pathing_sim_ran{0};
    /// Windows SEH faults caught in iplSimulatorRunPathing.
    std::atomic<uint64_t> instrumentation_pathing_sim_seh_fail{0};
    /// RunPathing skipped because no valid listener pose was pending.
    std::atomic<uint64_t> instrumentation_pathing_sim_skip_listener{0};
    std::atomic<uint64_t> instrumentation_pathing_sim_skip_cooldown{0};
    /// Player blocks that entered the pathing mix (pathing on, pathing mix level > 0).
    std::atomic<uint64_t> instrumentation_pathing_player_gate{0};
    std::atomic<uint64_t> instrumentation_pathing_player_applied{0};
    /// Pathing blocks with no params; the block gets no pathing audio.
    std::atomic<uint64_t> instrumentation_pathing_player_fetch_miss{0};
    /// Microseconds of the last worker tick; zeroed when a tick has nothing to run.
    std::atomic<uint64_t> instrumentation_worker_us_run_direct{0};
    std::atomic<uint64_t> instrumentation_worker_us_run_reflections{0};
    std::atomic<uint64_t> instrumentation_worker_reflection_passes_skipped_{0};
    std::atomic<uint64_t> instrumentation_worker_pathing_passes_skipped_{0};
    std::atomic<uint64_t> instrumentation_worker_us_run_pathing{0};
    std::atomic<uint64_t> instrumentation_worker_us_sync_fetch{0};
    /// numRays of the last tick; 0 when no source uses realtime reflections.
    std::atomic<int32_t> instrumentation_worker_last_num_rays_{0};
    /// Adaptive ray target of the last tick, before clamping to max_rays.
    std::atomic<int32_t> instrumentation_worker_last_adaptive_num_rays_target_{0};
    /// Last tick's iplSimulatorCommit time, for source add/remove and after SetScene.
    std::atomic<uint64_t> instrumentation_worker_us_simulator_commit{0};
    /// Last tick's iplSceneCommit + iplSimulatorSetScene time; 0 when the scene was clean.
    std::atomic<uint64_t> instrumentation_worker_us_scene_graph_commit{0};
    /// Last tick's time applying queued dynamic instance transforms.
    std::atomic<uint64_t> instrumentation_worker_us_dynamic_instanced_apply{0};
    /// True if the last pass ran reflections or pathing; their timings are only meaningful then.
    std::atomic<bool> instrumentation_worker_last_wake_was_heavy{false};
    /// Parts of instrumentation_worker_us_sync_fetch.
    std::atomic<uint64_t> instrumentation_worker_us_sync_fetch_occlusion{0};
    std::atomic<uint64_t> instrumentation_worker_us_sync_fetch_reflections{0};
    std::atomic<uint64_t> instrumentation_worker_us_sync_fetch_pathing{0};

    /// Audio-thread timings of the last block, in microseconds.
    std::atomic<uint64_t> instrumentation_audio_conv_refl_apply_last_us_{0};
    std::atomic<uint64_t> instrumentation_audio_conv_reverb_bus_last_us_{0};
    std::atomic<uint64_t> instrumentation_audio_mixer_sanitize_ambi_last_us_{0};
    std::atomic<uint64_t> instrumentation_audio_mixer_sanitize_stereo_last_us_{0};
    /// Convolution mixer feeds with a null IR; expected to stay 0.
    std::atomic<uint64_t> reverb_convolution_feed_ir_null{0};
    /// Linear mixer-feed gain range seen: reflections_mix_level * source volume.
    std::atomic<float> reverb_convolution_gain_min{1.0f};
    std::atomic<float> reverb_convolution_gain_max{0.0f};
    /// Mono convolution input RMS before the feed gain.
    std::atomic<float> reverb_convolution_input_rms_max{0.0f};

    /// Set by the reverb bus when Godot's real frame_count differs; the main thread reinits.
    std::atomic<int> pending_reinit_frame_size_{0};
    /// Only an Auto frame size is reinitialized on a frame_count mismatch.
    std::atomic<bool> audio_frame_size_was_auto_{true};
    /// False after triangle geometry is registered until the worker runs iplSceneCommit for that edit.
    std::atomic<bool> phonon_scene_audio_ready_{true};

    ResonanceBaker baker;
    SourceManager source_manager;
    ResonancePlayerResourcePool resource_pool_;
    IplResourcePoolBackend resource_pool_backend_;
    /// _apply_config copies these values into the plain members below for hot paths; a new key needs both.
    ResonanceServerConfig config_;

    IPLSimulationSettings simulation_settings{};
    std::atomic<int> global_triangle_count{0};

    /// IPLSource at the listener pose whose reflections feed the reverb bus and the FMOD bridge. -1 when not created.
    std::atomic<int32_t> listener_reverb_source_handle_{-1};
    std::atomic<bool> listener_reverb_enabled_{false};
    int listener_reverb_type_ = resonance::kListenerReverbRealtime;
    bool fmod_reverb_requested_ = false;
    void _sync_listener_reverb_source();
    int32_t _capped_source_count() const;
    void _push_listener_reverb_source_pose();

    int current_sample_rate = 48000;
    int frame_size = resonance::kGodotDefaultFrameSize; // Steam Audio block size; matches Godot's mix callback
    int ambisonic_order = 1;
    int simulation_threads = 1; // Derived from simulation_cpu_cores_percent
    float simulation_cpu_cores_percent = resonance::kDefaultSimulationCpuCoresPercent;
    int max_rays = resonance::kDefaultRealtimeRays;
    int max_bounces = 4;
    float reverb_influence_radius = resonance::kBakedEndpointRadius;

    // 0 = convolution, 1 = parametric, 2 = hybrid, 3 = TAN
    int reflection_type = 0;
    float hybrid_reverb_transition_time = 1.0f;
    float hybrid_reverb_overlap_percent = 0.25f; // Fraction, 0-1
    // 0 = frequency-independent, 1 = frequency-dependent
    int transmission_type = 1;
    int max_transmission_surfaces = resonance::kDefaultPlayerConfigTransmissionRays;
    // 0 = raycast, 1 = volumetric
    int occlusion_type = 1;
    int max_occlusion_samples = resonance::kMaxOcclusionSamples;
    int max_simulation_sources = resonance::kMaxSimulationSources;
    /// Stays set after the first refusal at max_simulation_sources, so the cap is logged once.
    bool simulation_source_cap_error_emitted_ = false;
    float hrtf_volume_db = 0.0f;
    int hrtf_normalization_type = 0;
    // Null = built-in HRTF.
    Ref<ResonanceSOFAAsset> hrtf_sofa_asset;
    // Only take effect with a loaded HRTF.
    bool direct_binaural = true;
    bool reverb_binaural = true;
    bool pathing_binaural = true;
    // False = nearest (cheaper), true = bilinear (smoother for moving sources).
    bool hrtf_interpolation_bilinear = false;
    // Decodes reverb Ambisonics to 7.1, then iplVirtualSurroundEffect renders that to binaural stereo.
    bool use_virtual_surround = false;
    /// Speaker count for direct-path panning: 1, 2, 4, 6 or 8.
    int direct_speaker_channels = 2;
    bool pathing_enabled = false;
    /// Pathing can only be turned on at runtime if the simulator was created with IPL_SIMULATIONFLAGS_PATHING.
    bool simulator_created_with_pathing_ = false;
    float pathing_vis_radius = 1.0f;
    float pathing_vis_threshold = 0.1f;
    float pathing_vis_range = 1000.0f;
    bool pathing_normalize_eq = true;
    int pathing_num_samples = resonance::kRuntimePathingDefaultNumVisSamples;
    /// Defaults for players on Use Global (-1).
    bool path_validation_enabled = true;
    bool find_alternate_paths = false;
    // Custom pathing deviation model; Steam Audio's default (UTD) when the callback is disabled.
    IPLDeviationModel _pathing_deviation_model{};
    bool _pathing_deviation_callback_enabled = false;
    std::mutex _pathing_deviation_mutex;
    struct PathingDeviationLut {
        int samples_per_band = resonance::kPathingDeviationDefaultLutSamples;
        std::vector<float> band0;
        std::vector<float> band1;
        std::vector<float> band2;
        const float* band_ptr(int band) const;
    };
    std::shared_ptr<const PathingDeviationLut> _pathing_deviation_lut;
    Callable _pathing_deviation_godot_callable;
    int _pathing_deviation_lut_samples = resonance::kPathingDeviationDefaultLutSamples;
    static float IPLCALL _pathing_deviation_lut_ipl_callback(IPLfloat32 angle, IPLint32 band, void* userData);
    static std::shared_ptr<PathingDeviationLut> _pathing_deviation_lut_from_callable(const Callable& callable, int samples_per_band);
    // 0 = built-in, 1 = Embree, 2 = Radeon Rays, 3 = custom (Godot physics)
    int scene_type = 0;
    /// rayBatchSize for the custom scene, clamped at init; other scene types use 1.
    int physics_ray_batch_size = resonance::kDefaultPhysicsRayBatchSize;
    // For Radeon Rays and TAN. 0 = GPU, 1 = CPU, 2 = any.
    int opencl_device_type = 0;
    int opencl_device_index = 0;
    bool context_validation = false;
    // 0 = AVX-512 ... 4 = SSE2, the reverse of IPLSIMDLevel; -1 = Steam Audio picks.
    int context_simd_level = -1;
    float realtime_irradiance_min_distance = 0.1f;
    float realtime_simulation_duration = 2.0f;
    int realtime_num_diffuse_samples = 32;

    // Written on the main thread, read on audio and worker threads.
    std::atomic<bool> output_direct_enabled{true};
    std::atomic<bool> output_reverb_enabled{true};
    std::atomic<bool> debug_occlusion{false};
    std::atomic<bool> debug_reflections{false};
    std::atomic<bool> debug_pathing{false};

    // Spatializes from the on-screen position instead of the 3D position; for flat screens, not VR.
    std::atomic<bool> perspective_correction_enabled{false};
    std::atomic<float> perspective_correction_factor{1.0f};

    struct PathVisSegment {
        Vector3 from;
        Vector3 to;
        bool occluded;
    };
    std::vector<PathVisSegment> pathing_vis_segments;
    std::mutex pathing_vis_mutex;
    /// userData is this server, so the callback must be cleared from the shared inputs before shutdown frees it.
    static void IPLCALL _pathing_vis_callback(IPLVector3 from, IPLVector3 to, IPLbool occluded, void* userData);

    RayTraceDebugContext ray_trace_debug_context_;
    std::atomic<int> ray_debug_bounce_index_{0};

    // Audio-thread caches are double buffered: the worker fills the back slot, then publishes by flipping
    // the front index. Readers check each entry's epoch against the slot epoch.
    static constexpr int kCacheSlots = 2;
    static constexpr int kMaxPathingSHCoeffs = resonance::kPathingFetchedShCoeffs;
    static constexpr int kMaxCacheHandles = resonance::kMaxSimulationSourcesUserMax;

    struct CachedParametricReverb {
        float reverbTimes[3] = {0};
        float eq[3] = {0};
        uint32_t epoch = 0;
    };
    std::array<std::array<CachedParametricReverb, kMaxCacheHandles>, kCacheSlots> reverb_param_cache_{};
    std::atomic<int> reverb_param_cache_front_{0};
    uint32_t reverb_param_cache_epoch_[kCacheSlots] = {1, 1};

    // The IR is Steam Audio's per-source triple buffer, so a cached handle stays valid; applying it while no
    // new IR was committed keeps convolving with the previous one.
    struct CachedReflectionParams {
        IPLReflectionEffectParams params{};
        uint32_t epoch = 0;
        uint32_t ir_generation = 0;
    };
    std::array<std::array<CachedReflectionParams, kMaxCacheHandles>, kCacheSlots> reflection_param_cache_{};
    std::atomic<int> reflection_param_cache_front_{0};
    uint32_t reflection_param_cache_epoch_[kCacheSlots] = {1, 1};
    /// Worker only. Last params with a non-null IR, used when a fetch returns a null IR.
    std::array<IPLReflectionEffectParams, kMaxCacheHandles> last_good_reflection_params_{};
    std::array<std::atomic<uint8_t>, kMaxCacheHandles> last_good_reflection_valid_{};
    /// Bumped when a handle is created or destroyed so a recycled id cannot Apply the previous IR.
    std::array<std::atomic<uint32_t>, kMaxCacheHandles> source_ir_generation_{};
    void _retire_reflection_ir_for_handle(int32_t handle);
    /// Last interpolated STATICSOURCE baked energy, for debug traces. The worker writes it while the main
    /// thread resets and reads it, so it is atomic; relaxed is enough for a diagnostic value.
    std::array<std::atomic<float>, kMaxCacheHandles> reflection_baked_energy_last_{};

    /// Last live reflection fetch result per handle, so the main thread can answer has-reverb queries
    /// without calling fetch_reverb_params.
    mutable std::mutex reverb_params_likely_available_mutex_;
    std::unordered_map<int32_t, bool> reverb_params_likely_available_;
    void _clear_reverb_params_likely_available_hints();

    struct CachedPathingParams {
        float eqCoeffs[3] = {0};
        std::array<float, kMaxPathingSHCoeffs> shCoeffs{};
        int order = 1;
        uint32_t epoch = 0;
    };
    std::array<std::array<CachedPathingParams, kMaxCacheHandles>, kCacheSlots> pathing_param_cache_{};
    std::atomic<int> pathing_param_cache_front_{0};
    uint32_t pathing_param_cache_epoch_[kCacheSlots] = {1, 1};

    // get_source_occlusion_data runs every frame on the main thread and must not wait for simulation_mutex
    // while the worker holds it through RunReflections.
    struct CachedOcclusionData {
        OcclusionData data{};
        uint32_t epoch = 0;
    };
    std::array<std::array<CachedOcclusionData, kMaxCacheHandles>, kCacheSlots> occlusion_cache_{};
    std::atomic<int> occlusion_cache_front_{0};
    uint32_t occlusion_cache_epoch_[kCacheSlots] = {1, 1};

    // When both are needed: phonon_context_exclusive_mutex_ before simulation_mutex. The bake takes it exclusive
    // without simulation_mutex during long ipl*Baker; the direct and reflections workers take it shared.
    resonance::WriterPreferringSharedMutex phonon_context_exclusive_mutex_;
    // simulation_mutex serializes scene and simulator changes, bake, geometry notifies and the worker tick.
    // With the probe batch registry mutex, take simulation_mutex first. The audio thread takes neither: it reads
    // only the lock-free caches and atomics.
    std::mutex simulation_mutex;
    std::thread worker_thread;
    std::mutex worker_mutex;
    std::condition_variable worker_cv;
    std::atomic<bool> thread_running = false;
    std::atomic<bool> simulation_requested = false;
    // The reflections worker holds simulation_mutex for its whole pass; meanwhile the direct worker runs
    // direct-only passes through reflections_pass_gate_.
    std::thread reflections_worker_thread_;
    std::mutex reflections_worker_mutex_;
    std::condition_variable reflections_worker_cv_;
    ReflectionsPassRequest reflections_pass_request_;
    bool reflections_pass_request_pending_ = false;
    /// From handing a pass to the reflections worker until that pass finished.
    std::atomic<bool> reflections_worker_busy_{false};
    resonance::ReflectionsPassGate reflections_pass_gate_;
    std::atomic<bool> scene_dirty = false;
    /// At cold start, geometry commits pile up in scene_dirty and reflections and pathing wait until
    /// finish_cold_start_settle().
    std::atomic<bool> cold_start_settle_pending_{false};
    float dynamic_scene_commit_min_interval_ = 0.0f;
    std::chrono::steady_clock::time_point last_dynamic_scene_commit_time_{};
    struct DynamicInstancedTransformEntry {
        IPLMatrix4x4 transform{};
        bool force_apply = false;
    };
    std::mutex dynamic_instanced_transform_queue_mutex_;
    std::unordered_map<IPLInstancedMesh, DynamicInstancedTransformEntry> dynamic_instanced_transform_queue_;
    std::atomic<uint64_t> instrumentation_main_us_dynamic_transform_enqueue_{0};
    /// Microseconds of the most recent enqueue_dynamic_instanced_mesh_transform call (main thread).
    std::atomic<uint64_t> instrumentation_main_us_last_dynamic_transform_enqueue_{0};
    std::atomic<uint64_t> instrumentation_dynamic_transform_enqueue_events_{0};
    // Seconds between scheduled reflections and pathing runs; direct runs on every tick.
    float reflections_sim_interval = 0.1f;
    float pathing_sim_interval = 0.1f;
    float reflections_interval_elapsed = 0.0f;
    float pathing_interval_elapsed = 0.0f;
    float realtime_reflection_max_distance_m = 0.0f;
    std::atomic<bool> reflection_sim_heavy_requested{false};
    std::atomic<bool> pathing_sim_heavy_requested{false};
    /// Seconds. When > 0, non-heavy ticks skip RunDirect until this much time has passed.
    float direct_sim_interval = 0.0f;
    float direct_sim_time_elapsed = 0.0f;
    std::atomic<bool> worker_run_direct_next{true};
    /// Set when direct inputs were set on the main thread; cleared once the worker ran RunDirect.
    std::atomic<bool> direct_after_inline_inputs_pending_{false};
    /// Set by the worker when it deferred RunReflections; the next tick requests reflections again.
    std::atomic<bool> reflection_force_heavy_next_tick_{false};
    /// Seconds added to the reflections interval by the adaptive budget.
    float reflections_adaptive_extra_interval_ = 0.0f;
    uint32_t reflections_adaptive_interval_seen_pass_ = 0;
    uint32_t reflections_adaptive_budget_us_ = 0;
    int reflections_adaptive_ray_min_ = 128;
    float reflections_adaptive_ray_recover_frac_ = 0.125f;
    int reflections_adaptive_ray_recover_cap_ = 512;
    float reflections_adaptive_step_sec_ = 0.02f;
    float reflections_adaptive_max_extra_interval_ = 0.2f;
    float reflections_adaptive_decay_per_sec_ = 0.05f;
    uint32_t reflections_defer_after_scene_commit_us_ = 0;
    int convolution_ir_max_samples_ = 0;

    // Worker only, under simulation_mutex.
    int _adaptive_realtime_num_rays_ = 0;
    bool _adaptive_realtime_num_rays_initialized_ = false;
    uint32_t _adaptive_rays_seen_reflection_pass_ = 0;
    /// Duration of the last tick that actually ran RunReflections. The worker stores it before
    /// bumping reflection_pass_count_ (release), so readers load the count first.
    std::atomic<uint64_t> last_reflection_pass_us_{0};
    std::atomic<uint32_t> reflection_pass_count_{0};
    /// Set by the worker when the last pass already used the minimum ray count or had no realtime rays.
    std::atomic<bool> reflections_adaptive_rays_at_floor_{false};

    bool batch_source_updates = true;

    using SourceTable =
        resonance::SourceSimulationTable<resonance::kMaxSimulationSourcesUserMax, SourceUpdateParams, IPLSource>;
    // Source adds and removes queue here so the main thread never waits for simulation_mutex; the worker
    // drains them at the start of every tick.
    SourceTable source_table_;

    // Seqlock: the main thread writes, an odd listener_seq_ means a write is in progress. Readers never consume
    // the pose, so the reverb bus and players see the same basis (a mismatch comb-filters wet against dry).
    IPLCoordinateSpace3 listener_coords_latest_{};
    std::atomic<uint32_t> listener_seq_{0};
    std::atomic<bool> pending_listener_valid{false};
    std::atomic<bool> pathing_ran_this_tick{false};
    /// True when the published pathing results match this tick's inputs: RunPathing ran, or was skipped
    /// because nothing it reads changed.
    std::atomic<bool> pathing_results_current_{false};
    /// Before the first RunReflections, Steam Audio's validation warns about reverbTimes of 0.
    std::atomic<bool> reflections_have_run_once_{false};
    /// Guarded by simulation_mutex.
    uint64_t scene_generation_ = 0;
    resonance::ReflectionPassInputs reflection_last_pass_inputs_{};
    bool reflection_have_last_pass_ = false;
    uint64_t pathing_deviation_generation_ = 0;
    resonance::PathingPassInputs pathing_last_pass_inputs_{};
    bool pathing_have_last_pass_ = false;
    /// flush_pending_source_updates found simulation_mutex taken.
    std::atomic<uint64_t> instrumentation_source_flush_try_lock_fail_{0};
    std::atomic<uint64_t> instrumentation_direct_passes_during_reflections_{0};
    /// Owned by whoever runs the reflections pass (simulation_mutex).
    resonance::ReflectionsAgeTracker reflections_age_tracker_;
    std::atomic<uint64_t> instrumentation_reflections_age_us_last_{0};
    std::atomic<uint64_t> instrumentation_reflections_age_us_max_{0};
    std::atomic<uint64_t> instrumentation_reflections_age_samples_{0};
    /// iplSourceSetInputs calls skipped because the params had not changed.
    std::atomic<uint64_t> instrumentation_source_update_skip_unchanged_{0};
    /// Attenuation callback table rewrites skipped because nothing changed.
    std::atomic<uint64_t> instrumentation_attenuation_callback_skip_unchanged_{0};
    /// iplReflectionMixerApply calls skipped because nothing fed the mixer.
    std::atomic<uint64_t> instrumentation_convolution_bus_skip_empty_mixer_apply_{0};
    /// Ticks left without RunPathing after a caught Windows SEH fault.
    std::atomic<int> pathing_crash_cooldown{0};
    /// Batches retained while setting source inputs, released after RunPathing. Guarded by simulation_mutex.
    std::vector<IPLProbeBatch> pathing_probe_batches_pending_release_;
    bool pathing_no_data_warned = false;
    /// Owner node paths for pathing batch warnings; set on the main thread, read by the worker.
    std::mutex source_pathing_owner_mutex_;
    std::unordered_map<int32_t, String> source_pathing_owner_path_;
    /// Once per owner path and lookup outcome. Source destroy must not clear this, or each new voice warns again.
    std::vector<String> pathing_batch_resolve_warned_keys_;

    /// Set by the editor before a bake; when non-empty the bake uses these instead of live geometry.
    std::vector<Ref<ResonanceGeometryAsset>> _bake_static_scene_assets;
    std::vector<Transform3D> _bake_static_scene_transforms;

    // Bake overrides from ResonanceRuntimeConfig; negative = use the default.
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
    bool _bake_pipeline_active = false;
    std::atomic<float> bake_progress_{0.0f};
    std::atomic<bool> bake_cancel_requested_{false};

    ResonanceProbeBatchRegistry probe_batch_registry_;
    ResonanceSceneManager scene_manager_;
    static uint64_t _hash_probe_data(const PackedByteArray& pba);
    static uint64_t _hash_probe_data(const uint8_t* ptr, size_t size);
    static StaticSceneHashFn _static_scene_hash_fn();
    static std::atomic<bool> is_shutting_down_flag;
    /// Set through all of _shutdown_steam_audio, reinit included, so the audio thread stops using IPL first.
    std::atomic<bool> ipl_teardown_active_{false};

    /// For shutdown, where is_shutting_down makes destroy_source_handle a no-op.
    void _destroy_source_handle_under_simulation_lock(resonance::SimulationAccess access, int32_t handle);
    /// Applies queued source adds and removes with one iplSimulatorCommit if anything changed.
    bool _drain_pending_source_lifecycle_assume_locked(resonance::SimulationAccess access, uint64_t* commit_us_out = nullptr);
    /// With direct-only access, reflection and pathing inputs are re-queued for the next full pass.
    bool _flush_pending_source_updates_assume_locked(resonance::SimulationAccess access);
    /// No-op during cold-start settle; finish_cold_start_settle() sends the single wake.
    void _wake_phonon_worker_for_lifecycle();
    /// Arms the next RunDirect after try_update_source set inputs on the main thread.
    void _arm_phonon_direct_after_inline_inputs();
    void _consume_heavy_sim_flags(bool allow_heavy, bool drop_on_block, bool& run_refl, bool& run_path);
    /// Requires simulation_mutex.
    void _drain_pathing_probe_batch_releases();
    /// Requires simulation_mutex. Copies iplSourceGetOutputs into the audio-thread caches. A skipped output
    /// keeps its last cached values.
    void _worker_sync_fetch_caches(resonance::SimulationAccess access, bool refresh_direct_outputs, bool refresh_reflection_outputs, bool refresh_pathing_outputs);
    /// Requires simulation_mutex. Call after RunReflections and before the cache sync.
    void _worker_clear_reflections_pending_after_run_assume_locked(bool ran_reflections);
    /// Shared by fetch_reverb_params and the worker's reflection sync.
    bool _source_reflection_fetch_allowed(int32_t handle, bool reflections_have_run) const;
    uint64_t _worker_fetch_occlusion_into_back(resonance::SimulationAccess access, IPLSource src, int32_t handle, int occ_back);
    bool _worker_fetch_reflection_into_back(resonance::SimulationAccess access, IPLSource src, int32_t handle, int reverb_back, int refl_back, bool reflections_have_run,
                                            bool sync_after_run_reflections, uint64_t& out_microseconds);
    float _static_source_interpolated_baked_energy(const SourceUpdateParams& params, const Vector3& listener_pos) const;
    static bool _pathing_copy_sh_coeffs(std::array<float, kMaxPathingSHCoeffs>& dst, const float* src, int sh_count);

    void _apply_config(Dictionary config);
    void _worker_thread_func();
    void _reflections_worker_thread_func();
    /// Hands a reflections pass to the reflections worker. Call only when reflections_pass_may_start().
    void _post_reflections_pass(const ReflectionsPassRequest& request);
    void _worker_note_run_direct_executed();
    /// generation: source table readiness generation read before the pass committed the scene graph.
    void _worker_mark_direct_results_published(const std::vector<int32_t>& handles, uint32_t generation);
    /// Direct then reflections pass. Main thread only, for the custom scene.
    void _run_phonon_simulation_locked(resonance::SimulationAccess access, const IPLCoordinateSpace3& current_listener, bool run_direct, bool run_reflection_sim, bool run_pathing_sim);
    /// Returns the reflections pass that is due; the caller runs it or hands it to the reflections worker.
    ReflectionsPassRequest _run_direct_pass(resonance::SimulationAccess access, const IPLCoordinateSpace3& current_listener, bool run_direct, bool run_reflection_sim,
                                            bool run_pathing_sim);
    /// Requires simulation_mutex.
    void _note_realtime_reflection_sources(const std::vector<int32_t>& handles);
    /// Requires simulation_mutex. Call once per reflections pass, also when it was skipped as unchanged.
    void _note_reflections_age(bool realtime_reflections_active);
    /// A non-null gate is opened around the Run* calls.
    void _run_reflections_pass_locked(resonance::SimulationAccess access, const ReflectionsPassRequest& request, resonance::ReflectionsPassGate* gate);
    /// Main thread. Returns whether this wake should run RunDirect.
    bool _tick_schedule_simulation(float delta, const std::vector<int32_t>& handles);
    struct ReflectionSourceCounts {
        int32_t active_reflection_sources = 0;
        int32_t active_realtime_sources = 0;
    };
    ReflectionSourceCounts _count_reflection_source_flags(const std::vector<int32_t>& handles) const;
    /// Lock-free; safe without simulation_mutex.
    bool _any_source_has_reflection_outputs(const std::vector<int32_t>& handles) const;
    bool _any_source_has_realtime_reflection_outputs(const std::vector<int32_t>& handles) const;
    bool _any_source_has_pathing_outputs(const std::vector<int32_t>& handles) const;
    bool _any_source_has_direct_outputs(const std::vector<int32_t>& handles) const;
    bool _has_pending_source_lifecycle();
    bool _has_pending_source_updates();
    bool _has_pending_dynamic_instanced_transforms();
    bool _phonon_worker_tick_has_work(bool run_direct_scheduled, bool run_reflection_heavy, bool run_pathing_heavy,
                                      const std::vector<int32_t>& handles);
    void _compute_adaptive_eff_num_rays(bool any_realtime_reflections, bool run_reflection_sim, int& eff_num_rays, int& adaptive_target);
    resonance::AdaptiveRayLimits _adaptive_ray_limits() const;
    void _note_reflection_pass(uint64_t pass_us, int num_rays, bool realtime_rays);
    void _update_adaptive_extra_interval(float delta);
    uint64_t _commit_simulator_scene_graph_if_dirty_assume_locked(bool* committed_out = nullptr);
    uint64_t _ipl_simulator_commit_assume_locked();
    uint64_t _run_pathing_sim_assume_locked(resonance::SimulationAccess access, bool run_pathing_sim, const IPLSimulationSharedInputs& shared);
    void _note_pathing_pass_ran(const resonance::PathingPassInputs& pass_inputs);
    IPLCoordinateSpace3 _snapshot_listener_for_simulation();
    IPLCoordinateSpace3 _read_listener_coords_seqlock() const;
    bool _uses_main_thread_phonon_simulation() const;
    /// Never CUSTOM: static mesh loads and bake scenes need a native tracer.
    IPLSceneType _tracer_type_for_mesh_operations() const;
    void _init_internal();
    void _init_context_and_devices();
    bool _init_scene_and_simulator();
    void _deferred_refresh_all_geometry_after_scene_load();
    void _start_worker_thread();
    void _start_resource_pool();
    bool _pooled_reflection_effect_matches(IPLContext context, const ReflectionEffectRecipe& recipe) const;
    static IPLSourceSettings _player_source_settings();
    void _stop_and_join_worker();
    void _shutdown_steam_audio();
    /// Caller holds AudioServer::lock.
    void _drain_ipl_context_clients_assume_audio_locked();
    void _drain_ipl_context_clients_before_context_destroy();

    struct IplContextClient {
        void* key = nullptr;
        void (*cleanup)(void*) = nullptr;
    };
    std::mutex ipl_context_clients_mutex_;
    std::vector<IplContextClient> ipl_context_clients_;

    SourceUpdateParams _default_new_source_params() const;
    /// Returns false when the update was skipped as unchanged.
    bool _update_source_internal(resonance::SimulationAccess access, IPLSource source, int32_t handle, const SourceUpdateParams& params);
    void _clear_source_pathing_inputs_assume_locked(resonance::SimulationAccess access, IPLSource source, int32_t handle);
    void _clear_pathing_for_probe_batch_assume_locked(resonance::SimulationAccess access, int32_t removing_handle);
    void _collect_handles_affected_by_probe_batch_remove_assume_locked(resonance::SimulationAccess access, int32_t removing_handle, std::vector<int32_t>& out_handles);
    void _invalidate_param_caches_for_handles(const std::vector<int32_t>& handles);
    void _apply_pending_probe_batch_changes_assume_locked(resonance::SimulationAccess access);
    /// Requires simulation_mutex.
    void _invalidate_pathing_cache_for_active_sources_assume_locked();
    /// Returns how many updates ran SetInputs; unchanged updates do not count.
    int _apply_source_update_batch(resonance::SimulationAccess access, const std::vector<SourceTable::QueuedUpdate>& batch);
    /// Counts an input change only when the inputs differ.
    void _set_source_inputs_assume_locked(resonance::SimulationAccess access, IPLSource src, int32_t handle, IPLSimulationFlags selector,
                                          IPLSimulationInputs* inputs);
    void _forget_source_inputs_assume_locked(resonance::SimulationAccess access, int32_t handle);
    resonance::ReflectionPassInputs _reflection_pass_inputs(resonance::SimulationAccess access, const IPLSimulationSharedInputs& shared) const;
    resonance::PathingPassInputs _pathing_pass_inputs(resonance::SimulationAccess access, const IPLSimulationSharedInputs& shared) const;

    int _get_bake_num_rays() const;
    int _get_bake_num_bounces() const;
    int _get_bake_num_threads() const;
    int _get_bake_ambisonics_order() const;
    int _get_bake_reflection_type() const;
    float _get_bake_pathing_param(const char* key, float default_val) const;
    int _get_bake_pathing_num_samples() const;
    /// Separate from the live scene. Release with _release_bake_scene_scratch().
    struct BakeSceneScratch {
        IPLScene scene = nullptr;
        std::vector<IPLStaticMesh> meshes;
        std::vector<IPLScene> sub_scenes;
        std::vector<IPLInstancedMesh> instanced;
        /// Parallel to sub_scenes.
        std::vector<IPLStaticMesh> meshes_in_sub;
    };
    /// Requires simulation_mutex. Builds from bake assets, runtime static meshes or a live-scene snapshot.
    bool _prepare_bake_scene(BakeSceneScratch& out);
    static void _release_bake_scene_scratch(BakeSceneScratch& scratch);
    bool _has_bake_static_scene_assets() const;
    /// Holds simulation_mutex only while preparing the scene, not during the bake itself.
    bool _with_bake_scene(std::function<bool(IPLScene bake_scene)> bake_fn);
    /// Requires simulation_mutex; call before the scene commit. Returns true if any transform was applied.
    bool _apply_queued_dynamic_instanced_mesh_transforms_assume_locked();
    enum class SceneMeshChangeKind { AddStatic,
                                     RemoveStatic,
                                     AddInstanced,
                                     RemoveInstanced,
                                     SetStaticMaterial };
    struct SceneMeshChange {
        SceneMeshChangeKind kind = SceneMeshChangeKind::AddStatic;
        IPLStaticMesh static_mesh = nullptr;
        IPLInstancedMesh instanced_mesh = nullptr;
        IPLScene sub_scene = nullptr;
        IPLMaterial material{};
    };
    std::mutex scene_mesh_queue_mutex_;
    std::vector<SceneMeshChange> scene_mesh_queue_;
    void _queue_scene_mesh_change(const SceneMeshChange& change);
    bool _has_pending_scene_mesh_changes();
    void _apply_scene_mesh_change_assume_locked(SceneMeshChange& change);
    void _apply_static_mesh_material_assume_locked(SceneMeshChange& change);
    /// preferred_handle if it has pathing data, else the first batch that has. The result is retained;
    /// the caller releases it.
    IPLProbeBatch _get_pathing_batch_for_source(int32_t source_handle, int32_t preferred_handle);
    void _clear_all_param_caches();
    bool _is_batch_compatible_with_config(int32_t handle) const;

    bool _uses_convolution_or_hybrid_or_tan() const;
    bool _uses_parametric_or_hybrid() const;
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
    /// Called on module uninitialize, before the destructor.
    void shutdown();

    void init_audio_engine(Dictionary config);
    /// Shuts down, then initializes with the new config.
    void reinit_audio_engine(Dictionary config);

    /// Holders of IPL buffers or effects register here; cleanup runs under AudioServer::lock before the
    /// context is released on reinit or shutdown.
    using IplContextClientCleanup = void (*)(void* userdata);
    void register_ipl_context_client(void* key, IplContextClientCleanup cleanup);
    void unregister_ipl_context_client(void* key);

    IPLContext get_context_handle() const { return steam_audio_context_ ? steam_audio_context_->get_context() : nullptr; }
    IPLScene get_scene_handle() const { return scene; }
    IPLSimulator get_simulator_handle() const { return simulator; }
    IPLEmbreeDevice get_embree_device_handle() const { return steam_audio_context_ ? steam_audio_context_->get_embree_device() : nullptr; }
    IPLOpenCLDevice get_opencl_device_handle() const { return steam_audio_context_ ? steam_audio_context_->get_opencl_device() : nullptr; }
    IPLRadeonRaysDevice get_radeon_rays_device_handle() const { return steam_audio_context_ ? steam_audio_context_->get_radeon_rays_device() : nullptr; }
    IPLHRTF get_hrtf_handle() const { return steam_audio_context_ ? steam_audio_context_->get_hrtf() : nullptr; }
    IPLReflectionMixer get_reflection_mixer_handle() const;
    uint64_t get_reflection_mixer_release_deferred_count() const;
    /// Alias of get_reflection_mixer_release_deferred_count.
    uint64_t get_reflection_mixer_release_forced_count() const;
    /// Null when TAN is off.
    IPLTrueAudioNextDevice get_tan_device_for_reflection() const { return _tan(); }

    void fill_reflection_mixer_apply_params(IPLReflectionEffectParams* out_params) const;
    IPLCoordinateSpace3 get_current_listener_coords();
    /// Audio thread safe. -1 while no listener reverb source exists.
    int32_t get_listener_reverb_source_handle() const {
        return listener_reverb_source_handle_.load(std::memory_order_acquire);
    }
    /// Audio thread safe. True while a ResonanceListener applies reverb to the reverb bus.
    bool is_listener_reverb_enabled() const { return listener_reverb_enabled_.load(std::memory_order_acquire); }
    /// Main thread. reverb_type is kListenerReverbRealtime or kListenerReverbBaked.
    void set_listener_reverb(bool enabled, int reverb_type);
    /// Keeps the listener reverb source alive for iplFMODSetReverbSource.
    void ensure_fmod_reverb_source();
    /// Null while uninitialized.
    const IPLSimulationSettings* get_simulation_settings_for_fmod() const { return _ctx() ? &simulation_settings : nullptr; }
    IPLSceneType get_scene_type() const { return steam_audio_context_ ? steam_audio_context_->get_scene_type() : IPL_SCENETYPE_DEFAULT; }
    /// For iplStaticMeshLoad, sub-scenes and bake scenes. DEFAULT when the runtime scene is CUSTOM.
    IPLSceneType get_phonon_mesh_scene_type() const { return _tracer_type_for_mesh_operations(); }
    /// Queued for the worker, latest transform per mesh wins; does not take simulation_mutex.
    void enqueue_dynamic_instanced_mesh_transform(IPLInstancedMesh mesh, const IPLMatrix4x4& transform);
    /// Like enqueue, but applied on the next worker tick regardless of dynamic_scene_commit_min_interval.
    void flush_dynamic_instanced_mesh_transform(IPLInstancedMesh mesh, const IPLMatrix4x4& transform);
    /// Call before releasing the mesh.
    void cancel_pending_dynamic_instanced_mesh_transform(IPLInstancedMesh mesh);
    /// Live geometry attaches to and detaches from the global scene through these; the worker applies them in
    /// order between passes. Each call takes ownership of one reference to every handle passed.
    void queue_scene_static_mesh_add(IPLStaticMesh mesh);
    void queue_scene_static_mesh_remove(IPLStaticMesh mesh);
    void queue_scene_instanced_mesh_add(IPLInstancedMesh mesh);
    /// sub_scene is released once the instance has left the global scene.
    void queue_scene_instanced_mesh_remove(IPLInstancedMesh mesh, IPLScene sub_scene);
    /// Sets material slot 0 of mesh and takes ownership of one reference to mesh and sub_scene. sub_scene is the
    /// dynamic geometry's private scene that holds mesh, or null when mesh lives in the global scene.
    void queue_scene_static_mesh_material(IPLStaticMesh mesh, IPLScene sub_scene, const IPLMaterial& material);
    void apply_queued_scene_mesh_changes_assume_locked();

    /// Main thread. World used for custom-scene raycasts.
    void set_physics_world(const Ref<World3D>& world);
    /// Main thread. Call after editing ResonanceMaterial files at runtime.
    void refresh_physics_materials();
    /// Merged with the listener and auto excludes for custom-scene rays.
    void set_physics_ray_exclude_rids(const TypedArray<RID>& exclude);
    /// Replaces the previous listener list, e.g. the camera's parent colliders.
    void set_listener_physics_ray_exclude_rids(const TypedArray<RID>& rids);
    /// Refcounted; players register their own colliders so custom-scene rays ignore them.
    void register_physics_ray_auto_exclude_rid(RID rid);
    void unregister_physics_ray_auto_exclude_rid(RID rid);

    /// Lock-free; for the audio thread.
    MixerReadGuard scoped_mixer_read() const { return MixerReadGuard(this); }
    /// Never from the audio thread: the worker holds this lock through RunReflections.
    void lock_simulation() { simulation_mutex.lock(); }
    void unlock_simulation() { simulation_mutex.unlock(); }
    std::unique_lock<std::mutex> scoped_simulation_lock() { return std::unique_lock<std::mutex>(simulation_mutex); }
    /// For main-thread debug drawing; retry later when it does not own the lock.
    std::unique_lock<std::mutex> try_scoped_simulation_lock() {
        return std::unique_lock<std::mutex>(simulation_mutex, std::try_to_lock);
    }
    String get_version();
    bool is_initialized() const;
    bool is_simulating() const;
    /// True once the current geometry is committed. Each source also needs is_source_spatial_output_ready().
    bool is_spatial_audio_output_ready() const;
    /// Scene ready and the source's first direct result for this scene published. A negative handle checks
    /// the scene only. Lock-free; any thread.
    bool is_source_spatial_output_ready(int32_t handle) const;
    /// Main thread. Closes every source's gate until the scene is committed again and each source has a new
    /// direct result.
    void arm_spatial_audio_output_gate();
    int get_global_triangle_count() const {
        return global_triangle_count.load(std::memory_order_acquire);
    }
    /// Mutes audio and joins the worker before geometry and probe volumes leave the tree on quit. Idempotent;
    /// shutdown() still releases IPL. Not for despawning single meshes or volumes.
    void begin_tree_teardown();
    /// Lets reflections and pathing run and wakes the worker once for the commit and first IR.
    void finish_cold_start_settle();
    bool is_cold_start_settle_pending() const {
        return cold_start_settle_pending_.load(std::memory_order_acquire);
    }
    int get_sample_rate() const { return current_sample_rate; }
    int get_audio_frame_size() const { return frame_size; }
    int get_direct_speaker_channels() const { return direct_speaker_channels; }
    int get_ambisonic_order() const { return ambisonic_order; }
    /// Audio thread. Ignored unless the frame size was Auto.
    void request_reinit_with_frame_size(int detected_frame_count);
    /// Main thread. Returns 0 if none is pending.
    int consume_pending_reinit_frame_size();
    bool get_audio_frame_size_was_auto() const { return audio_frame_size_was_auto_.load(std::memory_order_acquire); }
    Dictionary get_reverb_bus_instrumentation() const;
    void reset_reverb_bus_instrumentation();
    Dictionary get_pathing_instrumentation() const;
    void reset_pathing_instrumentation();
    /// Microseconds per stage of the last worker tick.
    Dictionary get_simulation_worker_timing() const;
    Dictionary get_resource_pool_stats() const;
    /// Settings a player's shared reflection effect is built with under the current config.
    ReflectionEffectRecipe reflection_effect_recipe() const;
    /// Main thread. nullptr on a miss; the caller builds the effect itself.
    IPLReflectionEffect take_pooled_reflection_effect();
    /// Main thread. Takes ownership of an effect built from recipe on context.
    void give_back_reflection_effect(IPLContext context, const ReflectionEffectRecipe& recipe, IPLReflectionEffect effect);
    /// Lock-free and approximate; cheap enough to poll every frame.
    int32_t get_active_source_count() const;
    /// Lock-free and approximate; cheap enough to poll every frame.
    int32_t get_active_probe_batch_count() const;
    /// Microseconds of the last audio block's convolution stages.
    Dictionary get_convolution_audio_timing() const;
    void record_convolution_reflection_apply_usec(uint64_t us);
    void record_convolution_reverb_bus_usec(uint64_t us);
    void record_mixer_sanitize_ambi_usec(uint64_t us);
    void record_mixer_sanitize_stereo_usec(uint64_t us);
    /// scene_type, effective rayBatchSize and whether the custom scene uses batched trace callbacks.
    Dictionary get_simulation_tracer_profile() const;
    void record_pathing_player_gate_enter();
    void record_pathing_player_applied();
    void record_pathing_player_fetch_miss();
    void record_mixer_feed() { reverb_mixer_feed_count.fetch_add(1, std::memory_order_relaxed); }
    /// Audio-thread safe.
    uint64_t get_mixer_feed_count() const { return reverb_mixer_feed_count.load(std::memory_order_relaxed); }
    /// input_rms is the mono input before reverb_gain.
    void record_convolution_feed(bool ir_non_null, float reverb_gain, float input_rms);
    /// pre_gain levels are measured after mixer apply and decode, post_gain after gain_db and clipping.
    void update_reverb_effect_instrumentation(bool mixer_null, bool success, int32_t frames_written,
                                              float output_peak_post_gain, float output_rms_post_gain,
                                              float output_peak_pre_gain, float output_rms_pre_gain);
    /// Whole reverb bus, before gain_db.
    float get_reverb_bus_output_rms_pre_gain() const { return reverb_effect_output_rms_pre_gain.load(std::memory_order_relaxed); }
    void record_reverb_effect_click_guard_trigger();
    void record_mixer_return_hold_last();
    void record_convolution_bus_skip_empty_mixer_apply();
    /// The click guard ducks a block when the bus peak collapses. Debug switch, on by default.
    void set_reverb_bus_click_guard_enabled(bool p_enabled);
    bool is_reverb_bus_click_guard_enabled() const;
    void set_reverb_bus_wet_ring_underrun_zero_fill(bool p_enabled);
    bool is_reverb_bus_wet_ring_underrun_zero_fill() const;
    float get_realtime_simulation_duration() const { return realtime_simulation_duration; }
    /// 0 = no cap.
    int get_convolution_ir_max_samples() const { return convolution_ir_max_samples_; }
    int get_num_channels_for_order() const { return (ambisonic_order + 1) * (ambisonic_order + 1); }
    static bool is_shutting_down() { return is_shutting_down_flag.load(std::memory_order_acquire); }
    /// True during process exit or a reinit teardown; audio-thread IPL callbacks must return early.
    static bool ipl_audio_teardown_active();

    /// Takes ResonanceRuntimeConfig.get_bake_params(); call before baking.
    void set_bake_params(Dictionary params);
    /// One pack at the identity transform; the bake then uses it instead of live geometry.
    void set_bake_static_scene_asset(const Ref<ResonanceGeometryAsset>& p_asset);
    void set_bake_static_scenes_from_assets(const TypedArray<ResonanceGeometryAsset>& assets,
                                            const TypedArray<Transform3D>& transforms);
    /// Replaces all existing static packs.
    void load_static_scene_from_asset(const Ref<ResonanceGeometryAsset>& p_asset, const Transform3D& p_transform = Transform3D());
    /// Additive. Use add_or_replace_static_pack() for packs owned by a ResonanceStaticScene node.
    void add_static_scene_from_asset(const Ref<ResonanceGeometryAsset>& p_asset, const Transform3D& p_transform = Transform3D());
    /// object_id is the ResonanceStaticScene's ObjectID.
    void add_or_replace_static_pack(uint64_t object_id, const Ref<ResonanceGeometryAsset>& p_asset, const Transform3D& p_transform);
    void remove_static_pack(uint64_t object_id);
    void clear_static_scenes();
    /// Clears and adds under one simulation_mutex hold. Empty arrays only clear.
    void replace_static_scenes_from_assets(const TypedArray<ResonanceGeometryAsset>& assets,
                                           const TypedArray<Transform3D>& transforms);
    /// Only affects the bake log: pathing follows reflections in this bake. Call before baking probes.
    void set_bake_pipeline_pathing(bool p_pathing);
    void set_bake_pipeline_active(bool p_active);
    bool is_bake_pipeline_active() const;
    void save_scene_data(String filename);
    void load_scene_data(String filename);
    /// Main thread. Call after load_scene_data() replaces the scene so every ResonanceGeometry rebuilds its meshes.
    void refresh_all_geometry_from_scene_tree();
    /// In memory only; null on failure. Works in play mode.
    Ref<ResonanceGeometryAsset> export_static_scene_to_geometry_asset(Node* scene_root);
    /// Merges all non-dynamic ResonanceGeometry under scene_root. Does not need an initialized server.
    Error export_static_scene_to_asset(Node* scene_root, const String& p_path);
    /// Writes OBJ and MTL; file_base_name has no extension.
    Error export_static_scene_to_obj(Node* scene_root, const String& file_base_name);
    /// Hashes the same input as the export, so an unchanged hash means no re-export is needed.
    int64_t get_static_scene_hash(Node* scene_root);
    /// Returns {"hash": int, "asset": ResonanceGeometryAsset or null}. asset is null when hash is 0, equals
    /// skip_if_hash, or the build fails.
    Dictionary build_static_scene_export(Node* scene_root, int64_t skip_if_hash);
    Error save_geometry_asset(const Ref<ResonanceGeometryAsset>& p_asset, const String& p_path);
    /// A changed hash means the probe bake is out of date.
    int64_t get_geometry_asset_hash(const Ref<ResonanceGeometryAsset>& p_asset) const;
    /// Debug export through iplSceneSaveOBJ; file_base_name has no extension.
    void save_scene_obj(String file_base_name);
    PackedVector3Array generate_manual_grid(const Transform3D& volume_transform, Vector3 extents, float spacing,
                                            int generation_type = 2, float height_above_floor = 1.5f);
    /// Uses iplProbeArrayGenerateProbes for uniform floor and centroid placement. Empty when there is no scene
    /// or no probe fits; bake_probes_for_volume then fails rather than falling back to a plain grid.
    PackedVector3Array generate_probes_scene_aware(const Transform3D& volume_transform, Vector3 extents, float spacing,
                                                   int generation_type, float height_above_floor);
    bool bake_manual_grid(const PackedVector3Array& points, Ref<ResonanceProbeData> probe_data_res, float spacing = 2.0f);
    /// exclusion_boxes: Array of {xform: Transform3D, size: Vector3}; probes inside them are dropped.
    bool bake_probes_for_volume(const Transform3D& volume_transform, Vector3 extents, float spacing,
                                int generation_type, float height_above_floor, Ref<ResonanceProbeData> probe_data_res,
                                const Array& exclusion_boxes = Array());
    bool bake_pathing(Ref<ResonanceProbeData> probe_data_res);
    bool bake_static_source(Ref<ResonanceProbeData> probe_data_res, Vector3 endpoint_position, float influence_radius);
    bool bake_static_listener(Ref<ResonanceProbeData> probe_data_res, Vector3 endpoint_position, float influence_radius);
    /// Safe from the bake thread. Emits no signal; the UI polls get_bake_progress().
    void emit_bake_progress(float progress);
    /// 0-1.
    float get_bake_progress() const;
    bool is_bake_cancel_requested() const { return bake_cancel_requested_.load(std::memory_order_acquire); }
    void reset_bake_cancel_requested() { bake_cancel_requested_.store(false, std::memory_order_release); }
    /// Call from another thread than the one baking.
    void cancel_reflections_bake();
    /// Call from another thread than the one baking.
    void cancel_pathing_bake();
    /// Returns -1 on failure. Pair with remove_probe_batch().
    int32_t load_probe_batch(Ref<ResonanceProbeData> probe_data_res);
    void remove_probe_batch(int32_t handle);
    void clear_probe_batches();
    /// Removes batches that no longer match reflection_type or pathing_enabled. Returns how many were removed.
    int revalidate_probe_batches_with_config();

    /// -1 when the context or data is invalid.
    int32_t editor_probe_data_get_num_probes(Ref<ResonanceProbeData> data) const;
    /// Changes data in memory only; volumes using it must reload their probe batch.
    bool editor_probe_data_remove_probe(Ref<ResonanceProbeData> data, int32_t index);
    bool editor_probe_data_remove_baked_layer(Ref<ResonanceProbeData> data, int baked_data_type, int variation, Vector3 endpoint,
                                              float influence_radius);

    void set_debug_occlusion(bool p_enabled);
    bool is_debug_occlusion_enabled() const;
    void set_debug_reflections(bool p_enabled);
    bool is_debug_reflections_enabled() const;
    void set_debug_pathing(bool p_enabled);
    bool is_debug_pathing_enabled() const;
    /// Array of {from: Vector3, to: Vector3, occluded: bool}; empty unless debug_pathing is on.
    Array get_pathing_visualization_segments();
    /// Array of {from: Vector3, to: Vector3, bounce: int}; empty unless debug_reflections and realtime rays are on.
    Array get_ray_debug_segments();
    /// Traces from origin instead of the stored listener pose, which lags one frame.
    Array get_ray_debug_segments_at(Vector3 origin);
    bool uses_custom_ray_tracer() const;
    bool wants_debug_reflection_viz() const { return debug_reflections.load(std::memory_order_relaxed) && max_rays > 0; }
    bool wants_debug_pathing_viz() const { return pathing_enabled && debug_pathing.load(std::memory_order_relaxed); }
    int register_debug_mesh(const std::vector<IPLVector3>& vertices, const std::vector<IPLTriangle>& triangles,
                            const IPLint32* material_indices, const IPLMatrix4x4* transform, const IPLMaterial* material);
    void unregister_debug_mesh(int mesh_id);
    void set_output_direct_enabled(bool p_enabled);
    bool is_output_direct_enabled() const;
    void set_output_reverb_enabled(bool p_enabled);
    bool is_output_reverb_enabled() const;
    void set_perspective_correction_enabled(bool p_enabled);
    bool is_perspective_correction_enabled() const;
    void set_perspective_correction_factor(float p_factor);
    float get_perspective_correction_factor() const;
    int get_reflection_type() const { return reflection_type; }
    int get_realtime_rays() const { return max_rays; }
    /// Applies scheduling and adaptive settings without a reinit.
    void patch_live_runtime_scheduling(const Dictionary& config);
    /// Applies the live-patchable keys. Returns true when the caller must reinit, e.g. pathing turned on but the
    /// simulator was created without it.
    bool patch_live_runtime_config(Dictionary config);
    static bool runtime_config_property_requires_engine_reinit(const StringName& property);
    static bool runtime_config_property_is_live_patchable(const StringName& property);
    static bool runtime_config_property_requires_routing_refresh(const StringName& property);
    int get_transmission_type() const { return transmission_type; }
    int get_max_transmission_surfaces() const { return max_transmission_surfaces; }
    int get_occlusion_type() const { return occlusion_type; }
    bool use_direct_binaural() const;
    bool use_reverb_binaural() const;
    bool use_pathing_binaural() const;
    bool use_virtual_surround_output() const { return use_virtual_surround; }
    bool get_hrtf_interpolation_bilinear() const { return hrtf_interpolation_bilinear; }
    bool is_pathing_enabled() const { return pathing_enabled; }
    bool was_simulator_created_with_pathing() const { return simulator_created_with_pathing_; }
    /// Defaults for players on Use Global (-1).
    bool get_default_path_validation_enabled() const { return path_validation_enabled; }
    bool get_default_find_alternate_paths() const { return find_alternate_paths; }
    /// C++ only. A null callback means Steam Audio's default (UTD) model.
    void set_pathing_deviation_callback(IPLDeviationCallback callback, void* userData);
    void clear_pathing_deviation_callback();
    /// callable(angle_rad, band_index) -> attenuation 0-1, sampled into a lookup table on the main thread. An empty
    /// callable restores the default (UTD) model.
    void set_pathing_deviation_callable(const Callable& callable, int samples_per_band = resonance::kPathingDeviationDefaultLutSamples);
    bool is_pathing_deviation_custom_enabled();
    void set_pathing_enabled(bool p_enabled);
    bool are_pathing_results_current() const { return pathing_results_current_.load(std::memory_order_acquire); }

    float calculate_distance_attenuation(Vector3 source_pos, Vector3 listener_pos, float min_dist, float max_dist);
    Vector3 calculate_air_absorption(Vector3 source_pos, Vector3 listener_pos);
    float calculate_directivity(Vector3 source_pos, Vector3 source_forward, Vector3 source_up, Vector3 source_right, Vector3 listener_pos, float weight, float power);

    void notify_geometry_changed(int triangle_delta);
    /// Caller holds simulation_mutex.
    void notify_geometry_changed_assume_locked(int triangle_delta);
    /// Caller holds simulation_mutex. For changes that keep the triangle count, like instance transforms.
    void mark_scene_commit_pending_assume_locked();
    /// Does not take simulation_mutex, so the main thread never waits for the worker.
    void mark_scene_commit_pending();
    void update_listener(Vector3 pos, Vector3 dir, Vector3 up);
    void set_listener_valid(bool valid);
    /// Keeps no listener node. Without ResonanceRuntime or ResonanceListener, call notify_listener_changed_to()
    /// or update_listener() and set_listener_valid() every frame.
    void notify_listener_changed();
    /// Call every frame or whenever the node moves.
    void notify_listener_changed_to(Node* listener_node);
    void tick(float delta);
    /// Same as enqueue_source_update().
    void update_source(int32_t handle, const SourceUpdateParams& params);
    /// Applies at once if simulation_mutex is free, else returns false.
    bool try_update_source(int32_t handle, const SourceUpdateParams& params);
    /// Latest update per handle wins.
    void enqueue_source_update(int32_t handle, const SourceUpdateParams& params);
    /// Uses try_lock; if the worker holds the lock the updates stay queued and the worker drains them.
    void flush_pending_source_updates();
    bool uses_batch_source_updates() const { return batch_source_updates; }
    /// For attenuation_mode 1 (linear) or 2 (curve); call before update_source().
    void set_source_attenuation_callback_data(int32_t handle, int attenuation_mode, float min_distance, float max_distance, const PackedFloat32Array& curve_samples);
    void clear_source_attenuation_callback_data(int32_t handle);
    /// Label used in pathing batch warnings, usually the node path. Cleared on destroy.
    void set_source_pathing_owner_path(int32_t handle, const String& owner_path);

    /// pathing_enabled_override: -1 = Use Global, 0 = off, 1 = on; it applies to the first inputs, so sources
    /// without pathing never warn about missing pathing data. listener_reverb_slot uses the reserved slot.
    int32_t create_source_handle(Vector3 position, float radius, const String& pathing_owner_path = String(),
                                 int pathing_enabled_override = -1, bool listener_reverb_slot = false);
    /// True while the cap error was logged and the source count is still at max_simulation_sources.
    bool simulation_source_cap_blocks_create() const;
    void destroy_source_handle(int32_t handle);
    IPLSource get_source_from_handle(int32_t handle);
    /// Changes on every reinit; a handle from an older epoch is dead even if its id was reused.
    uint32_t get_source_lifecycle_epoch() const { return source_table_.lifecycle_epoch(); }
    /// Same counter as get_source_lifecycle_epoch(); probe batch ids are recycled in the same shutdown.
    uint32_t get_probe_batch_lifecycle_epoch() const { return get_source_lifecycle_epoch(); }

    OcclusionData get_source_occlusion_data(int32_t handle);
    /// Lock-free, without building a Dictionary; for audio bridges on the main thread.
    float get_source_occlusion_linear_gain(int32_t handle);
    Dictionary get_source_occlusion_data_dict(int32_t handle);
    /// For external emitters such as FMOD that only move.
    void update_source_position(int32_t handle, Vector3 position, float radius,
                                bool use_sim_distance_attenuation = false, float min_distance = 1.0f);
    /// Cheap hint from the last reflection sync; can be false for a moment after the source is created.
    /// Use fetch_reverb_params() when the params are needed.
    bool peek_reverb_params_likely_available(int32_t handle) const;
    bool fetch_reverb_params(int32_t handle, IPLReflectionEffectParams& out_params, bool* out_epoch_fresh = nullptr);
    uint32_t get_source_ir_generation(int32_t handle) const;
    /// Audio-thread safe.
    uint32_t get_reflection_param_cache_epoch() const;
    /// Audio-thread safe.
    uint32_t get_pathing_param_cache_epoch() const;
    /// On success out.params.shCoeffs points at out.shCoeffs, so out must outlive the params.
    bool fetch_pathing_params(int32_t handle, FetchedPathingParams& out);
    uint16_t get_reflection_baked_energy_q16(int32_t handle) const;
    float probe_data_static_source_energy_at(Ref<ResonanceProbeData> data, Vector3 endpoint, Vector3 listener, float influence_radius,
                                             float neighbor_radius = 0.0f);
    /// Interpolates a baked layer from the probes at a world point; reconstruct_ir adds an IR preview.
    Dictionary probe_data_query_baked_at_point(Ref<ResonanceProbeData> data, Vector3 world_position, int baked_variation, Vector3 endpoint,
                                               float influence_radius, float neighbor_radius, bool reconstruct_ir);
    Dictionary probe_data_query_baked_at_probe(Ref<ResonanceProbeData> data, int32_t probe_index, int baked_variation, Vector3 endpoint,
                                               float influence_radius, bool reconstruct_ir);
};

} // namespace godot

#endif