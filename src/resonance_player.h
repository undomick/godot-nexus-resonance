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
#include <godot_cpp/variant/vector2.hpp>

#include "resonance_constants.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <phonon.h>
#include <vector>

#include <godot_cpp/variant/rid.hpp>

#include "resonance_block_stage_timer.h"
#include "resonance_block_time_histogram.h"
#include "resonance_constants.h"
#include "resonance_debug_drawer.h"
#include "resonance_directivity_drawer.h"
#include "resonance_exit_pins.h"
#include "resonance_latest_value_slot.h"
#include "resonance_mix_lateness_policy.h"
#include "resonance_mixer_processor.h"
#include "resonance_os_thread_id.h"
#include "resonance_owner_link.h"
#include "resonance_processor_direct.h"
#include "resonance_processor_path.h"
#include "resonance_processor_reflection.h"
#include "resonance_ring_buffer.h"
#include "resonance_server.h"
#include "resonance_spawn_timing.h"
#include "resonance_voice_virtualization_policy.h"

#include <godot_cpp/variant/packed_vector3_array.hpp>

namespace godot {

class ResonancePlayer;

struct PlaybackParameters {
    int32_t source_handle = -1;
    float occlusion = 1.0f; // 1 = line of sight, 0 = fully occluded
    float transmission[3] = {1, 1, 1};
    float attenuation = 1.0f; // Direct path only; the pathing wet is not scaled by it.
    float distance = 0.0f;    // Meters
    bool enable_direct = true;
    bool enable_reverb = true;
    bool enable_reflections = false; // Per-source reflections run, so a reflection IR will arrive
    bool has_valid_reverb = false;

    Vector3 source_position = Vector3(0, 0, 0); // World space
    bool use_binaural = true;
    float spatial_blend = 1.0f;         // 0 = unspatialized, 1 = fully spatialized
    bool use_ambisonics_encode = false; // Encode the point source to Ambisonics before binaural

    bool apply_air_absorption = false;
    resonance::AudioBands3 air_absorption{1.0f, 1.0f, 1.0f};

    bool apply_directivity = false;
    float directivity_value = 1.0f; // Directivity gain for the direct effect, 0..1

    IPLCoordinateSpace3 listener_orientation{};

    // Per-path gains (0 = off, 1 = full) - gate simulation bands and wet sends.
    float direct_mix_level = 1.0f;
    float reflections_mix_level = 1.0f;
    float pathing_mix_level = 1.0f;
    bool apply_hrtf_to_reflections = true;
    bool apply_hrtf_to_pathing = true;

    // Per-source hybrid reverb overrides. Only applied when reflection_type is Hybrid. -1 = use simulation value.
    float reflections_eq[3] = {1.0f, 1.0f, 1.0f}; // Multipliers on the parametric part
    int reflections_delay = -1;                   // Samples before the parametric part starts

    /// Copy the final output into the send ring that the ResonanceReverbOutput child plays on the reverb bus.
    bool reverb_send = false;

    /// Direct effect: 0 = freq-independent transmission, 1 = freq-dependent (3-band).
    int direct_effect_transmission_type = 0;
    /// Direct effect HRTF interpolation: false = nearest, true = bilinear.
    bool direct_effect_hrtf_bilinear = false;

    /// Gain on the reflection wet path. Stays 1.0: simulated and baked reverb already include occlusion.
    float wet_occlusion_factor = 1.0f;

    /// Inaudible voice: the stream keeps advancing but no DSP runs and the output is silent.
    bool virtualized = false;
};

class ResonanceStreamPlayback : public AudioStreamPlayback {
    GDCLASS(ResonanceStreamPlayback, AudioStreamPlayback)
    friend class ResonancePlayer;
    friend class SharedReflectionUse;

  private:
    /// The player can be freed on the main thread while the audio thread mixes. Audio-thread entry points hold a
    /// lease for their whole call; ~ResonancePlayer orphans the link and waits for that lease.
    resonance::OwnerLink<ResonancePlayer> owner_link_;
    ResonancePlayer* owner_player() const { return owner_link_.get(); }
    resonance::ExitPinned exit_pin_{this};

    /// Callback budget for live-path Steam blocks per _mix. The EOS tail drain bypasses this cap.
    static const int kMaxBlocksPerMixCall = 4;
    int frame_size_ = resonance::kGodotDefaultFrameSize;
    /// Direct spatializer output channels (1/2/4/6/8); _mix folds them down to stereo.
    int direct_out_channels_ = 2;
    Ref<AudioStreamPlayback> base_playback;

    /// _mix stays silent until the first update_parameters of a run, to avoid a one-frame full-gain burst.
    std::atomic<bool> params_ever_synced_{false};
    resonance::LatestValueSlot<PlaybackParameters> params_slot_;
    /// Audio thread only.
    PlaybackParameters params_current;
    bool voice_was_virtual_ = false;

    bool is_initialized = false;
    bool wet_effects_initialized_ = false; // Main thread only.
    /// Set by the audio thread when the IPL context changed; main fixes it in resolve_stale_steam_context_on_main().
    std::atomic<bool> steam_context_stale_{false};
    int current_sample_rate = 44100;

    /// Extra retain on the active sim source: taken on main in update_parameters, released on handle change or cleanup.
    std::atomic<uintptr_t> retained_ipl_source_{0};
    std::atomic<int32_t> retained_source_handle_{-1};
    int32_t current_source_handle = -1;
    std::atomic<int32_t> voice_source_handle_{-1};
    std::atomic<uint32_t> voice_source_epoch_{0};
    int32_t get_voice_source_handle() const { return voice_source_handle_.load(std::memory_order_acquire); }
    uint32_t get_voice_source_epoch() const { return voice_source_epoch_.load(std::memory_order_acquire); }
    void set_voice_source_binding(int32_t handle, uint32_t epoch) {
        voice_source_epoch_.store(epoch, std::memory_order_release);
        voice_source_handle_.store(handle, std::memory_order_release);
    }
    /// Audio thread holds this across iplReflectionEffectApply so main can wait before iplSourceRelease.
    std::atomic<int> convolution_apply_depth_{0};
    std::atomic<bool> convolution_blocked_{false};
    bool convolution_try_enter();
    void convolution_leave();
    void block_convolution_for_source_release();
    int convolution_apply_depth() const { return convolution_apply_depth_.load(std::memory_order_acquire); }
    IPLContext context = nullptr;
    uint64_t next_prewarm_retry_msec_ = 0;

    ResonanceDirectProcessor direct_processor;
    ResonancePathProcessor path_processor;
    ResonanceMixerProcessor mixer_processor;

    IPLAudioBuffer sa_in_buffer{};
    IPLAudioBuffer sa_direct_out_buffer{};
    IPLAudioBuffer sa_path_out_buffer{};
    IPLAudioBuffer sa_final_mix_buffer{};

    RingBuffer<float> input_ring_l;
    RingBuffer<float> input_ring_r;
    RingBuffer<float> output_ring_l;
    RingBuffer<float> output_ring_r;
    RingBuffer<float> output_ring_reverb_l;
    RingBuffer<float> output_ring_reverb_r;

    // One IPL frame per channel: frame_size_ samples, at most kMaxAudioFrameSize.
    std::vector<float> temp_process_buffer_l;
    std::vector<float> temp_process_buffer_r;

    std::vector<float> temp_reverb_buffer_l;
    std::vector<float> temp_reverb_buffer_r;
    // Click guard for the split reverb output: fade to zero when the ring runs dry mid-callback.
    float reverb_ring_prev_l_ = 0.0f;
    float reverb_ring_prev_r_ = 0.0f;
    bool reverb_ring_prev_valid_ = false;
    /// On a reverb ring underrun, one cosine taper spans several callbacks to avoid LF swallow and pumping.
    bool reverb_ring_gap_fade_armed_ = false;
    int reverb_ring_gap_fade_index_ = 0;
    int reverb_ring_gap_fade_total_ = 0;
    /// Last samples _mix wrote, post volume. When the output ring runs empty mid-callback the fade starts from
    /// these instead of a hard zero.
    float last_mix_out_l_ = 0.0f;
    float last_mix_out_r_ = 0.0f;
    bool last_mix_out_valid_ = false;
    /// Last decoded samples fed to the input rings; the next chunk blends from them after a partial decode.
    float input_ring_tail_l_ = 0.0f;
    float input_ring_tail_r_ = 0.0f;
    bool input_ring_tail_valid_ = false;
    /// The chunk crossfade runs only after a partial decode that was hold-padded.
    bool prev_mix_had_partial_input_pad_ = false;
    /// The pad was tapered to silence at EOS, so the ring ends near zero, not at input_ring_tail_*.
    /// Crossfading from the tail would step.
    bool prev_mix_had_eos_tapered_input_pad_ = false;
    /// Linear fade-in after _start; kPlaybackHostFadeInMs converted to samples at current_sample_rate.
    int playback_host_fade_in_elapsed_ = 0;
    int playback_host_fade_in_total_samples_ = 1;
    /// Linear fade-out after _stop or a soft stop, in samples.
    int playback_host_fade_out_remaining_ = 0;
    int playback_host_fade_out_total_samples_ = 1;

    void apply_playback_host_fades(AudioFrame* buffer, int32_t frames);

    // Previous gains for per-block ramps; 0 ramps up from silence.
    float prev_direct_weight = 0.0f;
    float prev_conv_reflections_mix_level_ = 0.0f;
    float prev_parametric_reflections_mix_level_ = 0.0f; // Parametric and hybrid
    float prev_pathing_mix_level_ = 0.0f;

    int no_reverb_warn_count = 0;

    bool input_started = false;

    float parametric_path_sh_coeffs[4];

    /// Last path params, reused while live params are stale; their SH coeffs point into pathing_tail_sh_coeffs_.
    IPLPathEffectParams pathing_tail_params_{};
    bool pathing_tail_have_params_ = false;
    uint32_t pathing_tail_param_epoch_ = 0;
    /// Own copy of the SH coeffs (order 3 at most): the server may swap its caches while the tail still plays.
    std::array<float, 16> pathing_tail_sh_coeffs_{};

    /// Set by _stop() or a soft stop from ResonancePlayer::stop(). Starts the tail drain: _is_playing() stays true
    /// until the reverb and path tails have decayed or the grace budget runs out.
    std::atomic<bool> stop_requested_{false};

    // AudioStreamPlayer3D::play() creates the playback but Godot calls _start only on its next
    // physics tick. Until then base_playback reports not playing although the run has not begun.
    std::atomic<bool> started_{false};

    // Godot ends a stream once mix returns fewer frames than requested. Some playbacks (an empty
    // AudioStreamWAV) keep reporting is_playing() after that, so the short read is the end signal.
    std::atomic<bool> base_short_read_{false};

    /// Hard cap on tail blocks after the dry signal ends, so a stalled effect cannot keep the playback alive.
    /// -1 = not armed yet, >0 = blocks left, 0 = force end. Armed from get_realtime_simulation_duration().
    std::atomic<int64_t> tail_grace_blocks_remaining_{-1};

    /// Tail drain is done. _mix still never returns 0: ResonancePlayer emits `finished` at dry EOS and stops
    /// the node itself.
    std::atomic<bool> tail_drain_complete_{false};

    // Instrumentation counters: written by the audio thread, read by main.
    std::atomic<uint64_t> instrumentation_input_dropped{0};   // Samples dropped because the input ring was full
    std::atomic<uint64_t> instrumentation_output_underrun{0}; // Output frames filled with silence
    std::atomic<uint64_t> instrumentation_output_blocked{0};  // Processing skipped because the output ring was full
    std::atomic<uint64_t> instrumentation_mix_call_count{0};
    std::atomic<uint64_t> instrumentation_blocks_processed{0};
    /// The live pump hit kMaxBlocksPerMixCall while the input ring still held a full block.
    std::atomic<uint64_t> instrumentation_pump_cap_reached{0};
    std::atomic<uint64_t> instrumentation_passthrough_blocks{0}; // No source handle
    std::atomic<uint64_t> instrumentation_reverb_miss_blocks{0}; // Reverb wanted but fetch_reverb_params failed
    std::atomic<uint64_t> instrumentation_max_block_time_us{0};
    std::atomic<uint64_t> instrumentation_last_block_time_us{0};
    std::atomic<uint64_t> instrumentation_block_time_total_us_{0};
    resonance::BlockStageTotals instrumentation_block_stages_;
    resonance::BlockTimeHistogram instrumentation_block_time_histogram_;
    // Thread that ran the first block; kept across resets because the audio thread does not change.
    std::atomic<int64_t> instrumentation_audio_thread_id_{resonance::kUnknownOsThreadId};
    /// _mix calls that fell behind real time.
    std::atomic<uint64_t> instrumentation_late_mix_count{0};
    std::atomic<uint64_t> instrumentation_last_mix_gap_us_{0};
    std::atomic<uint64_t> instrumentation_max_mix_gap_us_{0};
    /// Gap implied by the requested frames at the current sample rate.
    std::atomic<uint64_t> instrumentation_expected_mix_gap_us_{0};
    std::atomic<uint64_t> instrumentation_param_sync_count{0};
    std::atomic<uint64_t> instrumentation_zero_input_count{0};                                // _mix calls in the tail drain
    std::atomic<int32_t> instrumentation_mix_frames_min{std::numeric_limits<int32_t>::max()}; // Ignores calls with no input
    std::atomic<int32_t> instrumentation_mix_frames_max{0};
    std::atomic<uint64_t> instrumentation_silent_output_blocks{0}; // Output RMS < 0.0001
    std::atomic<uint32_t> instrumentation_last_output_rms_q8{0};   // RMS * 256
    /// Pathing diagnostics for the last block: SH coeff RMS, sum of squared coeffs, path effect output RMS.
    std::atomic<float> instrumentation_last_pathing_sh_rms{0.0f};
    std::atomic<float> instrumentation_last_pathing_sh_energy{0.0f};
    std::atomic<float> instrumentation_last_pathing_out_rms{0.0f};
    std::atomic<int32_t> instrumentation_last_pathing_order{-1}; // -1 = no pathing this block
    std::atomic<float> debug_signal_direct{0.0f};
    std::atomic<float> debug_signal_reverb{0.0f};
    std::atomic<float> debug_signal_pathing{0.0f}; // Path wet RMS, clamped 0..1
    /// Convolution/TAN blocks that found no IPLReflectionMixer for the shared wet bus.
    std::atomic<uint64_t> instrumentation_conv_mixer_null_blocks{0};
    /// Convolution/TAN blocks whose process_mix rejected the wet feed.
    std::atomic<uint64_t> instrumentation_conv_mix_failed_blocks{0};
    /// Blocks with a valid sim source but enable_reverb off.
    std::atomic<uint64_t> instrumentation_enable_reverb_false_blocks{0};
    std::chrono::steady_clock::time_point last_mix_time_; // Audio thread only
    // Mix timing state, audio thread only. Restarted by _start so a pause between plays is not a late call.
    bool mix_timing_started_ = false;
    uint64_t last_mix_block_us_ = 0;
    resonance::MixLatenessTracker mix_lateness_;
    bool geometry_gate_was_holding_decode_ = false;

    void _lazy_init_steam_audio(int sampling_rate);
    void _init_wet_effects(ResonanceServer* srv);
    void _ensure_wet_effects_on_main(ResonanceServer* srv);
    void _cleanup_steam_audio();
    void _process_steam_audio_block();
    void _remember_audio_thread();
    void _apply_reflections_wet(ResonanceServer* srv, bool& out_reverb_to_player, float& out_dbg_reverb);
    float _apply_pathing_wet(ResonanceServer* srv, const IPLCoordinateSpace3& listener_cs);
    void _process_passthrough_block();
    void _sync_params();
    void _note_mix_timing(int32_t frames);
    void _retain_source_for_main(int32_t handle);
    void _release_retained_source();
    void _write_output_block(const float* left, const float* right);
    void _add_reverb_to_output(IPLAudioBuffer* reverb_buf, float refl_mix,
                               const IPLCoordinateSpace3& listener_coords, bool apply_reverb_binaural);
    void _write_output_rings_folded(); // Folds sa_final_mix_buffer down to stereo; clobbers temp_process_buffer_*
    void _zero_sa_final_mix();
    bool _drain_shared_reflection_tail(ResonanceServer* srv);
    void internal_orphan_owner_player() { owner_link_.orphan(); }

    /// Mix with no dry input: renders the wet and direct tails. Always returns `frames`, padding with silence,
    /// so AudioServer does not detach the playback before the tail ends.
    int32_t _mix_drain_zero_input_tails(AudioFrame* buffer, int32_t frames, ResonanceServer* srv_guard);
    /// Dry fallback while IPL is not ready: decoder frames with only the source volume applied.
    int32_t _mix_passthrough_pre_steam(AudioFrame* buffer, int32_t frames, const Vector2* src, int32_t samples_read,
                                       bool base_playing);
    /// Feeds decoder samples into the input rings, with chunk crossfade, EOS taper and hold or zero pad.
    void _mix_ingest_decoder_samples(const Vector2* src_ptr, int32_t samples_read, int32_t frames, bool base_playing);
    void _mix_pump_available_steam_blocks();
    int32_t _mix_virtualized(AudioFrame* buffer, int32_t frames, float rate_scale,
                             const Ref<AudioStreamPlayback>& base);
    void _reset_voice_dsp_state();
    /// Always fills `frames`; an underrun fades out from the last sample.
    void _mix_emit_output_frames(AudioFrame* buffer, int32_t frames, bool count_underrun);

  public:
    ResonanceStreamPlayback();
    ~ResonanceStreamPlayback();

    /// Called by ResonanceServer before iplContextRelease (userdata = this).
    static void ipl_context_reinit_cleanup(void* userdata);

    ResonanceStreamPlayback(const ResonanceStreamPlayback&) = delete;
    ResonanceStreamPlayback(ResonanceStreamPlayback&&) = delete;

    void set_base_playback(const Ref<AudioStreamPlayback>& p_playback);
    Ref<AudioStreamPlayback> get_base_playback() const { return base_playback; }
    Ref<AudioStreamPlayback> get_inner_stream_playback() const { return base_playback; }
    void set_owner_player(ResonancePlayer* p_player) { owner_link_.set(p_player); }
    void update_parameters(const PlaybackParameters& p_params);

    virtual int32_t _mix(AudioFrame* buffer, float rate_scale, int32_t frames) override;
    virtual void _start(double from_pos) override;

    /// Allocates the IPL effects and buffers on main so the first _mix does not stall the audio thread. Idempotent.
    bool prewarm_steam_audio();
    /// Main thread only: tears down and prewarms again after the audio thread flagged a stale IPL context.
    void resolve_stale_steam_context_on_main();
    bool is_steam_audio_initialised() const { return is_initialized; }

    /// Last block's direct gain, reverb send and path wet RMS. Safe from the main thread.
    void get_debug_signal_levels(float& out_direct, float& out_reverb, float& out_pathing) const;
    /// Summed _process_steam_audio_block time since reset_instrumentation().
    uint64_t get_block_time_total_us() const;
    /// Per-stage split of get_block_time_total_us(), in nanoseconds.
    const resonance::BlockStageTotals& get_block_stage_totals() const { return instrumentation_block_stages_; }
    const resonance::BlockTimeHistogram& get_block_time_histogram() const { return instrumentation_block_time_histogram_; }
    int64_t get_audio_thread_id() const { return instrumentation_audio_thread_id_.load(std::memory_order_relaxed); }
    /// Safe to call from the main thread.
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
    void reset_instrumentation();
    /// ResonanceReverbPlayback reads this after the main stream stops.
    size_t get_reverb_ring_available_read() const { return output_ring_reverb_l.get_available_read(); }

    /// Effects still have tail samples or the output rings still hold samples; keeps _is_playing() true
    /// after the dry signal ends.
    bool has_active_tail_residue() const;

    /// Main thread: halts the dry signal but keeps the playback alive until the reverb and path tails ring out.
    /// ResonancePlayer::stop() calls this instead of AudioStreamPlayer3D::stop().
    void request_soft_stop();

    /// Audio thread, from the reverb playback's _mix. Pads with silence and always returns `frames`.
    int32_t read_reverb_frames(AudioFrame* buffer, int32_t frames);
    virtual void _stop() override;
    virtual bool _is_playing() const override;
    virtual int _get_loop_count() const override;
    virtual void _seek(double position) override;

    bool is_tail_drain_complete() const { return tail_drain_complete_.load(std::memory_order_acquire); }
    bool has_started() const { return started_.load(std::memory_order_acquire); }
    bool is_base_playing() const;
    uint64_t get_mix_call_count() const { return instrumentation_mix_call_count.load(std::memory_order_acquire); }

  protected:
    static void _bind_methods();
};

// Holds the voice's convolution depth while it reads the emitter's shared reflection output, so the
// main thread cannot free that effect between the Apply and the decode of its output buffer.
class SharedReflectionUse {
  public:
    explicit SharedReflectionUse(ResonanceStreamPlayback& voice) : voice_(voice), held_(voice.convolution_try_enter()) {}
    ~SharedReflectionUse() {
        if (held_)
            voice_.convolution_leave();
    }
    SharedReflectionUse(const SharedReflectionUse&) = delete;
    SharedReflectionUse& operator=(const SharedReflectionUse&) = delete;
    SharedReflectionUse(SharedReflectionUse&&) = delete;
    SharedReflectionUse& operator=(SharedReflectionUse&&) = delete;

    bool held() const { return held_; }

  private:
    ResonanceStreamPlayback& voice_;
    bool held_;
};

class ResonanceStream : public AudioStream {
    GDCLASS(ResonanceStream, AudioStream)
  private:
    Ref<AudioStream> base_stream;
    ResonancePlayer* stream_owner_ = nullptr;

  public:
    void set_base_stream(const Ref<AudioStream>& p_stream);
    Ref<AudioStream> get_base_stream() const { return base_stream; }
    Ref<AudioStream> get_inner_stream() const { return base_stream; }
    void set_stream_owner(ResonancePlayer* p_player) { stream_owner_ = p_player; }
    virtual Ref<AudioStreamPlayback> _instantiate_playback() const override;
    virtual String _get_stream_name() const override { return "Resonance"; }
    virtual double _get_length() const override { return base_stream.is_valid() ? base_stream->get_length() : 0.0; }
    virtual bool _is_monophonic() const override { return false; }

  protected:
    static void _bind_methods();
};

/// Shared by a player and every reverb-split playback it spawned; the player orphans it on tree exit and destruction.
using ReverbParentLink = resonance::OwnerLink<ResonancePlayer>;

class ResonanceReverbPlayback : public AudioStreamPlayback {
    GDCLASS(ResonanceReverbPlayback, AudioStreamPlayback)
  private:
    std::shared_ptr<ReverbParentLink> parent_link_;
    resonance::ExitPinned exit_pin_{this};

  public:
    ResonanceReverbPlayback() = default;
    ResonanceReverbPlayback(const ResonanceReverbPlayback&) = delete;
    ResonanceReverbPlayback(ResonanceReverbPlayback&&) = delete;
    void set_parent_link(std::shared_ptr<ReverbParentLink> p_link);
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
    std::shared_ptr<ReverbParentLink> parent_link_;

  public:
    ResonanceReverbStream() = default;
    ResonanceReverbStream(const ResonanceReverbStream&) = delete;
    ResonanceReverbStream(ResonanceReverbStream&&) = delete;
    void set_parent_link(std::shared_ptr<ReverbParentLink> p_link);
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
        ATTENUATION_INVERSE,
        ATTENUATION_LINEAR, // Falls to 0 at max_distance
        ATTENUATION_CUSTOM_CURVE,
        ATTENUATION_DISABLED, // No rolloff on the direct path or the reflection and pathing wet
    };

  private:
    Ref<ResonanceStream> internal_stream;
    /// With player_config the user's stream lives here; the engine plays internal_stream.
    Ref<AudioStream> logical_stream_;
    /// Main mutates the voice list under the mutex; the audio thread reads only the published snapshot.
    struct PlaybackVoiceSnapshot {
        int count = 0;
        std::array<ResonanceStreamPlayback*, resonance::kMaxPlayerPolyphonySnapshot> voices{};
    };
    mutable std::mutex internal_playbacks_mutex_;
    std::vector<ResonanceStreamPlayback*> internal_playbacks_;
    std::atomic<int> playback_count_{0};
    PlaybackVoiceSnapshot playback_snap_[2];
    std::atomic<int> playback_snap_front_{0};
    void internal_publish_playback_snapshot();
    void internal_get_playback_snapshot_for_audio(PlaybackVoiceSnapshot& out) const;
    void internal_register_playback(ResonanceStreamPlayback* p);
    void internal_unregister_playback(ResonanceStreamPlayback* p);
    void internal_reclaim_voice_source(int32_t handle, uint32_t epoch);
    void internal_copy_internal_playbacks(std::vector<ResonanceStreamPlayback*>& out) const;
    void _broadcast_update_parameters(const PlaybackParameters& p);
    /// Must run before ResonanceServer::destroy_source_handle.
    void _detach_playback_source_retains();
    /// Reflections or pathing are on, so prewarm allocates the wet IPL effects.
    bool internal_wants_wet_effects() const;
    bool internal_wants_ambisonics_encode() const;
    resonance::SpawnTiming* internal_spawn_timing() { return &spawn_timing_; }

    /// Idle handle, or the first live voice after one is bound.
    int32_t source_handle = -1;
    /// Server source lifecycle epoch at create; a different epoch means the handle is stale after a reinit.
    uint32_t source_lifecycle_epoch_ = 0;
    // Linear gain; written on main, read on the audio thread.
    std::atomic<float> cached_effective_volume_linear_{1.0f};
    /// Mirror of is_playing() for the audio thread, which must not call Node APIs.
    std::atomic<bool> playing_for_audio_{false};
    void _publish_playing_for_audio();
    std::shared_ptr<ReverbParentLink> reverb_parent_link_ = std::make_shared<ReverbParentLink>();
    NodePath pathing_probe_volume;
    Ref<Resource> player_config; // ResonancePlayerConfig; null = behave as a plain AudioStreamPlayer3D

    // Frames between config cache refreshes; edits to the config show up after at most this delay.
    static const int kConfigCacheRefreshInterval = 15;
    struct BusRoutingKey {
        int bus_override = -1;
        StringName bus_name;
        int reverb_bus_override = -1;
        StringName reverb_bus_name;
        bool operator==(const BusRoutingKey& o) const {
            return bus_override == o.bus_override && bus_name == o.bus_name &&
                   reverb_bus_override == o.reverb_bus_override && reverb_bus_name == o.reverb_bus_name;
        }
    };
    BusRoutingKey bus_routing_key_;
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
        /// 0 = runtime config value, 1 = this resource's max_transmission_surfaces. Legacy -1 is read as 0.
        int max_transmission_surfaces_override = 0;
        float master_mix_level;
        float direct_mix_level, reflections_mix_level, pathing_mix_level;
        float reflections_eq_low, reflections_eq_mid, reflections_eq_high;
        int reflections_delay;
        int perspective_override;
        float perspective_factor;
        /// Seconds between full parameter pushes; 0 = every frame.
        float playback_parameter_min_interval;
        /// Meters the source must move to force a full push early; 0 = off.
        float playback_parameter_min_move;
        /// Seconds; smoothing time constant for simulated occlusion and transmission. 0 = off.
        float playback_coeff_smoothing_time;
        /// Linear and Curve modes only: the resource's distance_attenuation_simulation_enabled flag.
        bool linear_curve_use_sim_distance_attenuation = true;
        /// False = full direct gain, no distance attenuation model.
        bool distance_attenuation = true;
        /// Linear and Curve modes only: apply the distance curve to the reflections IR through a callback.
        bool use_distance_curve_for_reflections = false;
        bool reverb_send = true;
        bool simulation_occlusion_enabled = true;
        bool simulation_transmission_enabled = true;
        /// -1 = runtime config, 0 = raycast, 1 = volumetric.
        int occlusion_type_override = -1;
        /// -1 = runtime config, 0 = frequency independent, 1 = frequency dependent.
        int transmission_type_override = -1;
        /// -1 = runtime config, 0 = nearest, 1 = bilinear.
        int hrtf_interpolation_override = -1;
    } config_cache_;
    bool config_cache_valid_ = false;
    struct AttenuationSetupCache {
        int mode = -1;
        float min_distance = 0.0f;
        float max_distance = 0.0f;
        int num_curve_samples = 0;
        float curve_samples[resonance::kAttenuationCurveSamples]{};
        bool valid = false;
    } attenuation_setup_cache_;

    float coeff_smooth_occ_ = 0.0f;
    float coeff_smooth_tx_[3] = {1.0f, 1.0f, 1.0f};
    bool coeff_smooth_initialized_ = false;
    int32_t coeff_smooth_source_handle_ = -1;

    double playback_lod_time_since_full_ = 0.0;
    bool playback_lod_have_anchor_ = false;
    Vector3 playback_lod_anchor_pos_;
    PlaybackParameters last_pushed_playback_params_{};
    bool last_pushed_playback_params_valid_ = false;
    bool source_simulation_active_pushed_ = true;
    resonance::VoiceVirtualState voice_virtual_state_;
    bool player_sync_uses_physics_ = false;

    ResonanceDebugDrawer debug_drawer;
    ResonanceDebugData debug_overlay_last_data_{};
    bool debug_overlay_has_last_data_ = false;
    double debug_overlay_grace_timer_ = 0.0;

    ResonanceDirectivityDrawer directivity_drawer_;
    bool show_directivity_gizmo_ = false;

    void _update_stream_setup();
    /// With player_config, turns off the AudioStreamPlayer3D attenuation and max_distance so Steam Audio owns distance.
    void _apply_steam_mode_asp3d_guards();
    /// The gain from volume_db and max_db is applied to the dry input before the Steam DSP.
    void _refresh_effective_volume_cache();
    void _ensure_source_exists();
    /// A reinit recycles handle ids, so a handle from an older lifecycle epoch must be dropped.
    void _invalidate_source_handle_if_stale(ResonanceServer* srv);
    void reload_source_after_reinit();
    /// Nodes can enter the tree before the runtime exists, so the source is created lazily.
    /// Returns true if a new handle was created.
    bool _try_ensure_source_and_sync(ResonanceServer* srv, bool deferred_playback_push_if_playing);
    int32_t _create_simulation_source(ResonanceServer* srv);
    void _ensure_voice_sources(ResonanceServer* srv);
    void ensure_shared_reflection_on_main(ResonanceServer* srv);
    void cleanup_shared_reflection();
    static void shared_reflection_context_cleanup(void* userdata);
    void reflection_round_enter(ResonanceStreamPlayback* voice);
    void reflection_round_add(ResonanceStreamPlayback* voice);
    void reflection_round_leave(ResonanceStreamPlayback* voice);
    void reflection_apply_outside_block(ResonanceStreamPlayback* voice);
    bool reflection_try_apply(ResonanceStreamPlayback* voice, bool& out_reverb_to_player, float& out_dbg_reverb);
    bool shared_reflection_is_parametric() const;
    bool shared_reflection_tail_applyable(const ResonanceServer* srv) const;
    int shared_reflection_tail_samples() const;
    bool reflection_try_drain_tail(ResonanceStreamPlayback* voice, bool& out_reverb_to_player);
    IPLAudioBuffer* shared_reflection_direct_output();
    void _zero_shared_reflection_accum();
    void _collect_simulation_source_handles(std::vector<int32_t>& out) const;
    bool _live_voice_uses_source(int32_t handle) const;
    int32_t _first_live_voice_source(uint32_t* out_epoch) const;
    void _drain_voice_source_reclaims();
    void _reclaim_voice_source_on_main(int32_t handle, uint32_t epoch);
    void _destroy_owned_sources_for_shutdown();
    void _block_live_convolution_and_wait();
    struct PendingVoiceSourceReclaim {
        int32_t handle = -1;
        uint32_t epoch = 0;
    };
    std::mutex voice_source_reclaim_mutex_;
    std::vector<PendingVoiceSourceReclaim> voice_source_reclaims_;
    /// One convolution effect for every voice of this emitter. New plays must not reset it.
    ResonanceReflectionProcessor shared_reflection_;
    /// Time of reflection_try_apply calls that close a round outside any voice block, in nanoseconds.
    std::atomic<uint64_t> instrumentation_reflection_outside_block_ns_{0};
    IPLAudioBuffer shared_refl_accum_{};
    IPLContext shared_refl_context_ = nullptr;
    int shared_refl_frame_size_ = 0;
    bool shared_reflection_inited_ = false;
    float shared_prev_conv_mix_ = 0.0f;
    float shared_prev_param_mix_ = 0.0f;
    IPLReflectionEffectParams shared_tail_params_{};
    bool shared_tail_have_ = false;
    int32_t shared_tail_source_ = -1;
    uint32_t shared_tail_ir_gen_ = 0;
    static constexpr int kReflectionRoundVoices = 64;
    ResonanceStreamPlayback* refl_round_voices_[kReflectionRoundVoices] = {};
    int refl_round_seen_ = 0;
    int refl_round_expected_ = 0;
    bool refl_round_applied_ = false;
    bool refl_round_have_dry_ = false;
    ResonanceStreamPlayback* refl_round_closer_ = nullptr;
    int32_t refl_apply_reader_ = -1;
    float refl_round_wet_occ_ = 1.0f;
    float refl_round_mix_ = 1.0f;
    float refl_round_eq_[3] = {1.0f, 1.0f, 1.0f};
    int refl_round_delay_ = -1;
    std::vector<int32_t> attenuation_callback_handles_;
    void _deferred_try_ensure_source_after_config();
    void _start_reverb_send_child_if_needed();
    bool _reverb_send_wanted(const ResonanceServer* srv) const;
    /// Per-source reflections or pathing wet on this player; the listener reverb goes through the send instead.
    bool _playback_wet_enabled(const ResonanceServer* srv) const;
    void _sync_reverb_send_child(const ResonanceServer* srv);
    bool warned_source_handle_create_failed_ = false;
    void _ensure_config_valid();
    void _ensure_config_and_apply_source(int32_t pathing_batch); // Always queued, never dropped
    ResonanceServer::SourceUpdateParams _build_source_update_params(ResonanceServer* srv, int32_t pathing_batch) const;
    /// defer_if_sim_mutex_busy: never wait on RunReflections; without batch updates a busy mutex drops the update.
    void _apply_update_source(int32_t pathing_batch, bool defer_if_sim_mutex_busy = false);

    void _setup_attenuation(ResonanceServer* srv);
    void _prepare_source_for_simulation(ResonanceServer* srv);
    int32_t _resolve_pathing_probe_batch();
    /// A source of a stopped player (tail drained) or a virtual one costs no simulation; play() re-arms it before
    /// the first mix.
    void _sync_source_simulation_activity(ResonanceServer* srv);
    bool _source_simulation_wanted() const;
    // Returns true when the virtual state flipped.
    bool _update_voice_virtual_state(ResonanceServer* srv, double delta_seconds);
    /// Runs after _prepare_source_for_simulation.
    void _apply_playback_params_from_simulation(ResonanceServer* srv, ResonanceDebugData* opt_debug_out, double delta_seconds);
    void _apply_playback_coeff_refresh_from_simulation(ResonanceServer* srv, double delta_seconds);
    bool _playback_lod_should_apply_playback_params(double delta, bool debug_hud_active, const Vector3& source_pos);
    void _apply_process_mode_for_tracer();
    void _ensure_listener_pose_synced_for_playback(ResonanceServer* srv);
    void _sync_playback_simulation_frame(double delta, ResonanceServer* srv, bool show_debug_hud);
    /// Same push as one _process tick. opt_debug_out gets the debug fields but not the signal levels.
    void _push_playback_parameters_from_simulation(ResonanceServer* srv, ResonanceDebugData* opt_debug_out, double delta_seconds,
                                                   bool run_prepare = true);
    void _deferred_push_playback_parameters();
    void _sync_player_debug_drawer(double delta, ResonanceServer* srv, const ResonanceDebugData& dbg_data, bool hud_active);
    void _compute_listener_data(ResonanceServer* srv, Vector3& out_listener_pos, IPLCoordinateSpace3& out_listener_orient);
    void _compute_attenuation(float dist, const OcclusionData& occ_data, float& out_attenuation);
    /// Takes simulated or manual values and smooths them when smoothing is on.
    void _resolve_playback_occ_tx_directivity(const OcclusionData& occ_data, double delta_seconds,
                                              bool reset_smooth_state_when_not_smoothing,
                                              float& occ_val, float& tx_low, float& tx_mid, float& tx_high,
                                              float& directivity_val);
    Vector3 _apply_perspective_correction(Vector3 listener_pos, Viewport* vp, bool apply_perspective, float perspective_factor_val);
    PlaybackParameters _build_playback_params(const Vector3& listener_pos, const IPLCoordinateSpace3& listener_orient,
                                              float attenuation, float dist, const Vector3& effective_source_pos,
                                              float occ_val, float tx_low, float tx_mid, float tx_high, float directivity_val, const Vector3& air_abs,
                                              bool has_reverb, bool direct_enabled, bool reverb_enabled);

    ResonanceStreamPlayback* _get_resonance_playback();
    void _aggregate_debug_signal_levels(float& out_direct, float& out_reverb, float& out_pathing);
    bool _want_player_debug_ui(const ResonanceServer* srv) const;
    void _tick_debug_overlay_grace_timer(double delta, ResonanceServer* srv);
    bool _compute_show_debug_hud(const ResonanceServer* srv) const;
    void _enrich_debug_overlay_signal_levels(ResonanceDebugData& dbg_data, ResonanceServer* srv, bool reflections_hud_active,
                                             bool grace_refresh_meters);
    void _apply_debug_hud_overlay_hints(ResonanceDebugData& dbg_data, bool sim_hold);
    void _present_player_debug_overlay_from_cache(double delta, ResonanceServer* srv, bool show_debug_hud);
    void _nexus_deferred_spawn_anim_audio_helper();
    void _nexus_deferred_emit_finished();

    StringName reverb_send_bus_;
    bool reverb_send_active_ = false;
    /// With player_config, a helper converts the scene's audio animation tracks to play_animation_audio_clip calls
    /// once; native audio tracks would bypass Steam Audio.
    bool convert_anim_audio_runtime_ = false;
    bool exclude_from_debug_ = false;
    /// Registers descendant CollisionObject3D RIDs as ray excludes for the Custom scene.
    bool auto_exclude_colliders_ = true;
    int physics_auto_exclude_resync_counter_ = 0;
    std::vector<RID> registered_physics_auto_exclude_rids_;
    void _sync_physics_ray_auto_exclude_rids();
    void _clear_physics_ray_auto_exclude_rids();

    /// `finished` fires once at dry EOS; the wet and path tails may keep running after it.
    bool dry_finished_emitted_ = false;
    bool dry_finished_deferred_queued_ = false;
    uint64_t play_serial_ = 0;
    resonance::SpawnTiming spawn_timing_;
    uint64_t dry_finished_deferred_serial_ = 0;
    /// Soft-stop watchdog, seconds since stop(); -1 = inactive. Forces AudioStreamPlayer3D::stop only when
    /// mix callbacks stall or the absolute cap is hit.
    double soft_stop_elapsed_sec_ = -1.0;
    double soft_stop_mix_stall_sec_ = 0.0;
    uint64_t soft_stop_last_mix_calls_ = 0;

    float _config_float(const char* key, float default_val) const;
    int _config_int(const char* key, int default_val) const;
    bool _config_bool(const char* key, bool default_val) const;
    Ref<Curve> _config_curve(const char* key, const Ref<Curve>& default_val) const;
    void _refresh_config_cache();
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
    void _physics_process(double delta) override;
    void _exit_tree() override;
    void set_stream(const Ref<AudioStream>& p_stream);
    Ref<AudioStream> get_stream() const;
    Ref<AudioStream> get_inner_stream() const;
    Ref<AudioStreamPlayback> get_inner_stream_playback();

    /// Wraps AudioStreamPlayer3D::play: also sets up the internal stream, source handle and reverb send child.
    void play(float from_position = 0.0f);
    void play_stream(double from_position = 0.0);
    /// For AnimationPlayer method tracks: sets the stream and plays it. from_position is the clip's start offset.
    void play_animation_audio_clip(const Ref<AudioStream>& p_stream, float from_position = 0.0f);
    void stop();

    void set_pathing_probe_volume(const NodePath& p_path);
    NodePath get_pathing_probe_volume() const;
    float get_effective_volume_linear_cached() const { return cached_effective_volume_linear_.load(std::memory_order_relaxed); }
    /// Sends pathing batch -1 now, so the worker stops reading a batch that is about to be removed.
    /// Keeps pathing_probe_volume; clear_pathing_probe_immediate also empties it.
    void detach_pathing_probe_immediate();
    void clear_pathing_probe_immediate();
    void set_player_config(const Ref<Resource>& p_config);
    Ref<Resource> get_player_config() const;
    /// Bus of the listener reverb; empty when the player already outputs to that bus.
    void set_reverb_send_bus(const StringName& p_bus);
    StringName get_reverb_send_bus() const { return reverb_send_bus_; }
    bool is_reverb_send_active() const { return reverb_send_active_; }

    void set_exclude_from_debug(bool p_exclude);
    bool get_exclude_from_debug() const { return exclude_from_debug_; }

    void set_auto_exclude_colliders(bool p_enable);
    bool get_auto_exclude_colliders() const { return auto_exclude_colliders_; }

    void set_convert_anim_audio_runtime(bool p_enable);
    bool get_convert_anim_audio_runtime() const { return convert_anim_audio_runtime_; }

    void set_show_directivity_gizmo(bool p_enable);
    bool get_show_directivity_gizmo() const { return show_directivity_gizmo_; }

    /// Queues the config for the Steam source inputs. flags is unused.
    void set_inputs(int flags = -1);
    Dictionary get_outputs() const;

    /// Returns a line list. input_mode 0 draws the dipole from weight and power; 1 draws a sphere with the arrow
    /// scaled by user_value. size is the radius in meters.
    static PackedVector3Array build_directivity_gizmo_lines(bool enabled, int input_mode, float weight, float power, float user_value, float size);

    /// Connected to the player_config's Resource.changed.
    void _on_player_config_changed_refresh_gizmo();
    void _apply_player_config_bus_routing();
    BusRoutingKey _bus_routing_key_from_config() const;
    void _sync_bus_routing_with_config();

    /// Empty without a player_config.
    Dictionary get_audio_instrumentation();
    void reset_audio_instrumentation();
    /// Main-thread microseconds spent building this player since it was created, split into voice_dsp_us,
    /// reflection_effect_us, source_us and params_push_us.
    Dictionary get_spawn_timing() const;
    bool is_spatial_output_ready() const;

    /// Keeps the reverb send child playing while a voice still has samples in its reverb ring.
    bool any_internal_playback_has_reverb_ring_data() const;
    bool internal_is_playing_for_audio() const { return playing_for_audio_.load(std::memory_order_acquire); }
};
} // namespace godot

VARIANT_ENUM_CAST(ResonancePlayer::AttenuationMode);

#endif