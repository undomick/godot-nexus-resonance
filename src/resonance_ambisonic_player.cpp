#include "resonance_ambisonic_player.h"
#include "resonance_ambisonics_decode_orientation.h"
#include "resonance_constants.h"
#include "resonance_log.h"
#include "resonance_server.h"
#include <algorithm>
#include <cstring>
#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/variant.hpp>

using namespace godot;

namespace {

inline bool ambisonic_needs_rotation_effect(const AmbisonicPlaybackParameters& p) {
    return !p.combined_matrix_decode && p.rotation_enabled;
}

IPLVector3 to_decoder_axis(const Vector3& v_world) {
    return resonance::ipl_dir_from_godot_world((float)v_world.x, (float)v_world.y, (float)v_world.z);
}

void fill_ambisonics_bed_local_to_world_row16(const Transform3D& src_world, float matrix_row_major[16]) {
    Basis b = src_world.get_basis();
    Vector3 fwd_w = (-b.get_column(2)).normalized();
    Vector3 up_w = b.get_column(1).normalized();
    Vector3 right_w = fwd_w.cross(up_w).normalized();
    up_w = right_w.cross(fwd_w).normalized();
    IPLVector3 ru = to_decoder_axis(right_w);
    IPLVector3 uu = to_decoder_axis(up_w);
    IPLVector3 fu = to_decoder_axis(fwd_w);
    std::memset(matrix_row_major, 0, 16 * sizeof(float));
    matrix_row_major[0] = ru.x;
    matrix_row_major[1] = ru.y;
    matrix_row_major[2] = ru.z;
    matrix_row_major[4] = uu.x;
    matrix_row_major[5] = uu.y;
    matrix_row_major[6] = uu.z;
    matrix_row_major[8] = fu.x;
    matrix_row_major[9] = fu.y;
    matrix_row_major[10] = fu.z;
    IPLVector3 ou = to_decoder_axis(src_world.get_origin());
    matrix_row_major[12] = ou.x;
    matrix_row_major[13] = ou.y;
    matrix_row_major[14] = ou.z;
    matrix_row_major[15] = 1.0f;
}

void basis_to_ambisonics_listener_rotation_row16(const Basis& rotary_world_to_listener, float L[16]) {
    // Caller zeroes matrix; row-major 4×4 with columns stored at indices (0,4,8), (1,5,9), (2,6,10).
    for (int col = 0; col < 3; ++col) {
        Vector3 c = rotary_world_to_listener.get_column(col);
        L[0 + col * 4 + 0] = c.x;
        L[0 + col * 4 + 1] = c.y;
        L[0 + col * 4 + 2] = c.z;
    }
}

void fill_ambisonics_listener_world_to_listener_row16(const Transform3D& listener_world_inverse, float L[16]) {
    const Basis flip = Basis::from_scale(Vector3(1.0f, 1.0f, -1.0f));
    Basis conjugated = flip * listener_world_inverse.get_basis() * flip;
    std::memset(L, 0, 16 * sizeof(float));
    basis_to_ambisonics_listener_rotation_row16(conjugated, L);
    IPLVector3 t = to_decoder_axis(listener_world_inverse.get_origin());
    L[12] = t.x;
    L[13] = t.y;
    L[14] = t.z;
    L[15] = 1.0f;
}

Node3D* resolve_ambisonic_bed_orientation_node(ResonanceAmbisonicPlayer* player) {
    if (!player || !player->get_use_bed_scene_orientation())
        return nullptr;
    const NodePath p = player->get_ambisonic_orientation_node();
    if (!p.is_empty()) {
        Node* hit = player->get_node_or_null(p);
        return Object::cast_to<Node3D>(hit);
    }
    for (Node* cur = player->get_parent(); cur != nullptr; cur = cur->get_parent()) {
        if (Node3D* spatial = Object::cast_to<Node3D>(cur))
            return spatial;
    }
    return nullptr;
}

AmbisonicPlaybackParameters build_ambisonic_params_from_player(ResonanceAmbisonicPlayer* player) {
    AmbisonicPlaybackParameters params{};
    IPLCoordinateSpace3 listener_orient{};
    listener_orient.origin = {0.0f, 0.0f, 0.0f};

    Transform3D listener_world_inverse{};
    Viewport* vp = player->get_viewport();
    Camera3D* cam = vp ? vp->get_camera_3d() : nullptr;
    const bool have_camera = cam != nullptr;
    if (have_camera) {
        Transform3D cam_xform = cam->get_global_transform();
        listener_world_inverse = cam_xform.affine_inverse();

        Vector3 forward = -cam_xform.basis.get_column(2);
        Vector3 up = cam_xform.basis.get_column(1);
        Vector3 right = cam_xform.basis.get_column(0);
        listener_orient.ahead = {forward.x, forward.y, forward.z};
        listener_orient.up = {up.x, up.y, up.z};
        listener_orient.right = {right.x, right.y, right.z};
    } else {
        listener_orient.ahead = {0.0f, 0.0f, -1.0f};
        listener_orient.up = {0.0f, 1.0f, 0.0f};
        listener_orient.right = {1.0f, 0.0f, 0.0f};
    }

    params.listener_orientation = listener_orient;
    params.rotation_enabled = player->is_rotation_enabled();
    params.apply_hrtf = player->get_apply_hrtf();
    params.input_is_sn3d = player->get_input_is_sn3d();
    params.apply_output_gain = player->get_apply_output_gain();

    Node3D* bed = resolve_ambisonic_bed_orientation_node(player);
    const bool use_combined_bed_listener_matrices = player->is_rotation_enabled() && bed != nullptr && have_camera;
    params.combined_matrix_decode = use_combined_bed_listener_matrices;
    if (use_combined_bed_listener_matrices) {
        float S[16]{};
        float L[16]{};
        fill_ambisonics_bed_local_to_world_row16(bed->get_global_transform(), S);
        fill_ambisonics_listener_world_to_listener_row16(listener_world_inverse, L);
        resonance::ambisonics_decode_orientation_row_major(S, L, &params.combined_decode_orientation);
    }
    return params;
}

} // namespace

ResonanceAmbisonicInternalPlayback::ResonanceAmbisonicInternalPlayback() {
    params_next.listener_orientation.ahead = {0, 0, -1};
    params_next.listener_orientation.up = {0, 1, 0};
    params_next.listener_orientation.right = {1, 0, 0};
    params_next.listener_orientation.origin = {0, 0, 0};
    params_current = params_next;

    // Buffers initialized in set_channel_playbacks when order is known
}

ResonanceAmbisonicInternalPlayback::~ResonanceAmbisonicInternalPlayback() { _cleanup_steam_audio(); }

void ResonanceAmbisonicInternalPlayback::ipl_context_reinit_cleanup(void* userdata) {
    if (!userdata)
        return;
    static_cast<ResonanceAmbisonicInternalPlayback*>(userdata)->_cleanup_steam_audio();
}

void ResonanceAmbisonicInternalPlayback::set_channel_playbacks(const Array& playbacks, int p_order) {
    ambisonic_order = CLAMP(p_order, 1, 3);
    int num_channels = resonance::ambisonic_num_channels_for_order(ambisonic_order);

    channel_playbacks.clear();
    channel_playbacks.reserve(num_channels);
    for (int i = 0; i < num_channels; i++) {
        Ref<AudioStreamPlayback> pb;
        if (i < playbacks.size())
            pb = playbacks[i];
        channel_playbacks.push_back(pb);
    }
    channel_mix_bufs_.resize(num_channels);

    // Resize buffers
    size_t in_capacity = resonance::kRingBufferCapacity * num_channels;
    input_ring.resize(in_capacity);

    size_t out_capacity = resonance::kRingBufferCapacity;
    output_ring_l.resize(out_capacity);
    output_ring_r.resize(out_capacity);

    temp_interleaved_input.resize(static_cast<size_t>(resonance::kGodotDefaultFrameSize) * static_cast<size_t>(num_channels)); // Resized to frame_size_ in _lazy_init
}

void ResonanceAmbisonicInternalPlayback::update_parameters(const AmbisonicPlaybackParameters& p_params) {
    params_next = p_params;
    params_dirty.store(true, std::memory_order_release);
}

void ResonanceAmbisonicInternalPlayback::_sync_params() {
    if (params_dirty.load(std::memory_order_acquire)) {
        params_current = params_next;
        params_dirty.store(false, std::memory_order_release);
    }
}

bool ResonanceAmbisonicInternalPlayback::_has_pending_output() const {
    return output_ring_l.get_available_read() > 0 || output_ring_r.get_available_read() > 0 ||
           input_ring.get_available_read() > 0;
}

void ResonanceAmbisonicInternalPlayback::_cleanup_steam_audio() {
    if (ResonanceServer* reg_srv = ResonanceServer::get_singleton())
        reg_srv->unregister_ipl_context_client(this);

    processor.cleanup();
    if (context && sa_out_buffer.data) {
        iplAudioBufferFree(context, &sa_out_buffer);
    }
    memset(&sa_out_buffer, 0, sizeof(sa_out_buffer));
    context = nullptr;
    is_initialized = false;
    steam_context_stale_.store(false, std::memory_order_release);

    input_ring.clear();
    output_ring_l.clear();
    output_ring_r.clear();
}

void ResonanceAmbisonicInternalPlayback::_ensure_ambisonic_processor(ResonanceServer* srv) {
    if (!srv || !srv->is_initialized() || !context)
        return;
    _sync_params();
    const AmbisonicPlaybackParameters& p = params_current;
    const bool use_rot_eff = ambisonic_needs_rotation_effect(p);
    if (processor.matches_config(ambisonic_order, use_rot_eff, p.apply_hrtf, p.input_is_sn3d, p.apply_output_gain))
        return;
    processor.cleanup();
    processor.initialize(context, current_sample_rate, frame_size_, ambisonic_order, use_rot_eff, p.apply_hrtf, p.input_is_sn3d,
                         p.apply_output_gain, srv->get_hrtf_handle());
}

bool ResonanceAmbisonicInternalPlayback::prewarm_steam_audio() {
    if (is_initialized)
        return true;
    _lazy_init_steam_audio();
    return is_initialized;
}

void ResonanceAmbisonicInternalPlayback::resolve_stale_steam_context_on_main() {
    // Stale: teardown old IPL. Always retry prewarm when uninitialized (late server
    // ready, failed first prewarm, or reinit cleanup which clears the stale flag).
    if (steam_context_stale_.load(std::memory_order_acquire)) {
        _cleanup_steam_audio();
        steam_context_stale_.store(false, std::memory_order_release);
    }
    if (!is_initialized)
        prewarm_steam_audio();
}

void ResonanceAmbisonicInternalPlayback::_lazy_init_steam_audio() {
    // Main-thread only (via prewarm_steam_audio). Do not call from _mix.
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!srv || !srv->is_initialized())
        return;

    current_sample_rate = srv->get_sample_rate();
    frame_size_ = srv->get_audio_frame_size();
    context = srv->get_context_handle();

    int num_channels = resonance::ambisonic_num_channels_for_order(ambisonic_order);
    temp_interleaved_input.resize(static_cast<size_t>(frame_size_) * static_cast<size_t>(num_channels));

    processor.initialize(context, current_sample_rate, frame_size_, ambisonic_order, ambisonic_needs_rotation_effect(params_current),
                         params_current.apply_hrtf, params_current.input_is_sn3d, params_current.apply_output_gain,
                         srv->get_hrtf_handle());

    // Allocate Output Buffer (Stereo)
    if (iplAudioBufferAllocate(context, 2, frame_size_, &sa_out_buffer) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceAmbisonicPlayer: Buffer allocation failed (IPLerror).");
        processor.cleanup();
        return;
    }
    if (!sa_out_buffer.data) {
        ResonanceLog::error("ResonanceAmbisonicPlayer: Buffer allocation returned null.");
        processor.cleanup();
        return;
    }

    is_initialized = true;
    if (ResonanceServer* reg_srv = ResonanceServer::get_singleton())
        reg_srv->register_ipl_context_client(this, &ResonanceAmbisonicInternalPlayback::ipl_context_reinit_cleanup);
    ResonanceLog::info("Nexus Resonance: Ambisonic DSP Initialized (Order: " + String::num(ambisonic_order) + ").");
}

void ResonanceAmbisonicInternalPlayback::_process_steam_audio_block() {
    // Crash protection: validate buffers before use
    if (!sa_out_buffer.data || !sa_out_buffer.data[0] || !sa_out_buffer.data[1])
        return;

    int num_channels = resonance::ambisonic_num_channels_for_order(ambisonic_order);
    size_t block_samples = static_cast<size_t>(frame_size_) * static_cast<size_t>(num_channels);

    input_ring.read(temp_interleaved_input.data(), block_samples);

    ResonanceServer* srv = ResonanceServer::get_singleton();
    _ensure_ambisonic_processor(srv);
    if (srv && srv->is_initialized() && !srv->is_spatial_audio_output_ready()) {
        for (int ch = 0; ch < sa_out_buffer.numChannels && sa_out_buffer.data && sa_out_buffer.data[ch]; ch++)
            memset(sa_out_buffer.data[ch], 0, frame_size_ * sizeof(float));
    } else {
        IPLHRTF hrtf = (srv && params_current.apply_hrtf) ? srv->get_hrtf_handle() : nullptr;
        processor.process(temp_interleaved_input.data(), block_samples, sa_out_buffer, params_current.combined_matrix_decode,
                          params_current.listener_orientation, params_current.combined_decode_orientation, hrtf);
    }

    output_ring_l.write(sa_out_buffer.data[0], frame_size_);
    output_ring_r.write(sa_out_buffer.data[1], frame_size_);
}

int32_t ResonanceAmbisonicInternalPlayback::pull_channel_samples(float rate_scale, int32_t frames, int num_channels) {
    // Channel 0 (W) drives the sample count; mix it once and bail if the source produced nothing.
    channel_mix_bufs_[0] = channel_playbacks[0]->mix_audio(rate_scale, frames);
    const int32_t samples_read = channel_mix_bufs_[0].size();
    if (samples_read == 0)
        return 0;

    for (int c = 1; c < num_channels; c++) {
        if (c < (int)channel_playbacks.size() && channel_playbacks[c].is_valid())
            channel_mix_bufs_[c] = channel_playbacks[c]->mix_audio(rate_scale, frames);
        else
            channel_mix_bufs_[c].clear();
    }
    return samples_read;
}

void ResonanceAmbisonicInternalPlayback::push_interleaved_input(int32_t samples_read, int num_channels) {
    const size_t interleaved_count = static_cast<size_t>(samples_read) * static_cast<size_t>(num_channels);
    if (temp_interleaved_input.size() < interleaved_count)
        temp_interleaved_input.resize(interleaved_count);

    for (int i = 0; i < samples_read; i++) {
        for (int c = 0; c < num_channels; c++) {
            float sample = (c < (int)channel_mix_bufs_.size() && (int)channel_mix_bufs_[c].size() > i)
                               ? channel_mix_bufs_[c][i].x
                               : 0.0f;
            temp_interleaved_input[static_cast<size_t>(i) * static_cast<size_t>(num_channels) + static_cast<size_t>(c)] = sample;
        }
    }
    const size_t to_write = std::min(interleaved_count, input_ring.get_available_write());
    if (to_write > 0)
        input_ring.write(temp_interleaved_input.data(), to_write);
}

void ResonanceAmbisonicInternalPlayback::pull_stereo_output(AudioFrame* buffer, int32_t samples_read) {
    const int available = (int)output_ring_l.get_available_read();
    const int valid_copy = (samples_read < available) ? samples_read : available;

    if (valid_copy > 0) {
        const size_t copy_size = static_cast<size_t>(valid_copy);
        if (temp_output_l.size() < copy_size)
            temp_output_l.resize(copy_size);
        if (temp_output_r.size() < copy_size)
            temp_output_r.resize(copy_size);
        output_ring_l.read(temp_output_l.data(), copy_size);
        output_ring_r.read(temp_output_r.data(), copy_size);
        for (int i = 0; i < valid_copy; i++) {
            buffer[i].left = temp_output_l[i];
            buffer[i].right = temp_output_r[i];
        }
    }

    for (int i = valid_copy; i < samples_read; i++) {
        buffer[i].left = 0.0f;
        buffer[i].right = 0.0f;
    }
}

int32_t ResonanceAmbisonicInternalPlayback::_mix(AudioFrame* buffer, float rate_scale, int32_t frames) {
    if (ResonanceServer::ipl_audio_teardown_active()) {
        for (int32_t i = 0; i < frames; i++) {
            buffer[i].left = 0.0f;
            buffer[i].right = 0.0f;
        }
        return frames;
    }
    if (channel_playbacks.empty() || !channel_playbacks[0].is_valid())
        return 0;

    _sync_params();

    ResonanceServer* srv_guard = ResonanceServer::get_singleton();
    if (is_initialized && srv_guard && srv_guard->is_initialized() && context != srv_guard->get_context_handle())
        steam_context_stale_.store(true, std::memory_order_release);

    int num_channels = resonance::ambisonic_num_channels_for_order(ambisonic_order);
    size_t block_samples = static_cast<size_t>(frame_size_) * static_cast<size_t>(num_channels);

    int32_t samples_read;
    const bool stopping = stop_requested.load(std::memory_order_acquire);
    if (stopping) {
        // Keep mixer alive while input/output rings drain after stop().
        samples_read = frames;
        for (PackedVector2Array& buf : channel_mix_bufs_)
            buf.clear();
    } else {
        samples_read = pull_channel_samples(rate_scale, frames, num_channels);
        if (samples_read == 0)
            return 0;
    }

    if (!is_initialized || steam_context_stale_.load(std::memory_order_acquire)) {
        // No IPL create/teardown on the audio thread; W-channel passthrough until main resolve/prewarm.
        for (int i = 0; i < samples_read; i++) {
            float w = (!channel_mix_bufs_[0].is_empty() && channel_mix_bufs_[0].size() > (unsigned)i)
                          ? channel_mix_bufs_[0][i].x
                          : 0.0f;
            buffer[i].left = w;
            buffer[i].right = w;
        }
        return samples_read;
    }

    if (!stopping)
        push_interleaved_input(samples_read, num_channels);

    while (input_ring.get_available_read() >= block_samples) {
        if (output_ring_l.get_available_write() >= frame_size_) {
            _process_steam_audio_block();
        } else {
            break;
        }
    }

    pull_stereo_output(buffer, samples_read);

    if (stopping && !_has_pending_output()) {
        stop_requested.store(false, std::memory_order_release);
    }

    return samples_read;
}

void ResonanceAmbisonicInternalPlayback::_start(double from_pos) {
    stop_requested.store(false, std::memory_order_release);
    for (size_t i = 0; i < channel_playbacks.size(); i++) {
        if (channel_playbacks[i].is_valid())
            channel_playbacks[i]->start(from_pos);
    }
}
void ResonanceAmbisonicInternalPlayback::_stop() {
    stop_requested.store(true, std::memory_order_release);
    for (size_t i = 0; i < channel_playbacks.size(); i++) {
        if (channel_playbacks[i].is_valid())
            channel_playbacks[i]->stop();
    }
}
bool ResonanceAmbisonicInternalPlayback::_is_playing() const {
    if (stop_requested.load(std::memory_order_acquire) && _has_pending_output())
        return true;
    return !channel_playbacks.empty() && channel_playbacks[0].is_valid() && channel_playbacks[0]->is_playing();
}
int ResonanceAmbisonicInternalPlayback::_get_loop_count() const {
    return (!channel_playbacks.empty() && channel_playbacks[0].is_valid()) ? channel_playbacks[0]->get_loop_count() : 0;
}
double ResonanceAmbisonicInternalPlayback::_get_playback_position() const {
    return (!channel_playbacks.empty() && channel_playbacks[0].is_valid()) ? channel_playbacks[0]->get_playback_position() : 0.0;
}
void ResonanceAmbisonicInternalPlayback::_seek(double position) {
    for (size_t i = 0; i < channel_playbacks.size(); i++) {
        if (channel_playbacks[i].is_valid())
            channel_playbacks[i]->seek(position);
    }
}

void ResonanceAmbisonicInternalStream::sync_channel_streams_size_to_order() {
    const int n = resonance::ambisonic_num_channels_for_order(ambisonic_order);
    const int cur = channel_streams.size();
    if (cur == n)
        return;
    channel_streams.resize(n);
    notify_property_list_changed();
}

void ResonanceAmbisonicInternalStream::_notification(int p_what) {
    if (p_what == Object::NOTIFICATION_POSTINITIALIZE)
        sync_channel_streams_size_to_order();
}

void ResonanceAmbisonicInternalStream::set_channel_streams(const Array& p_streams) {
    channel_streams = p_streams;
    sync_channel_streams_size_to_order();
}

Array ResonanceAmbisonicInternalStream::get_channel_streams() const { return channel_streams; }

void ResonanceAmbisonicInternalStream::set_ambisonic_order(int p_order) {
    ambisonic_order = CLAMP(p_order, 1, 3);
    sync_channel_streams_size_to_order();
}

int ResonanceAmbisonicInternalStream::get_ambisonic_order() const { return ambisonic_order; }

double ResonanceAmbisonicInternalStream::_get_length() const {
    // Length from channel_streams[0]; all channels assumed same duration (typical B-format).
    if (!channel_streams.is_empty()) {
        Variant elem = channel_streams[0];
        Object* obj = (elem.get_type() == Variant::OBJECT) ? static_cast<Object*>(elem) : nullptr;
        AudioStream* as = Object::cast_to<AudioStream>(obj);
        if (as)
            return as->get_length();
    }
    return 0.0;
}

Ref<AudioStreamPlayback> ResonanceAmbisonicInternalStream::_instantiate_playback() const {
    Ref<ResonanceAmbisonicInternalPlayback> playback;
    playback.instantiate();

    const int order = CLAMP(ambisonic_order, 1, 3);
    const int num_channels = resonance::ambisonic_num_channels_for_order(order);
    const int cs_size = channel_streams.size();

    if (cs_size != num_channels) {
        UtilityFunctions::push_warning(
            "Nexus Resonance: ResonanceAmbisonicInternalStream channel_streams size (", cs_size,
            ") does not match ambisonic_order (", order, " requires ", num_channels,
            " channels). Resize the array or change Ambisonic Order in the inspector.");
    }

    Array streams;
    for (int i = 0; i < num_channels; i++) {
        Ref<AudioStreamPlayback> pb;
        if (i < cs_size) {
            Variant elem = channel_streams[i];
            Object* obj = (elem.get_type() == Variant::OBJECT) ? static_cast<Object*>(elem) : nullptr;
            AudioStream* as = Object::cast_to<AudioStream>(obj);
            Ref<AudioStream> s = as ? Ref<AudioStream>(as) : Ref<AudioStream>();
            pb = s.is_valid() ? s->instantiate_playback() : Ref<AudioStreamPlayback>();
        }
        streams.push_back(pb);
    }

    if (streams.is_empty())
        return playback;

    playback->set_channel_playbacks(streams, order);
    playback->prewarm_steam_audio();
    return playback;
}

void ResonanceAmbisonicInternalStream::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_channel_streams", "p_streams"), &ResonanceAmbisonicInternalStream::set_channel_streams);
    ClassDB::bind_method(D_METHOD("get_channel_streams"), &ResonanceAmbisonicInternalStream::get_channel_streams);
    ClassDB::bind_method(D_METHOD("set_ambisonic_order", "p_order"), &ResonanceAmbisonicInternalStream::set_ambisonic_order);
    ClassDB::bind_method(D_METHOD("get_ambisonic_order"), &ResonanceAmbisonicInternalStream::get_ambisonic_order);

    ADD_GROUP("Ambisonics", "");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "ambisonic_order", PROPERTY_HINT_ENUM, "1st Order:1,2nd Order:2,3rd Order:3"),
                 "set_ambisonic_order", "get_ambisonic_order");
    ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "channel_streams", PROPERTY_HINT_ARRAY_TYPE, "AudioStream"), "set_channel_streams",
                 "get_channel_streams");
}

void ResonanceAmbisonicPlayer::_ready() {
    Ref<AudioStream> s = get_stream();
    if (s.is_valid() && !Object::cast_to<ResonanceAmbisonicInternalStream>(s.ptr())) {
        UtilityFunctions::push_warning(
            "Nexus Resonance: ResonanceAmbisonicPlayer expects stream to be a ResonanceAmbisonicInternalStream resource.");
    }
    if (is_autoplay_enabled())
        play();
}

void ResonanceAmbisonicPlayer::_process(double delta) {
    Ref<AudioStreamPlayback> pb = get_stream_playback();
    ResonanceAmbisonicInternalPlayback* res_pb =
        pb.is_valid() ? Object::cast_to<ResonanceAmbisonicInternalPlayback>(pb.ptr()) : nullptr;
    if (res_pb)
        res_pb->resolve_stale_steam_context_on_main();

    if (!is_playing())
        return;

    AmbisonicPlaybackParameters params = build_ambisonic_params_from_player(this);

    if (res_pb)
        res_pb->update_parameters(params);
}

void ResonanceAmbisonicPlayer::set_rotation_enabled(bool p_enabled) {
    rotation_enabled = p_enabled;
}

void ResonanceAmbisonicPlayer::set_use_bed_scene_orientation(bool p_enabled) {
    use_bed_scene_orientation = p_enabled;
}

void ResonanceAmbisonicPlayer::set_ambisonic_orientation_node(const NodePath& p_path) {
    ambisonic_orientation_node = p_path;
}

void ResonanceAmbisonicPlayer::set_apply_hrtf(bool p_enabled) {
    apply_hrtf = p_enabled;
}

void ResonanceAmbisonicPlayer::set_input_is_sn3d(bool p_sn3d) {
    input_is_sn3d = p_sn3d;
}

void ResonanceAmbisonicPlayer::set_apply_output_gain(bool p_enabled) {
    apply_output_gain = p_enabled;
}

void ResonanceAmbisonicPlayer::_validate_property(PropertyInfo& p_property) const {
    const StringName& name = p_property.name;
    // IPL decode is always stereo; sample playback and mix_target routing do not apply.
    if (name == StringName("mix_target") || name == StringName("playback_type")) {
        p_property.usage &= ~PROPERTY_USAGE_EDITOR;
    }
}

void ResonanceAmbisonicPlayer::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_rotation_enabled", "enabled"), &ResonanceAmbisonicPlayer::set_rotation_enabled);
    ClassDB::bind_method(D_METHOD("is_rotation_enabled"), &ResonanceAmbisonicPlayer::is_rotation_enabled);
    ClassDB::bind_method(D_METHOD("set_use_bed_scene_orientation", "enabled"), &ResonanceAmbisonicPlayer::set_use_bed_scene_orientation);
    ClassDB::bind_method(D_METHOD("get_use_bed_scene_orientation"), &ResonanceAmbisonicPlayer::get_use_bed_scene_orientation);
    ClassDB::bind_method(D_METHOD("set_ambisonic_orientation_node", "node_path"), &ResonanceAmbisonicPlayer::set_ambisonic_orientation_node);
    ClassDB::bind_method(D_METHOD("get_ambisonic_orientation_node"), &ResonanceAmbisonicPlayer::get_ambisonic_orientation_node);
    ClassDB::bind_method(D_METHOD("set_apply_hrtf", "enabled"), &ResonanceAmbisonicPlayer::set_apply_hrtf);
    ClassDB::bind_method(D_METHOD("get_apply_hrtf"), &ResonanceAmbisonicPlayer::get_apply_hrtf);
    ClassDB::bind_method(D_METHOD("set_input_is_sn3d", "sn3d"), &ResonanceAmbisonicPlayer::set_input_is_sn3d);
    ClassDB::bind_method(D_METHOD("get_input_is_sn3d"), &ResonanceAmbisonicPlayer::get_input_is_sn3d);
    ClassDB::bind_method(D_METHOD("set_apply_output_gain", "enabled"), &ResonanceAmbisonicPlayer::set_apply_output_gain);
    ClassDB::bind_method(D_METHOD("get_apply_output_gain"), &ResonanceAmbisonicPlayer::get_apply_output_gain);
    ADD_GROUP("Ambisonic decode", "");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "rotation_enabled"), "set_rotation_enabled", "is_rotation_enabled");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_bed_scene_orientation"), "set_use_bed_scene_orientation", "get_use_bed_scene_orientation");
    ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "ambisonic_orientation_node", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"),
                 "set_ambisonic_orientation_node", "get_ambisonic_orientation_node");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "apply_hrtf"), "set_apply_hrtf", "get_apply_hrtf");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "input_is_sn3d"), "set_input_is_sn3d", "get_input_is_sn3d");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "apply_output_gain"), "set_apply_output_gain", "get_apply_output_gain");
}