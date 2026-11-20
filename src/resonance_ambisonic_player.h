#ifndef RESONANCE_AMBISONIC_PLAYER_H
#define RESONANCE_AMBISONIC_PLAYER_H

#include "resonance_constants.h"
#include <atomic>
#include <godot_cpp/classes/audio_frame.hpp>
#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_playback.hpp>
#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <phonon.h>
#include <vector>

#include "resonance_exit_pins.h"
#include "resonance_latest_value_slot.h"
#include "resonance_processor_ambisonic.h"
#include "resonance_ring_buffer.h"

namespace godot {

class ResonanceServer;

struct AmbisonicPlaybackParameters {
    /// Listener basis in Godot world space; used when combined_matrix_decode is false.
    IPLCoordinateSpace3 listener_orientation{};
    /// Decode orientation comes from combined_decode_orientation and the rotation effect is bypassed.
    bool combined_matrix_decode = false;
    IPLCoordinateSpace3 combined_decode_orientation{};
    bool rotation_enabled = true;
    bool apply_hrtf = true;
    bool input_is_sn3d = true;
    bool apply_output_gain = true;
};

class ResonanceAmbisonicInternalPlayback : public AudioStreamPlayback {
    GDCLASS(ResonanceAmbisonicInternalPlayback, AudioStreamPlayback)

  public:
    ResonanceAmbisonicInternalPlayback(const ResonanceAmbisonicInternalPlayback&) = delete;
    ResonanceAmbisonicInternalPlayback(ResonanceAmbisonicInternalPlayback&&) = delete;
    // Assignment implicitly deleted when copy ctor is deleted; explicit decl conflicts with Godot base.

  private:
    int frame_size_ = resonance::kGodotDefaultFrameSize; // Steam Audio block size from ResonanceServer

    // One playback per ambisonic channel: (order+1)^2.
    std::vector<Ref<AudioStreamPlayback>> channel_playbacks;
    int ambisonic_order = 1;
    resonance::ExitPinned exit_pin_{this};

    resonance::LatestValueSlot<AmbisonicPlaybackParameters> params_slot_;
    /// Main thread only: the last value passed to update_parameters.
    AmbisonicPlaybackParameters params_latest_;
    /// Audio thread only.
    AmbisonicPlaybackParameters params_current;

    // Main publishes processor, context and sa_out_buffer with a release store of true.
    std::atomic<bool> is_initialized{false};
    std::atomic<bool> steam_context_stale_{false};
    std::atomic<bool> processor_config_stale_{false};
    // _start may run on main or audio; only _mix resets the processor, and only while it owns it.
    std::atomic<bool> processor_reset_requested_{false};
    // Written only by _mix, so _is_playing can ask from any thread without reading the rings.
    std::atomic<bool> has_pending_output_{false};
    std::atomic<bool> stop_requested = false;
    int current_sample_rate = 48000;
    IPLContext context = nullptr;
    uint64_t next_prewarm_retry_msec_ = 0;

    ResonanceAmbisonicProcessor processor;

    IPLAudioBuffer sa_out_buffer{};

    // Interleaved, (order+1)^2 channels.
    RingBuffer<float> input_ring;
    RingBuffer<float> output_ring_l;
    RingBuffer<float> output_ring_r;

    std::vector<float> temp_interleaved_input;
    std::vector<float> temp_output_l;
    std::vector<float> temp_output_r;
    // One per ambisonic channel, reused across _mix calls so the audio thread does not allocate.
    std::vector<PackedVector2Array> channel_mix_bufs_;
    float last_mix_out_l_ = 0.0f;
    float last_mix_out_r_ = 0.0f;
    bool last_mix_out_valid_ = false;
    bool geometry_gate_was_holding_decode_ = false;
    bool channel_length_mismatch_warned_ = false;

    bool _has_pending_output() const;

    void _lazy_init_steam_audio();
    void _cleanup_steam_audio();
    bool _audio_owns_processor() const;
    bool _processor_matches(const AmbisonicPlaybackParameters& p) const;
    void _reinit_ambisonic_processor_on_main(ResonanceServer* srv);
    /// Returns false when input was not consumed (caller must break pump loop).
    bool _process_steam_audio_block();
    void _sync_params();

    int32_t pull_channel_samples(float rate_scale, int32_t frames, int num_channels);
    void interleave_input_chunk(int32_t first_frame, int32_t frame_count, int num_channels);
    void push_interleaved_input(int32_t samples_read, int num_channels);
    int32_t mix_w_passthrough(AudioFrame* buffer, int32_t samples_read);
    void pull_stereo_output(AudioFrame* buffer, int32_t samples_read, bool holding_decode);

  public:
    ResonanceAmbisonicInternalPlayback();
    ~ResonanceAmbisonicInternalPlayback();

    /// Called by ResonanceServer before iplContextRelease (userdata = this).
    static void ipl_context_reinit_cleanup(void* userdata);

    void set_channel_playbacks(const Array& playbacks, int p_order);

    void update_parameters(const AmbisonicPlaybackParameters& p_params);

    /// Main thread: allocates the IPL buffers before the first _mix.
    bool prewarm_steam_audio();
    /// Main thread: tears down and prewarms again after the audio thread flagged a stale IPL context.
    void resolve_stale_steam_context_on_main();

    virtual int32_t _mix(AudioFrame* buffer, float rate_scale, int32_t frames) override;
    virtual void _start(double from_pos) override;
    virtual void _stop() override;
    virtual bool _is_playing() const override;
    virtual int _get_loop_count() const override;
    virtual double _get_playback_position() const override;
    virtual void _seek(double position) override;

  protected:
    static void _bind_methods() {}
};

class ResonanceAmbisonicInternalStream : public AudioStream {
    GDCLASS(ResonanceAmbisonicInternalStream, AudioStream)
  private:
    Array channel_streams;
    int ambisonic_order = 1;

    void sync_channel_streams_size_to_order();

  public:
    void set_channel_streams(const Array& p_streams);
    Array get_channel_streams() const;

    void set_ambisonic_order(int p_order);
    int get_ambisonic_order() const;

    virtual Ref<AudioStreamPlayback> _instantiate_playback() const override;
    virtual String _get_stream_name() const override { return "ResonanceAmbisonicInternal"; }
    virtual double _get_length() const override;
    virtual bool _is_monophonic() const override { return false; }

    void _notification(int p_what);

  protected:
    static void _bind_methods();
};

class ResonanceAmbisonicPlayer : public AudioStreamPlayer {
    GDCLASS(ResonanceAmbisonicPlayer, AudioStreamPlayer)
  private:
    bool rotation_enabled = true;          ///< Rotates the bed by the listener orientation.
    bool use_bed_scene_orientation = true; ///< Takes the bed orientation from ambisonic_orientation_node or the parent Node3D.
    NodePath ambisonic_orientation_node{}; ///< Empty uses the first Node3D ancestor.
    bool apply_hrtf = true;
    bool input_is_sn3d = true;
    bool apply_output_gain = true;

  protected:
    static void _bind_methods();
    void _validate_property(PropertyInfo& p_property) const;

  public:
    ResonanceAmbisonicPlayer() = default;
    ~ResonanceAmbisonicPlayer() = default;

    void _ready() override;
    void _process(double delta) override;

    void set_rotation_enabled(bool p_enabled);
    bool is_rotation_enabled() const { return rotation_enabled; }

    void set_use_bed_scene_orientation(bool p_enabled);
    bool get_use_bed_scene_orientation() const { return use_bed_scene_orientation; }

    void set_ambisonic_orientation_node(const NodePath& p_path);
    NodePath get_ambisonic_orientation_node() const { return ambisonic_orientation_node; }

    void set_apply_hrtf(bool p_enabled);
    bool get_apply_hrtf() const { return apply_hrtf; }

    void set_input_is_sn3d(bool p_sn3d);
    bool get_input_is_sn3d() const { return input_is_sn3d; }

    void set_apply_output_gain(bool p_enabled);
    bool get_apply_output_gain() const { return apply_output_gain; }

    // Plays the ResonanceAmbisonicInternalStream assigned to the inherited stream property.
};

} // namespace godot

#endif