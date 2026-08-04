#ifndef RESONANCE_PLAYER_H
#define RESONANCE_PLAYER_H

#include <godot_cpp/classes/audio_frame.hpp>
#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_playback.hpp>
#include <godot_cpp/classes/audio_stream_player3d.hpp>
#include <godot_cpp/classes/curve.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/templates/safe_refcount.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include "resonance_constants.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <phonon.h>
#include <vector>

#include <godot_cpp/variant/rid.hpp>

#include "resonance_constants.h"
#include "resonance_debug_drawer.h"
#include "resonance_directivity_drawer.h"
#include "resonance_mixer_processor.h"
#include "resonance_processor_direct.h"
#include "resonance_processor_path.h"
#include "resonance_processor_reflection.h"
#include "resonance_ring_buffer.h"
#include "resonance_server.h"

#include <godot_cpp/variant/packed_vector3_array.hpp>

namespace godot {

class ResonancePlayer;

struct PlaybackParameters {
    int32_t source_handle = -1;        // ResonanceServer simulation source handle
    float occlusion = 1.0f;            // Steam: 1=line-of-sight, 0=occluded (see direct_simulator / direct_effect)
    float transmission[3] = {1, 1, 1}; // Wall transparency
    float attenuation = 1.0f;          // Distance attenuation factor (direct path only)
    float distance = 0.0f;             // Source-listener distance (meters)
    bool enable_direct = true;         // Enable direct sound processing
    bool enable_reverb = true;         // Enable reverb processing
    bool has_valid_reverb = false;     // Whether the reverb parameters are valid

    Vector3 source_position = Vector3(0, 0, 0); // Source position in world space
    bool use_binaural = true;                   // Use HRTF binaural processing
    float spatial_blend = 1.0f;                 // 0=unspatialized, 1=fully spatialized (e.g. for diegetic/non-diegetic blend)
    bool use_ambisonics_encode = false;         // Encode point source to Ambisonics before binaural (for mixing scenarios)

    bool apply_air_absorption = false;                       // Enable air absorption effect
    resonance::AudioBands3 air_absorption{1.0f, 1.0f, 1.0f}; // Air absorption coefficients (Low, Mid, High)

    bool apply_directivity = false; // Enable directivity effect
    float directivity_value = 1.0f; // Directivity factor (0.0 = Omni, 1.0 = Dipole)

    IPLCoordinateSpace3 listener_orientation{};

    // Per-path gains (0 = off, 1 = full) - gate simulation bands and wet sends.
    float direct_mix_level = 1.0f;
    float reflections_mix_level = 1.0f;
    float pathing_mix_level = 1.0f;
    bool apply_hrtf_to_reflections = true;
    bool apply_hrtf_to_pathing = true;

    // Per-source hybrid reverb overrides. Only applied when reflection_type is Hybrid. -1 = use simulation value.
    float reflections_eq[3] = {1.0f, 1.0f, 1.0f}; // EQ multipliers for parametric part (1.0 = no change)
    int reflections_delay = -1;                   // Samples before parametric starts; -1 = use simulation

    // Parametric/Hybrid split output: when true, reverb goes to reverb ring instead of main output.
    bool reverb_split_output = false;

    /// Direct effect: 0 = freq-independent transmission, 1 = freq-dependent (3-band).
    int direct_effect_transmission_type = 0;
    /// Direct effect HRTF interpolation: false = nearest, true = bilinear.
    bool direct_effect_hrtf_bilinear = false;

    /// Wet-path occlusion damping (main thread); 1.0 for realtime/static bake modes that already encode geometry.
    float wet_occlusion_factor = 1.0f;

    /// When true, run the wet mono tap through the air-absorption pre-EQ in ResonanceReflectionProcessor.
    /// Only set for baked reflection modes (variation >= 0) - realtime IRs already encode air absorption per ray.
    bool apply_air_absorption_to_wet = false;
};

class ResonanceStreamPlayback : public AudioStreamPlayback {
    GDCLASS(ResonanceStreamPlayback, AudioStreamPlayback)
    friend class ResonancePlayer;

  private:
    /// AudioStreamPlayer3D volume is not applied by the engine to this GDExtension playback's _mix output; scale explicitly.
    ResonancePlayer* owner_player_ = nullptr;

    static const int kMaxBlocksPerMixCall = 4;           // Limit steam-audio blocks per _mix (callback budget)
    int frame_size_ = resonance::kGodotDefaultFrameSize; // IPL frame size from server (256/512/1024/2048)
    /// Direct spatializer output channels (1/2/4/6/8); Godot _mix is still stereo (fold-down here).
    int direct_out_channels_ = 2;
    Ref<AudioStreamPlayback> base_playback;

    std::atomic<bool> params_dirty = false;
    /// Set after the first `update_parameters` each run - _mix stays silent until then to avoid a one-frame full-gain burst.
    std::atomic<bool> params_ever_synced_{false};
    PlaybackParameters params_next;
    PlaybackParameters params_current;

    bool is_initialized = false;
    /// Audio sets when IPL context handle drifts; Main calls resolve_stale_steam_context_on_main().
    std::atomic<bool> steam_context_stale_{false};
    int current_sample_rate = 44100;

    /// Extra retain for the active sim source; acquired on main in update_parameters, released on handle change/cleanup.
    std::atomic<uintptr_t> retained_ipl_source_{0};
    std::atomic<int32_t> retained_source_handle_{-1};
    int32_t current_source_handle = -1;
    IPLContext context = nullptr;

    // --- MODULAR PROCESSORS ---
    ResonanceDirectProcessor direct_processor;
    ResonanceReflectionProcessor reflection_processor;
    ResonancePathProcessor path_processor;
    ResonanceMixerProcessor mixer_processor;

    // Main Buffers (Bridge between Godot & Processors)
    IPLAudioBuffer sa_in_buffer{};
    IPLAudioBuffer sa_direct_out_buffer{}; // Output from Direct Processor
    IPLAudioBuffer sa_path_out_buffer{};   // Output from Path Processor
    IPLAudioBuffer sa_final_mix_buffer{};  // Mixed result

    RingBuffer<float> input_ring_l;
    RingBuffer<float> input_ring_r;
    RingBuffer<float> output_ring_l;
    RingBuffer<float> output_ring_r;
    RingBuffer<float> output_ring_reverb_l;
    RingBuffer<float> output_ring_reverb_r;

    // One IPL frame of stereo samples (`frame_size_`, max `kMaxAudioFrameSize`)
    std::vector<float> temp_process_buffer_l;
    std::vector<float> temp_process_buffer_r;

    // Reusable buffers for read_reverb_frames (avoids allocation in audio path)
    std::vector<float> temp_reverb_buffer_l;
    std::vector<float> temp_reverb_buffer_r;
    // Click guard for split-reverb output: fade to zero when ring runs dry mid-callback.
    float reverb_ring_prev_l_ = 0.0f;
    float reverb_ring_prev_r_ = 0.0f;
    bool reverb_ring_prev_valid_ = false;
    /// Reverb bus ring underrun (`to_read < frames`): one cosine taper across callbacks - avoids LF swallow + pumping.
    bool reverb_ring_gap_fade_armed_ = false;
    int reverb_ring_gap_fade_index_ = 0;
    int reverb_ring_gap_fade_total_ = 0;
    /// Last stereo samples written to Godot's `buffer` in `_mix` (post volume). Used when the output ring is empty
    /// mid-callback (`to_copy == 0` or `valid_copy == 0`) so linear fade starts from the previous tick, not a hard zero.
    float last_mix_out_l_ = 0.0f;
    float last_mix_out_r_ = 0.0f;
    bool last_mix_out_valid_ = false;
    /// Last decoded samples fed to input rings - blends into the next chunk after partial `mix_audio`.
    float input_ring_tail_l_ = 0.0f;
    float input_ring_tail_r_ = 0.0f;
    bool input_ring_tail_valid_ = false;
    /// True after a mix where `samples_read < frames` (partial decode + hold pad). Chunk crossfade runs only then.
    bool prev_mix_had_partial_input_pad_ = false;
    /// True after a mix that tapered the input-ring pad to silence at EOS (`!is_playing`). Chunk crossfade must not
    /// blend with `input_ring_tail_*` (last *dry* sample): the ring actually ended near zero - crossfade would step.
    bool prev_mix_had_eos_tapered_input_pad_ = false;
    /// Linear fade-in on host buffer samples after `_start` (`kPlaybackHostFadeInMs` → samples at `current_sample_rate`).
    int playback_host_fade_in_elapsed_ = 0;
    int playback_host_fade_in_total_samples_ = 1;
    /// Linear fade-out countdown after `_stop` / soft stop (`kPlaybackHostFadeOutMs` → samples).
    int playback_host_fade_out_remaining_ = 0;
    int playback_host_fade_out_total_samples_ = 1;

    void apply_playback_host_fades(AudioFrame* buffer, int32_t frames);

    // Mix ramps: direct weight; reflections (-1 = first block skip ramp on conv path); parametric/path track previous wet scale.
    float prev_direct_weight = 0.0f;
    float prev_conv_reflections_mix_level_ = -1.0f;      // -1 = first conv/TAN block uses constant mix (no crossfade from stale state)
    float prev_parametric_reflections_mix_level_ = 0.0f; // parametric/hybrid wet ramp state
    float prev_pathing_mix_level_ = 0.0f;

    // Throttle "no reverb params" warning: skip first 3, then log only every 200+ misses (reset after log)
    int no_reverb_warn_count = 0;

    // Input-start detection: delay processing until first non-zero sample
    // to avoid ramp artifacts when Godot sends incorrect params before playback actually starts.
    bool input_started = false;

    // Parametric pathing fallback: persistent sh coeffs when baked path fails (order 1 = 4 coeffs)
    float parametric_path_sh_coeffs[4];

    IPLReflectionEffectParams reflection_tail_params_{};
    bool reflection_tail_have_params_ = false;
    float reflection_tail_wet_gain_ = 1.0f;
    bool reflection_tail_split_output_ = false;
    /// Conv/TAN: after dry ends, drive Apply with silence until internal tail drains (shared mixer path).
    bool conv_reverb_eos_silence_apply_done_ = false;

    /// Cached path params for EOS tail (Apply silence each tick); SH coeffs copied to `pathing_tail_sh_coeffs_`.
    IPLPathEffectParams pathing_tail_params_{};
    bool pathing_tail_have_params_ = false;
    /// Stable storage for tail SH coeffs (max order 3 => 16 coeffs). Avoids dangling pointers when server swaps caches.
    std::array<float, 16> pathing_tail_sh_coeffs_{};

    /// Set when the dry signal has been halted (either via Godot's _stop() or via a soft-stop
    /// request from ResonancePlayer::stop()). Marks the start of the tail-drain window: _mix
    /// continues to be called by Godot until _is_playing() returns false, which only happens
    /// once the parametric/convolution/hybrid reverb tail and the path tail have decayed
    /// or the grace block budget is exhausted.
    std::atomic<bool> stop_requested_{false};

    /// Decremented per tail block produced after the dry signal ends. -1 = grace not yet
    /// armed (still in normal play); >0 = tail blocks remaining; 0 = exhausted, force end.
    /// Hard cap to prevent pathological cases (degenerate effect handles, stalled mixer)
    /// from keeping a playback alive indefinitely. Initial budget is derived from
    /// ResonanceServer::get_max_reverb_duration() on the first transition into the tail
    /// branch (samples_read == 0).
    std::atomic<int64_t> tail_grace_blocks_remaining_{-1};

    /// Set to true once the tail-drain window is complete (rings empty and grace exhausted).
    /// This playback intentionally does not "naturally" end (no return 0) because `finished`
    /// is emitted on dry-EOS by ResonancePlayer. ResonancePlayer will explicitly stop the node.
    std::atomic<bool> tail_drain_complete_{false};

    // --- AUDIO INSTRUMENTATION (for dropout debugging) ---
    // Atomic counters updated from audio thread; read from main thread
    std::atomic<uint64_t> instrumentation_input_dropped{0};      // Samples dropped when input ring full
    std::atomic<uint64_t> instrumentation_output_underrun{0};    // Output frames filled with silence
    std::atomic<uint64_t> instrumentation_output_blocked{0};     // Processing skipped (output ring full)
    std::atomic<uint64_t> instrumentation_mix_call_count{0};     // Total _mix calls
    std::atomic<uint64_t> instrumentation_blocks_processed{0};   // Blocks processed (512 frames each)
    std::atomic<uint64_t> instrumentation_passthrough_blocks{0}; // Blocks in passthrough (no source handle)
    std::atomic<uint64_t> instrumentation_reverb_miss_blocks{0}; // Wanted reverb but fetch_reverb_params=false
    std::atomic<uint64_t> instrumentation_max_block_time_us{0};  // Max _process_steam_audio_block duration (us)
    std::atomic<uint64_t> instrumentation_last_block_time_us{0}; // Last block duration (us)
    std::atomic<uint64_t> instrumentation_late_mix_count{0};     // _mix calls with inter-callback time >15ms
    /// Last measured inter-callback gap (microseconds). Helps determine if late_mix_count is real or threshold mismatch.
    std::atomic<uint64_t> instrumentation_last_mix_gap_us_{0};
    /// Maximum measured inter-callback gap (microseconds) since last reset.
    std::atomic<uint64_t> instrumentation_max_mix_gap_us_{0};
    /// Expected callback gap from requested frames and current sample rate (microseconds).
    std::atomic<uint64_t> instrumentation_expected_mix_gap_us_{0};
    std::atomic<uint64_t> instrumentation_param_sync_count{0};                                // Times params were synced (params_dirty)
    std::atomic<uint64_t> instrumentation_zero_input_count{0};                                // _mix calls with samples_read==0 (tail drain)
    std::atomic<int32_t> instrumentation_mix_frames_min{std::numeric_limits<int32_t>::max()}; // Min samples_read per _mix (when >0)
    std::atomic<int32_t> instrumentation_mix_frames_max{0};                                   // Max samples_read per _mix
    std::atomic<uint64_t> instrumentation_silent_output_blocks{0};                            // Processed blocks with output RMS < 0.0001
    std::atomic<uint32_t> instrumentation_last_output_rms_q8{0};                              // Last block output RMS * 256 (fixed-point for display)
    /// Pathing diagnostics (last audio block): SH coeff RMS sqrt(mean(c^2)), sum(c^2), path effect stereo RMS before add to mix
    std::atomic<float> instrumentation_last_pathing_sh_rms{0.0f};
    std::atomic<float> instrumentation_last_pathing_sh_energy{0.0f};
    std::atomic<float> instrumentation_last_pathing_out_rms{0.0f};
    std::atomic<int32_t> instrumentation_last_pathing_order{-1}; // -1 = n/a this block
    std::atomic<float> debug_signal_direct{0.0f};                // Effective direct gain (for Debug Sources display)
    std::atomic<float> debug_signal_reverb{0.0f};                // Effective reverb gain (for Debug Sources display)
    std::atomic<float> debug_signal_pathing{0.0f};               // Path wet stereo RMS after path sim (per block, clamped 0..1 for HUD)
    /// Convolution/TAN: `IPLReflectionMixer` was null (cannot feed shared wet bus).
    std::atomic<uint64_t> instrumentation_conv_mixer_null_blocks{0};
    /// Convolution/TAN: `process_mix` returned false (wet feed rejected for that Steam block).
    std::atomic<uint64_t> instrumentation_conv_mix_failed_blocks{0};
    /// Blocks with a valid sim source where `enable_reverb` in PlaybackParameters was false (reverb send gated off).
    std::atomic<uint64_t> instrumentation_enable_reverb_false_blocks{0};
    std::chrono::steady_clock::time_point last_mix_time_; // For inter-callback timing (audio thread only)

    void _lazy_init_steam_audio(int sampling_rate); // Alloc IPL processors/buffers on first need
    void _cleanup_steam_audio();
    void _process_steam_audio_block(); // One `frame_size_` IPL pipeline step
    float _debug_sa_in_mono_rms() const;
    bool _feed_convolution_mixer(ResonanceServer* srv, IPLReflectionEffectParams& params, float curr_refl_mix,
                                 float refl_wet_output_gain, bool store_tail_params, float& out_dbg_reverb);
    void _apply_reflections_wet(ResonanceServer* srv, float wet_occ, float refl_wet_output_gain,
                                bool& out_reverb_to_player, float& out_dbg_reverb);
    float _apply_pathing_wet(ResonanceServer* srv, const IPLCoordinateSpace3& listener_cs);
    void _process_passthrough_block();
    void _sync_params(); // Sync parameters from next to current
    void _retain_source_for_main(int32_t handle);
    void _release_retained_source();
    void _add_reverb_to_output(IPLAudioBuffer* reverb_buf, float refl_mix, bool split_output,
                               const IPLCoordinateSpace3& listener_coords); // Ambisonic decode must match reverb bus listener
    void _write_output_rings_folded();                                      // sa_final_mix_buffer (N ch) -> stereo rings via temp_process_buffer_*
    void _zero_sa_final_mix();                                              // memset all direct_out_channels_
    void internal_orphan_owner_player() { owner_player_ = nullptr; }

  public:
    ResonanceStreamPlayback();
    ~ResonanceStreamPlayback();

    /// Called by ResonanceServer before iplContextRelease (userdata = this).
    static void ipl_context_reinit_cleanup(void* userdata);

    ResonanceStreamPlayback(const ResonanceStreamPlayback&) = delete;
    ResonanceStreamPlayback(ResonanceStreamPlayback&&) = delete;

    void set_base_playback(const Ref<AudioStreamPlayback>& p_playback);
    void set_owner_player(ResonancePlayer* p_player) { owner_player_ = p_player; }
    void update_parameters(const PlaybackParameters& p_params);

    virtual int32_t _mix(AudioFrame* buffer, float rate_scale, int32_t frames) override; // Mixes audio frames into the buffer
    virtual void _start(double from_pos) override;                                       // Starts playback from a specific position

    /// Alloc IPL effects/buffers before first _mix (avoids audio-thread stall); idempotent.
    bool prewarm_steam_audio();
    /// Main thread: if audio flagged a stale IPL context, teardown + prewarm (no cleanup in _mix).
    void resolve_stale_steam_context_on_main();
    /// True if the audio-side initialisation is complete and _mix will skip the lazy path.
    bool is_steam_audio_initialised() const { return is_initialized; }

    /// Debug Sources: last block - direct gain, reverb send, path wet output RMS (clamped 0..1). Safe from main thread.
    void get_debug_signal_levels(float& out_direct, float& out_reverb, float& out_pathing) const;
    /// Snapshot of instrumentation counters for debugging dropouts. Safe to call from main thread.
    void get_instrumentation_snapshot(uint64_t& out_input_dropped, uint64_t& out_output_underrun,
                                      uint64_t& out_output_blocked, uint64_t& out_mix_calls, uint64_t& out_blocks_processed,
                                      uint64_t& out_passthrough_blocks, uint64_t& out_reverb_miss_blocks, uint64_t& out_max_block_time_us,
                                      uint64_t& out_last_block_time_us,
                                      uint64_t& out_late_mix, uint64_t& out_last_mix_gap_us, uint64_t& out_max_mix_gap_us,
                                      uint64_t& out_expected_mix_gap_us, uint64_t& out_param_syncs, uint64_t& out_zero_input,
                                      int32_t& out_mix_frames_min, int32_t& out_mix_frames_max,
                                      uint64_t& out_silent_blocks, float& out_last_rms,
                                      float& out_pathing_sh_rms, float& out_pathing_sh_energy, float& out_pathing_out_rms,
                                      int32_t& out_pathing_order,
                                      uint64_t& out_conv_mixer_null_blocks, uint64_t& out_conv_mix_failed_blocks,
                                      uint64_t& out_enable_reverb_false_blocks) const;
    /// Reset all instrumentation counters. Call from main thread to clear and re-observe.
    void reset_instrumentation();
    /// Split convolution wet ring depth (for ResonanceReverbPlayback after main stream stops).
    size_t get_reverb_ring_available_read() const { return output_ring_reverb_l.get_available_read(); }

    /// True if the per-source effects still have tail samples to emit, or if the output
    /// rings still hold queued samples. Used by _is_playing() to keep the playback alive
    /// during the reverb / pathing tail decay after the dry signal ends.
    bool has_active_tail_residue() const;

    /// Soft-stop request from main thread: halts base_playback so no further dry samples
    /// are produced, but keeps the playback object alive long enough for the reverb /
    /// pathing tail to ring out. Godot keeps invoking _mix as long as _is_playing()
    /// returns true. Called by ResonancePlayer::stop() in place of AudioStreamPlayer3D::stop().
    void request_soft_stop();

    /// Fills buffer with reverb frames (or silence if unavailable). Always returns frames. Called from reverb playback _mix.
    int32_t read_reverb_frames(AudioFrame* buffer, int32_t frames);
    virtual void _stop() override;                // Stops playback
    virtual bool _is_playing() const override;    // Checks if playback is active
    virtual int _get_loop_count() const override; // Returns the loop count for the playback
    virtual void _seek(double position) override; // Seeks to a specific position in the stream

    bool is_tail_drain_complete() const { return tail_drain_complete_.load(std::memory_order_acquire); }

  protected:
    static void _bind_methods() {}
};

class ResonanceStream : public AudioStream {
    GDCLASS(ResonanceStream, AudioStream)
  private:
    Ref<AudioStream> base_stream;
    ResonancePlayer* stream_owner_ = nullptr;

  public:
    void set_base_stream(const Ref<AudioStream>& p_stream);
    void set_stream_owner(ResonancePlayer* p_player) { stream_owner_ = p_player; }
    virtual Ref<AudioStreamPlayback> _instantiate_playback() const override;
    virtual String _get_stream_name() const override { return "Resonance"; }
    virtual double _get_length() const override { return base_stream.is_valid() ? base_stream->get_length() : 0.0; }
    virtual bool _is_monophonic() const override { return false; }

  protected:
    static void _bind_methods() {}
};

class ResonanceReverbPlayback : public AudioStreamPlayback {
    GDCLASS(ResonanceReverbPlayback, AudioStreamPlayback)
  private:
    ResonancePlayer* parent_player = nullptr;

  public:
    ResonanceReverbPlayback() = default;
    ResonanceReverbPlayback(const ResonanceReverbPlayback&) = delete;
    ResonanceReverbPlayback(ResonanceReverbPlayback&&) = delete;
    void set_parent_player(ResonancePlayer* p_player);
    virtual int32_t _mix(AudioFrame* buffer, float rate_scale, int32_t frames) override;
    virtual void _start(double from_pos) override;
    virtual void _stop() override;
    virtual bool _is_playing() const override;
    virtual int _get_loop_count() const override;
    virtual void _seek(double position) override;

  protected:
    static void _bind_methods();
};

class ResonanceReverbStream : public AudioStream {
    GDCLASS(ResonanceReverbStream, AudioStream)
  private:
    ResonancePlayer* parent_player = nullptr;

  public:
    ResonanceReverbStream() = default;
    ResonanceReverbStream(const ResonanceReverbStream&) = delete;
    ResonanceReverbStream(ResonanceReverbStream&&) = delete;
    void set_parent_player(ResonancePlayer* p_player);
    virtual Ref<AudioStreamPlayback> _instantiate_playback() const override;
    virtual String _get_stream_name() const override { return "ResonanceReverb"; }
    virtual double _get_length() const override { return 0.0; }
    virtual bool _is_monophonic() const override { return false; }

  protected:
    static void _bind_methods();
};

class ResonancePlayer : public AudioStreamPlayer3D {
    GDCLASS(ResonancePlayer, AudioStreamPlayer3D)
    friend class ResonanceStreamPlayback;
    friend class ResonanceReverbPlayback;

  public:
    enum AttenuationMode {
        ATTENUATION_INVERSE,      // Inverse distance + IPL DIRECT distance attenuation when enabled
        ATTENUATION_LINEAR,       // Linear falloff to 0 at max_dist
        ATTENUATION_CUSTOM_CURVE, // User defined curve
        ATTENUATION_DISABLED,     // No distance rolloff on direct or reflection/pathing wet
    };

  private:
    Ref<ResonanceStream> internal_stream;
    /// User-facing stream when player_config is set (serialization / inspector); engine stream is ResonanceStream.
    Ref<AudioStream> logical_stream_;
    /// Active voices (main thread mutates under mutex; audio reads published snapshot only).
    struct PlaybackVoiceSnapshot {
        int count = 0;
        std::array<ResonanceStreamPlayback*, resonance::kMaxPlayerPolyphonySnapshot> voices{};
    };
    mutable std::mutex internal_playbacks_mutex_;
    std::vector<ResonanceStreamPlayback*> internal_playbacks_;
    PlaybackVoiceSnapshot playback_snap_[2];
    std::atomic<int> playback_snap_front_{0};
    void internal_publish_playback_snapshot();
    void internal_get_playback_snapshot_for_audio(PlaybackVoiceSnapshot& out) const;
    void internal_register_playback(ResonanceStreamPlayback* p);
    void internal_unregister_playback(ResonanceStreamPlayback* p);
    void internal_copy_internal_playbacks(std::vector<ResonanceStreamPlayback*>& out) const;
    void _broadcast_update_parameters(const PlaybackParameters& p);
    /// Release playback IPLSource retains before [method ResonanceServer::destroy_source_handle].
    void _detach_playback_source_retains();

    int32_t source_handle = -1;
    /// Captured from ResonanceServer::get_source_lifecycle_epoch() at create; mismatch => stale after reinit.
    uint32_t source_lifecycle_epoch_ = 0;
    // Cached effective volume (linear), updated on main thread, read on audio thread.
    std::atomic<float> cached_effective_volume_linear_{1.0f};
    NodePath pathing_probe_volume; // Scene-specific: which ProbeVolume to use for pathing
    Ref<Resource> player_config;   // ResonancePlayerConfig; null = behave as plain AudioStreamPlayer3D

    // Config cache: refresh every N frames to reduce _config_* overhead while still reflecting edits
    static const int kConfigCacheRefreshInterval = 15;
    int config_cache_frame_countdown = 0;
    struct ConfigCache {
        float min_distance, max_distance, source_radius;
        int attenuation_mode;
        Ref<Curve> attenuation_curve;
        bool air_absorption_enabled;
        int air_absorption_input;
        float air_absorption_low, air_absorption_mid, air_absorption_high;
        int direct_binaural_override;
        bool directivity_enabled;
        float directivity_weight, directivity_power, spatial_blend;
        bool use_ambisonics_encode;
        int path_validation_override, find_alternate_paths_override;
        int reflections_type, reflections_enabled, pathing_enabled_override;
        int reverb_binaural_override, pathing_binaural_override;
        int occlusion_input, transmission_input, directivity_input;
        float occlusion_value, transmission_low, transmission_mid, transmission_high, directivity_value;
        int occlusion_samples, max_transmission_surfaces;
        /// 0 = use ResonanceRuntimeConfig.max_transmission_surfaces; 1 = use max_transmission_surfaces from resource. (-1 from legacy .tres is normalized to 0 in refresh.)
        int max_transmission_surfaces_override = 0;
        float master_mix_level;
        float direct_mix_level, reflections_mix_level, pathing_mix_level;
        float reflections_eq_low, reflections_eq_mid, reflections_eq_high;
        int reflections_delay;
        int perspective_override;
        float perspective_factor;
        /// 0 = push playback params every frame. >0 = min seconds between full occlusion/reverb/param pushes (LOD).
        float playback_parameter_min_interval;
        /// 0 = ignore. Else full push when source moved at least this many meters from last push anchor.
        float playback_parameter_min_move;
        /// Exponential smoothing time constant (s) for simulation-derived occlusion/transmission; 0 = off.
        float playback_coeff_smoothing_time;
        /// For Linear/Curve modes only: legacy [code]distance_attenuation_simulation_enabled[/code] from resource (Steam DIRECT distance flag).
        bool linear_curve_use_sim_distance_attenuation = true;
        /// When false, direct simulation skips occlusion rays (sim-defined occlusion path).
        bool simulation_occlusion_enabled = true;
        /// When false, direct simulation skips transmission through geometry.
        bool simulation_transmission_enabled = true;
        /// -1 = use ResonanceRuntimeConfig.occlusion_type; 0 = raycast; 1 = volumetric.
        int occlusion_type_override = -1;
        /// -1 = use runtime transmission_type; 0 = freq independent; 1 = freq dependent.
        int transmission_type_override = -1;
        /// -1 = use runtime hrtf_interpolation_bilinear; 0 = nearest; 1 = bilinear.
        int hrtf_interpolation_override = -1;
        /// -1 = use ResonanceServer.apply_occlusion_to_baked_reflections; 0 = Disabled; 1 = Enabled.
        int apply_occlusion_to_baked_reflections_override = -1;
        /// -1 = use ResonanceServer.baked_reverb_use_listener_probe; 0 = Disabled (probe = source pos); 1 = Enabled (probe = listener pos).
        int baked_reverb_use_listener_probe_override = -1;
        /// 0 = use ResonanceServer.reverb_transmission_amount (global); 1 = use reverb_transmission_amount below.
        int reverb_transmission_amount_input = 0;
        /// Per-source transmission damping on reverb (only used when reverb_transmission_amount_input == 1).
        float reverb_transmission_amount = 1.0f;
    } config_cache_;
    bool config_cache_valid_ = false;

    float coeff_smooth_occ_ = 0.0f;
    float coeff_smooth_tx_[3] = {1.0f, 1.0f, 1.0f};
    bool coeff_smooth_initialized_ = false;
    int32_t coeff_smooth_source_handle_ = -1;

    double playback_lod_time_since_full_ = 0.0;
    bool playback_lod_have_anchor_ = false;
    Vector3 playback_lod_anchor_pos_;

    // Debug visualization
    ResonanceDebugDrawer debug_drawer;
    ResonanceDebugData debug_overlay_last_data_{};
    bool debug_overlay_has_last_data_ = false;
    double debug_overlay_grace_timer_ = 0.0;

    // Directivity gizmo (runtime wireframe; toggled by [member show_directivity_gizmo]).
    ResonanceDirectivityDrawer directivity_drawer_;
    bool show_directivity_gizmo_ = false;

    void _update_stream_setup(); // Ensures the internal stream is set up correctly
    /// With player_config: disable Godot ASP3D attenuation/max_distance so Steam owns distance (also after runtime config assign).
    void _apply_steam_mode_asp3d_guards();
    /// Refresh host gain cache from volume_db/max_db (applied to dry input before Steam DSP).
    void _refresh_effective_volume_cache();
    void _ensure_source_exists(); // Ensures the source handle exists in the ResonanceServer
    /// Drop source_handle when server lifecycle epoch advanced (reinit recycled IDs).
    void _invalidate_source_handle_if_stale(ResonanceServer* srv);
    /// After engine reinit: clear stale handle and recreate IPL source.
    void reload_source_after_reinit();
    /// Lazy-create IPL source when server is ready (ordering: nodes before runtime).
    /// Returns true if a new handle was created. Optionally defers playback param push when already playing.
    bool _try_ensure_source_and_sync(ResonanceServer* srv, bool deferred_playback_push_if_playing);
    void _deferred_try_ensure_source_after_config();
    void _start_reverb_split_child_if_needed();
    bool warned_source_handle_create_failed_ = false;
    void _ensure_config_valid();                                 // Refreshes config cache if countdown expired or invalid
    void _ensure_config_and_apply_source(int32_t pathing_batch); // Ensures config valid, then applies update_source (blocking; clear_pathing etc.)
    ResonanceServer::SourceUpdateParams _build_source_update_params(ResonanceServer* srv, int32_t pathing_batch) const;
    /// defer_if_sim_mutex_busy: enqueue (batched) or try_update_source from _process so the main thread does not wait on RunReflections.
    void _apply_update_source(int32_t pathing_batch, bool defer_if_sim_mutex_busy = false);

    /// Computes IPLBakedDataVariation for reflections: -1 = realtime, 0 = REVERB, 1 = STATICSOURCE, 2 = STATICLISTENER.
    /// Mirrors the branch in [_apply_update_source]; shared so the audio-thread-facing playback parameters know
    /// whether baked REVERB is active (needed for wet-path occlusion damping).
    int _compute_baked_data_variation(const ResonanceServer* srv) const;

    void _setup_attenuation(ResonanceServer* srv);
    /// Config, attenuation callback, pathing batch, and simulation source update (try_update / enqueue).
    void _prepare_source_for_simulation(ResonanceServer* srv);
    /// Occlusion/reverb readback and ResonanceStreamPlayback::update_parameters (after [member _prepare_source_for_simulation]).
    void _apply_playback_params_from_simulation(ResonanceServer* srv, ResonanceDebugData* opt_debug_out, double delta_seconds);
    bool _playback_lod_should_apply_playback_params(double delta, bool debug_hud_active, const Vector3& source_pos);
    /// Pushes occlusion/attenuation from simulation into the audio thread (same as one _process tick without debug UI).
    /// When opt_debug_out is set, fills debug fields (not signal levels).
    void _push_playback_parameters_from_simulation(ResonanceServer* srv, ResonanceDebugData* opt_debug_out, double delta_seconds);
    void _deferred_push_playback_parameters();
    void _sync_player_debug_drawer(double delta, ResonanceServer* srv, const ResonanceDebugData& dbg_data, bool hud_active);
    void _compute_listener_data(Viewport* vp, Vector3& out_listener_pos, IPLCoordinateSpace3& out_listener_orient);
    void _compute_attenuation(float dist, const OcclusionData& occ_data, float& out_attenuation);
    Vector3 _apply_perspective_correction(Vector3 listener_pos, Viewport* vp, bool apply_perspective, float perspective_factor_val);
    PlaybackParameters _build_playback_params(const Vector3& listener_pos, const IPLCoordinateSpace3& listener_orient,
                                              float attenuation, float dist, const Vector3& effective_source_pos,
                                              float occ_val, float tx_low, float tx_mid, float tx_high, float directivity_val, const Vector3& air_abs,
                                              bool has_reverb, bool direct_enabled, bool reverb_enabled);

    ResonanceStreamPlayback* _get_resonance_playback();
    void _aggregate_debug_signal_levels(float& out_direct, float& out_reverb, float& out_pathing);
    void _update_reverb_split_child(const StringName& p_reverb_bus = StringName());
    void _nexus_deferred_spawn_anim_audio_helper();
    void _nexus_deferred_emit_finished();

    bool reverb_split_output_ = false;
    /// When [member player_config] is set: spawn a helper that converts TYPE_AUDIO tracks on the current scene
    /// to [method play_animation_audio_clip] once (Steam-safe path; native polyphonic cannot be wrapped).
    bool convert_anim_audio_runtime_ = false;
    bool exclude_from_debug_ = false;
    /// When true (default), register descendant CollisionObject3D RIDs with ResonanceServer for Custom-scene ray excludes.
    bool auto_exclude_colliders_ = true;
    int physics_auto_exclude_resync_counter_ = 0;
    std::vector<RID> registered_physics_auto_exclude_rids_;
    void _sync_physics_ray_auto_exclude_rids();
    void _clear_physics_ray_auto_exclude_rids();

    /// `finished` should fire exactly once at dry-EOS. Wet/pathing tails may keep running afterwards.
    bool dry_finished_emitted_ = false;
    bool dry_finished_deferred_queued_ = false;
    uint64_t play_serial_ = 0;
    uint64_t dry_finished_deferred_serial_ = 0;
    /// Soft-stop watchdog: seconds since stop() requested soft-stop (-1 = inactive).
    /// Forces AudioStreamPlayer3D::stop if audio-thread drain never signals (e.g. Dummy driver / no _mix).
    double soft_stop_elapsed_sec_ = -1.0;

    float _config_float(const char* key, float default_val) const;
    int _config_int(const char* key, int default_val) const;
    bool _config_bool(const char* key, bool default_val) const;
    Ref<Curve> _config_curve(const char* key, const Ref<Curve>& default_val) const;
    NodePath _config_node_path(const char* key) const;
    void _refresh_config_cache();
    /// Whether direct sim should use IPL distance attenuation for this attenuation mode.
    static bool _steam_sim_distance_attenuation_enabled(const ConfigCache& c);

  protected:
    static void _bind_methods();
    void _notification(int p_what);
    void _validate_property(PropertyInfo& p_property) const;

  public:
    ResonancePlayer() = default;
    ~ResonancePlayer() override;

    void _enter_tree() override;
    void _ready() override;
    void _process(double delta) override;
    void _exit_tree() override;
    /// Wraps AudioStreamPlayer3D::play: ensures internal stream, retries source handle, reverb split child, deferred sim push.
    void set_stream(const Ref<AudioStream>& p_stream);
    Ref<AudioStream> get_stream() const;

    void play(float from_position = 0.0f);
    void play_stream(double from_position = 0.0);
    /// Use from AnimationPlayer **method** tracks (or enable [member convert_anim_audio_runtime]).
    /// Sets [member stream] and calls [method play]. [param from_position] is usually the clip's start offset (trim).
    void play_animation_audio_clip(const Ref<AudioStream>& p_stream, float from_position = 0.0f);
    void stop();

    void set_pathing_probe_volume(const NodePath& p_path);
    NodePath get_pathing_probe_volume() const;
    float get_effective_volume_linear_cached() const { return cached_effective_volume_linear_.load(std::memory_order_relaxed); }
    /// Called by ResonanceProbeVolume when it is removed; immediately clears pathing batch so worker does not use freed data.
    void clear_pathing_probe_immediate();
    void set_player_config(const Ref<Resource>& p_config);
    Ref<Resource> get_player_config() const;
    void set_reverb_split_output(bool p_enable, const StringName& p_reverb_bus = StringName());
    bool get_reverb_split_output() const { return reverb_split_output_; }

    void set_exclude_from_debug(bool p_exclude);
    bool get_exclude_from_debug() const { return exclude_from_debug_; }

    void set_auto_exclude_colliders(bool p_enable);
    bool get_auto_exclude_colliders() const { return auto_exclude_colliders_; }

    void set_convert_anim_audio_runtime(bool p_enable);
    bool get_convert_anim_audio_runtime() const { return convert_anim_audio_runtime_; }

    void set_show_directivity_gizmo(bool p_enable);
    bool get_show_directivity_gizmo() const { return show_directivity_gizmo_; }

    /// Build wireframe line segments for a directivity gizmo as pairs of Vector3 (line list).
    /// [param enabled] master toggle from [code]directivity_enabled[/code]; when false draws a unit sphere + forward arrow.
    /// [param input_mode] [code]0[/code] = Simulation Defined (dipole from weight/power), [code]1[/code] = User Defined (scalar; draws sphere + scaled arrow).
    /// [param weight] / [param power] map to IPL directivity dipole weight / power when simulation-defined.
    /// [param user_value] scalar 0..1 used to scale the forward arrow when [param input_mode] is User Defined.
    /// [param size] world-space radius of the gizmo in meters (typical value: [code]1.0[/code]).
    static PackedVector3Array build_directivity_gizmo_lines(bool enabled, int input_mode, float weight, float power, float user_value, float size);

    /// Called from [member player_config] [signal Resource.changed] to refresh the editor gizmo and runtime drawer.
    void _on_player_config_changed_refresh_gizmo();

    /// Returns audio instrumentation dict for dropout debugging. Keys include pathing_sh_rms, pathing_sh_energy (sum c^2),
    /// pathing_out_rms (path effect stereo RMS before add to final mix), pathing_sh_order (-1 if n/a). When player_config is set,
    /// also godot_attenuation_model, godot_pitch_scale, godot_max_distance, godot_max_db, godot_volume_db, godot_unit_size,
    /// effective_volume_linear (AudioStreamPlayer3D snapshot + source loudness applied pre-Steam).
    /// Empty when no player_config.
    Dictionary get_audio_instrumentation();
    /// Reset instrumentation counters on this player's playback. Call to clear and re-observe dropouts.
    void reset_audio_instrumentation();

    /// True if any active ResonanceStreamPlayback voice still has split-reverb samples queued
    /// in its output_ring_reverb. Used by ResonanceReverbPlayback::_is_playing() to keep the
    /// reverb-split child alive while the parent's tail decays.
    bool any_internal_playback_has_reverb_ring_data() const;
};
} // namespace godot

// Register Enum for Godot Inspector
VARIANT_ENUM_CAST(ResonancePlayer::AttenuationMode);

#endif