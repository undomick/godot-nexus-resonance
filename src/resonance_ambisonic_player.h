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

#include "resonance_processor_ambisonic.h"
#include "resonance_ring_buffer.h"

namespace godot {

class ResonanceServer;

struct AmbisonicPlaybackParameters {
    /// Listener/camera-facing basis in Godot world space (Steam Audio IPL). Used when
    /// [member combined_matrix_decode] is false.
    IPLCoordinateSpace3 listener_orientation{};
    /// When true: decode orientation is taken from combined bed + listener row-major matrices (IPL Ambisonics rotation bypassed).
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

    // Playbacks for each Ambisonic channel (4 for 1st, 9 for 2nd, 16 for 3rd order)
    std::vector<Ref<AudioStreamPlayback>> channel_playbacks;
    int ambisonic_order = 1;

    std::atomic<bool> params_dirty = false;
    AmbisonicPlaybackParameters params_next;
    AmbisonicPlaybackParameters params_current;

    bool is_initialized = false;
    std::atomic<bool> stop_requested = false;
    int current_sample_rate = 48000;
    IPLContext context = nullptr;

    ResonanceAmbisonicProcessor processor;

    IPLAudioBuffer sa_out_buffer{};

    // RingBuffers
    // Input: Interleaved N channels (N = (order+1)^2)
    RingBuffer<float> input_ring;
    // Output: Stereo (Left/Right)
    RingBuffer<float> output_ring_l;
    RingBuffer<float> output_ring_r;

    // Temp buffer for processing loop
    std::vector<float> temp_interleaved_input;
    // Temp buffers for batch read from output ring buffers (avoids per-sample memcpy)
    std::vector<float> temp_output_l;
    std::vector<float> temp_output_r;
    // Reused per _mix: one PackedVector2Array per Ambisonic channel (no per-frame heap alloc).
    std::vector<PackedVector2Array> channel_mix_bufs_;

    bool _has_pending_output() const;

    void _lazy_init_steam_audio();
    void _cleanup_steam_audio();
    void _ensure_ambisonic_processor(ResonanceServer* srv);
    void _process_steam_audio_block();
    void _sync_params();

    int32_t pull_channel_samples(float rate_scale, int32_t frames, int num_channels);
    void push_interleaved_input(int32_t samples_read, int num_channels);
    void pull_stereo_output(AudioFrame* buffer, int32_t samples_read);

  public:
    ResonanceAmbisonicInternalPlayback();
    ~ResonanceAmbisonicInternalPlayback();

    /// Called by ResonanceServer before iplContextRelease (userdata = this).
    static void ipl_context_reinit_cleanup(void* userdata);

    void set_channel_playbacks(const Array& playbacks, int p_order);

    void update_parameters(const AmbisonicPlaybackParameters& p_params);

    /// Main-thread IPL buffer setup before first `_mix` (same as ResonanceStreamPlayback).
    bool prewarm_steam_audio();

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
    bool rotation_enabled = true;          ///< Apply Ambisonics rotation (listener orientation). Disable for fixed/world-space decode.
    bool use_bed_scene_orientation = true; ///< When true, resolve HOA bed orientation from [member ambisonic_orientation_node] or parent Node3D.
    NodePath ambisonic_orientation_node{}; ///< Optional Node3D for HOA bed basis; empty uses first Node3D ancestor.
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

    // This player uses the standard "stream" property of AudioStreamPlayer.
    // The user must assign a ResonanceAmbisonicInternalStream to it.
    // We provide a helper to play easily.
};

} // namespace godot

#endif