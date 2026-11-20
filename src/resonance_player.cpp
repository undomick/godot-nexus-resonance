#include "resonance_player.h"
#include "resonance_constants.h"
#include "resonance_listener.h"
#include "resonance_listener_attenuation_policy.h"
#include "resonance_log.h"
#include "resonance_math.h"
#include "resonance_playback_lod_policy.h"
#include "resonance_player_debug_hud_policy.h"
#include "resonance_polyphonic_voice_policy.h"
#include "resonance_probe_volume.h"
#include "resonance_reflection_fetch_policy.h"
#include "resonance_reverb_send_policy.h"
#include "resonance_server.h"
#include "resonance_soft_stop_watchdog_policy.h"
#include "resonance_source_handle_policy.h"
#include "resonance_tail_drain_policy.h"
#include "resonance_utils.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/collision_object3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector4.hpp>
#include <limits>
#include <sstream>
#include <thread>
#include <utility>

namespace {
thread_local int tls_convolution_apply_depth = 0;
} // namespace

using namespace godot;

namespace {
static void collect_collision_object_rids_recursive(Node* node, std::vector<RID>& out) {
    if (!node)
        return;
    auto* co = Object::cast_to<CollisionObject3D>(node);
    if (co)
        out.push_back(co->get_rid());
    const int nch = node->get_child_count();
    for (int i = 0; i < nch; ++i)
        collect_collision_object_rids_recursive(node->get_child(i), out);
}
} // namespace

ResonanceStreamPlayback::ResonanceStreamPlayback() {
    params_current.apply_air_absorption = false;
    params_current.air_absorption[0] = 1.0f;
    params_current.air_absorption[1] = 1.0f;
    params_current.air_absorption[2] = 1.0f;
    params_current.apply_directivity = false;
    params_current.directivity_value = 1.0f;
    params_current.occlusion = 0.0f;
    params_current.transmission[0] = 1.0f;
    params_current.transmission[1] = 1.0f;
    params_current.transmission[2] = 1.0f;
    params_current.attenuation = 1.0f;
    params_current.listener_orientation.ahead = {0, 0, -1};
    params_current.listener_orientation.up = {0, 1, 0};
    params_current.listener_orientation.right = {1, 0, 0};
    params_current.listener_orientation.origin = {0, 0, 0};

    input_ring_l.resize(resonance::kRingBufferCapacity);
    input_ring_r.resize(resonance::kRingBufferCapacity);
    output_ring_l.resize(resonance::kRingBufferCapacity);
    output_ring_r.resize(resonance::kRingBufferCapacity);
    output_ring_reverb_l.resize(resonance::kRingBufferCapacity);
    output_ring_reverb_r.resize(resonance::kRingBufferCapacity);

    // _lazy_init resizes these to the server's frame size.
    temp_process_buffer_l.resize(resonance::kGodotDefaultFrameSize);
    temp_process_buffer_r.resize(resonance::kGodotDefaultFrameSize);

    temp_reverb_buffer_l.resize(resonance::kMaxAudioFrameSize);
    temp_reverb_buffer_r.resize(resonance::kMaxAudioFrameSize);

    memset(&sa_in_buffer, 0, sizeof(IPLAudioBuffer));
    memset(&sa_direct_out_buffer, 0, sizeof(IPLAudioBuffer));
    memset(&sa_final_mix_buffer, 0, sizeof(IPLAudioBuffer));

    parametric_path_sh_coeffs[0] = resonance::kAmbisonicWChannelScale;
    parametric_path_sh_coeffs[1] = parametric_path_sh_coeffs[2] = parametric_path_sh_coeffs[3] = 0.0f;
}

ResonanceStreamPlayback::~ResonanceStreamPlayback() {
    convolution_blocked_.store(true, std::memory_order_release);
    const int32_t handle = voice_source_handle_.exchange(-1, std::memory_order_acq_rel);
    const uint32_t epoch = voice_source_epoch_.exchange(0, std::memory_order_acq_rel);
    const auto owner_lease = owner_link_.lease();
    ResonancePlayer* owner = owner_lease.get();
    if (owner)
        owner->internal_unregister_playback(this);
    // The applying thread must not wait on itself. Main waits until that Apply returns.
    if (tls_convolution_apply_depth == 0) {
        while (convolution_apply_depth_.load(std::memory_order_acquire) > 0)
            std::this_thread::yield();
    }
    _cleanup_steam_audio();
    if (owner && handle >= 0)
        owner->internal_reclaim_voice_source(handle, epoch);
}

bool ResonanceStreamPlayback::convolution_try_enter() {
    if (convolution_blocked_.load(std::memory_order_acquire))
        return false;
    convolution_apply_depth_.fetch_add(1, std::memory_order_acq_rel);
    if (convolution_blocked_.load(std::memory_order_acquire)) {
        convolution_apply_depth_.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }
    tls_convolution_apply_depth += 1;
    return true;
}

void ResonanceStreamPlayback::convolution_leave() {
    if (tls_convolution_apply_depth > 0)
        tls_convolution_apply_depth -= 1;
    convolution_apply_depth_.fetch_sub(1, std::memory_order_acq_rel);
}

void ResonanceStreamPlayback::block_convolution_for_source_release() {
    convolution_blocked_.store(true, std::memory_order_release);
}

void ResonanceStreamPlayback::ipl_context_reinit_cleanup(void* userdata) {
    if (!userdata)
        return;
    static_cast<ResonanceStreamPlayback*>(userdata)->_cleanup_steam_audio();
}

void ResonanceStreamPlayback::set_base_playback(const Ref<AudioStreamPlayback>& p_playback) {
    base_playback = p_playback;
}

void ResonanceStreamPlayback::_release_retained_source() {
    IPLSource src = reinterpret_cast<IPLSource>(retained_ipl_source_.exchange(0, std::memory_order_acq_rel));
    retained_source_handle_.store(-1, std::memory_order_release);
    if (src)
        iplSourceRelease(&src);
}

void ResonanceStreamPlayback::_retain_source_for_main(int32_t handle) {
    if (handle == retained_source_handle_.load(std::memory_order_acquire))
        return;
    _release_retained_source();
    if (handle < 0)
        return;
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!srv || !srv->is_initialized())
        return;
    IPLSource src = srv->get_source_from_handle(handle);
    if (!src)
        return;
    retained_ipl_source_.store(reinterpret_cast<uintptr_t>(src), std::memory_order_release);
    retained_source_handle_.store(handle, std::memory_order_release);
}

void ResonanceStreamPlayback::update_parameters(const PlaybackParameters& p_params) {
    _retain_source_for_main(p_params.source_handle);
    params_slot_.publish(p_params);
    // Opens the _mix gate. Before this, params_current holds defaults with no position or attenuation.
    params_ever_synced_.store(true, std::memory_order_release);
}

void ResonanceStreamPlayback::_start(double from_pos) {
    const auto owner_lease = owner_link_.lease();
    ResonancePlayer* owner = owner_lease.get();
    input_started = false;
    // Closes the params gate again so a restart cannot burst at full volume with stale parameters.
    params_ever_synced_.store(false, std::memory_order_release);
    stop_requested_.store(false, std::memory_order_release);
    tail_grace_blocks_remaining_.store(-1, std::memory_order_release);
    tail_drain_complete_.store(false, std::memory_order_release);
    base_short_read_.store(false, std::memory_order_release);
    voice_was_virtual_ = false;
    mix_timing_started_ = false;
    mix_lateness_.reset();
    _reset_voice_dsp_state();
    if (base_playback.is_valid()) {
        base_playback->start(from_pos);
    }
    started_.store(true, std::memory_order_release);
    if (owner)
        owner->internal_register_playback(this);
    playback_host_fade_out_remaining_ = 0;
    // play() often pushed parameters before this _start, and _start just closed the gate again.
    // Push once more so the gate opens on the next main-thread flush, not on the next _process.
    if (owner)
        owner->call_deferred("_deferred_push_playback_parameters");
}

void ResonanceStreamPlayback::_reset_voice_dsp_state() {
    prev_direct_weight = 0.0f;
    prev_conv_reflections_mix_level_ = 0.0f;
    prev_parametric_reflections_mix_level_ = 0.0f;
    prev_pathing_mix_level_ = 0.0f;
    // Shared reflection stays armed. Resetting it would cut the previous voice's tail.
    path_processor.reset_effect();
    pathing_tail_have_params_ = false;
    pathing_tail_param_epoch_ = 0;
    memset(&pathing_tail_params_, 0, sizeof(pathing_tail_params_));
    reverb_ring_prev_l_ = 0.0f;
    reverb_ring_prev_r_ = 0.0f;
    reverb_ring_prev_valid_ = false;
    reverb_ring_gap_fade_armed_ = false;
    reverb_ring_gap_fade_index_ = 0;
    reverb_ring_gap_fade_total_ = 0;
    last_mix_out_l_ = 0.0f;
    last_mix_out_r_ = 0.0f;
    last_mix_out_valid_ = false;
    input_ring_tail_l_ = 0.0f;
    input_ring_tail_r_ = 0.0f;
    input_ring_tail_valid_ = false;
    prev_mix_had_partial_input_pad_ = false;
    prev_mix_had_eos_tapered_input_pad_ = false;
    playback_host_fade_in_total_samples_ =
        resonance::host_fade_samples_from_ms(resonance::kPlaybackHostFadeInMs, current_sample_rate);
    playback_host_fade_out_total_samples_ =
        resonance::host_fade_samples_from_ms(resonance::kPlaybackHostFadeOutMs, current_sample_rate);
    playback_host_fade_in_elapsed_ = 0;
    playback_host_fade_out_remaining_ = 0;
    direct_processor.reset_for_new_playback();
    input_ring_l.clear();
    input_ring_r.clear();
    output_ring_l.clear();
    output_ring_r.clear();
    output_ring_reverb_l.clear();
    output_ring_reverb_r.clear();
}

void ResonanceStreamPlayback::_stop() {
    // Halts only the dry input; the reverb and path tails keep decaying. Resetting the processors
    // here would cut the tail.
    if (base_playback.is_valid())
        base_playback->stop();
    stop_requested_.store(true, std::memory_order_release);
    if (resonance::kPlaybackHostFadeOutEnabled && playback_host_fade_out_remaining_ <= 0) {
        playback_host_fade_out_total_samples_ =
            resonance::host_fade_samples_from_ms(resonance::kPlaybackHostFadeOutMs, current_sample_rate);
        playback_host_fade_out_remaining_ = playback_host_fade_out_total_samples_;
    }
}

void ResonanceStreamPlayback::request_soft_stop() {
    if (base_playback.is_valid())
        base_playback->stop();
    stop_requested_.store(true, std::memory_order_release);
    if (resonance::kPlaybackHostFadeOutEnabled && playback_host_fade_out_remaining_ <= 0) {
        playback_host_fade_out_total_samples_ =
            resonance::host_fade_samples_from_ms(resonance::kPlaybackHostFadeOutMs, current_sample_rate);
        playback_host_fade_out_remaining_ = playback_host_fade_out_total_samples_;
    }
}

bool ResonanceStreamPlayback::has_active_tail_residue() const {
    if (output_ring_l.get_available_read() > 0)
        return true;
    if (output_ring_reverb_l.get_available_read() > 0)
        return true;
    if (direct_processor.get_tail_size_samples() > 0)
        return true;
    if (path_processor.get_tail_size_samples() > 0)
        return true;
    return false;
}

bool ResonanceStreamPlayback::_is_playing() const {
    // Never finish on our own after the tail drain; the player stops the node.
    if (tail_drain_complete_.load(std::memory_order_acquire))
        return true;
    if (is_base_playing())
        return true;
    if (!is_initialized)
        return false;
    if (has_active_tail_residue())
        return true;
    const int64_t grace = tail_grace_blocks_remaining_.load(std::memory_order_acquire);
    if (grace == 0)
        return true;
    // Grace is armed only by the first mix without input, which Godot may not have run yet.
    if (grace < 0)
        return true;
    // Only _mix counts grace down; reporting false here would stop the mix calls and cut the tail hard.
    return true;
}

bool ResonanceStreamPlayback::is_base_playing() const {
    if (base_short_read_.load(std::memory_order_acquire))
        return false;
    return base_playback.is_valid() && base_playback->is_playing();
}

int ResonanceStreamPlayback::_get_loop_count() const {
    return base_playback.is_valid() ? base_playback->get_loop_count() : 0;
}
void ResonanceStreamPlayback::_seek(double position) {
    if (base_playback.is_valid())
        base_playback->seek(position);
}

void ResonanceStream::set_base_stream(const Ref<AudioStream>& p_stream) { base_stream = p_stream; }
Ref<AudioStreamPlayback> ResonanceStream::_instantiate_playback() const {
    // Animation audio tracks use AudioStreamPolyphonic. Wrapping it in ResonanceStreamPlayback crashes on
    // Windows before playback starts, so it plays natively without Steam Audio.
    if (base_stream.is_valid() && base_stream->is_class("AudioStreamPolyphonic")) {
        return base_stream->instantiate_playback();
    }
    Ref<ResonanceStreamPlayback> playback;
    playback.instantiate();
    playback->set_owner_player(stream_owner_);
    if (base_stream.is_valid()) {
        playback->set_base_playback(base_stream->instantiate_playback());
    }
    // Prewarm here so a new polyphonic voice does not allocate IPL effects on the audio thread.
    playback->prewarm_steam_audio();
    return playback;
}

void ResonanceReverbPlayback::set_parent_link(std::shared_ptr<ReverbParentLink> p_link) {
    parent_link_ = std::move(p_link);
}

int32_t ResonanceReverbPlayback::_mix(AudioFrame* buffer, float _rate_scale, int32_t frames) {
    if (!parent_link_ || frames <= 0 || !buffer)
        return 0;
    const auto lease = parent_link_->lease();
    const ResonancePlayer* parent = lease.get();
    if (!parent)
        return 0;
    ResonancePlayer::PlaybackVoiceSnapshot snap;
    parent->internal_get_playback_snapshot_for_audio(snap);
    // The parent's voices register on its next physics tick, after this child has started. Mixing
    // fewer frames than asked makes the AudioServer delete this playback, so play silence until then.
    size_t total_reverb_samples = 0;
    for (int vi = 0; vi < snap.count; vi++) {
        ResonanceStreamPlayback* pb = snap.voices[static_cast<size_t>(vi)];
        if (pb)
            total_reverb_samples += pb->get_reverb_ring_available_read();
    }
    if (!resonance::reverb_send_child_should_play(parent->internal_is_playing_for_audio(), total_reverb_samples > 0))
        return 0;
    for (int32_t i = 0; i < frames; i++) {
        buffer[i].left = 0.0f;
        buffer[i].right = 0.0f;
    }
    thread_local AudioFrame tmp_voice_reverb[resonance::kMaxAudioFrameSize];
    const int32_t mix_frames = (frames > resonance::kMaxAudioFrameSize) ? resonance::kMaxAudioFrameSize : frames;
    for (int vi = 0; vi < snap.count; vi++) {
        ResonanceStreamPlayback* pb = snap.voices[static_cast<size_t>(vi)];
        if (!pb)
            continue;
        pb->read_reverb_frames(tmp_voice_reverb, mix_frames);
        for (int32_t i = 0; i < mix_frames; i++) {
            buffer[i].left += tmp_voice_reverb[i].left;
            buffer[i].right += tmp_voice_reverb[i].right;
        }
    }
    for (int32_t i = 0; i < frames; i++) {
        buffer[i].left = std::clamp(buffer[i].left, -1.0f, 1.0f);
        buffer[i].right = std::clamp(buffer[i].right, -1.0f, 1.0f);
    }
    return frames;
}

void ResonanceReverbPlayback::_start(double _from_pos) {}
void ResonanceReverbPlayback::_stop() {}
bool ResonanceReverbPlayback::_is_playing() const {
    if (!parent_link_)
        return false;
    const auto lease = parent_link_->lease();
    const ResonancePlayer* parent = lease.get();
    if (!parent)
        return false;
    return resonance::reverb_send_child_should_play(parent->internal_is_playing_for_audio(),
                                                    parent->any_internal_playback_has_reverb_ring_data());
}
int ResonanceReverbPlayback::_get_loop_count() const { return 0; }
void ResonanceReverbPlayback::_seek(double _position) {}

void ResonanceReverbStream::set_parent_link(std::shared_ptr<ReverbParentLink> p_link) {
    parent_link_ = std::move(p_link);
}

Ref<AudioStreamPlayback> ResonanceReverbStream::_instantiate_playback() const {
    Ref<ResonanceReverbPlayback> pb;
    pb.instantiate();
    pb->set_parent_link(parent_link_);
    return pb;
}

void ResonancePlayer::_enter_tree() {
    reverb_parent_link_->set(this);
    set_process_priority(resonance::kResonancePlayerProcessPriority);
    set_physics_process_priority(resonance::kResonancePlayerProcessPriority);
    call_deferred("_apply_process_mode_for_tracer");
}

void ResonancePlayer::_apply_process_mode_for_tracer() {
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint()) {
        set_physics_process(false);
        player_sync_uses_physics_ = false;
        return;
    }
    ResonanceServer* server = ResonanceServer::get_singleton();
    const bool custom = server && server->is_initialized() && server->uses_custom_ray_tracer();
    player_sync_uses_physics_ = custom;
    set_physics_process(custom);
}

void ResonancePlayer::_ready() {
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint())
        return;
    if (!player_config.is_valid()) {
        // No config: behave as plain AudioStreamPlayer3D
        set_process(false);
        if (is_autoplay_enabled())
            play();
        return;
    }
    set_process(true);
    add_to_group("resonance_player");
    debug_drawer.initialize(this);
    directivity_drawer_.initialize(this);
    directivity_drawer_.mark_dirty();
    _apply_steam_mode_asp3d_guards();
    _refresh_effective_volume_cache();
    _ensure_source_exists();
    _update_stream_setup();
    if (is_autoplay_enabled())
        play_stream();
}

void ResonancePlayer::_apply_steam_mode_asp3d_guards() {
    // Godot still cuts off at max_distance > 0 even with ATTENUATION_DISABLED. Clear both so the node's own
    // distance settings cannot shape the mix a second time.
    set_attenuation_model(AttenuationModel::ATTENUATION_DISABLED);
    set_max_distance(0.0f);
}

void ResonancePlayer::_publish_playing_for_audio() {
    playing_for_audio_.store(is_playing(), std::memory_order_release);
}

void ResonancePlayer::_refresh_effective_volume_cache() {
    const float lin = resonance::effective_asp3d_volume_linear(get_volume_db(), get_max_db());
    cached_effective_volume_linear_.store(lin, std::memory_order_relaxed);
}

void ResonancePlayer::_exit_tree() {
    _clear_physics_ray_auto_exclude_rids();
    // Hard stop on purpose: a soft stop would race node destruction, so the wet tail is cut here.
    warned_source_handle_create_failed_ = false;
    playback_lod_have_anchor_ = false;
    playback_lod_time_since_full_ = 0.0;
    last_pushed_playback_params_valid_ = false;
    coeff_smooth_initialized_ = false;
    coeff_smooth_source_handle_ = -1;
    Node* reverb_child = get_node_or_null(NodePath("ResonanceReverbOutput"));
    if (reverb_child && reverb_child->is_class("AudioStreamPlayer")) {
        if (AudioStreamPlayer* rp = Object::cast_to<AudioStreamPlayer>(reverb_child))
            rp->stop();
    }
    AudioStreamPlayer3D::stop();
    _publish_playing_for_audio();
    reverb_parent_link_->orphan();
    debug_drawer.cleanup();
    directivity_drawer_.cleanup();
    _destroy_owned_sources_for_shutdown();
}

ResonancePlayer::~ResonancePlayer() {
    reverb_parent_link_->orphan();
    std::vector<ResonanceStreamPlayback*> copy;
    {
        std::lock_guard<std::mutex> lock(internal_playbacks_mutex_);
        copy.swap(internal_playbacks_);
    }
    for (ResonanceStreamPlayback* p : copy) {
        if (p)
            p->internal_orphan_owner_player();
    }
    cleanup_shared_reflection();
}

void ResonancePlayer::internal_publish_playback_snapshot() {
    const int back = 1 - playback_snap_front_.load(std::memory_order_relaxed);
    PlaybackVoiceSnapshot& snap = playback_snap_[static_cast<size_t>(back)];
    snap.count = 0;
    {
        std::lock_guard<std::mutex> lock(internal_playbacks_mutex_);
        for (ResonanceStreamPlayback* p : internal_playbacks_) {
            if (!p || snap.count >= resonance::kMaxPlayerPolyphonySnapshot)
                continue;
            snap.voices[static_cast<size_t>(snap.count++)] = p;
        }
    }
    playback_snap_front_.store(back, std::memory_order_release);
}

void ResonancePlayer::internal_get_playback_snapshot_for_audio(PlaybackVoiceSnapshot& out) const {
    const int front = playback_snap_front_.load(std::memory_order_acquire);
    out = playback_snap_[static_cast<size_t>(front)];
}

void ResonancePlayer::internal_register_playback(ResonanceStreamPlayback* p) {
    if (!p)
        return;
    {
        std::lock_guard<std::mutex> lock(internal_playbacks_mutex_);
        for (ResonanceStreamPlayback* q : internal_playbacks_) {
            if (q == p)
                return;
        }
        internal_playbacks_.push_back(p);
        playback_count_.fetch_add(1, std::memory_order_release);
    }
    internal_publish_playback_snapshot();
}

void ResonancePlayer::internal_unregister_playback(ResonanceStreamPlayback* p) {
    if (!p)
        return;
    {
        std::lock_guard<std::mutex> lock(internal_playbacks_mutex_);
        const size_t before = internal_playbacks_.size();
        internal_playbacks_.erase(std::remove(internal_playbacks_.begin(), internal_playbacks_.end(), p), internal_playbacks_.end());
        if (internal_playbacks_.size() < before)
            playback_count_.fetch_sub(1, std::memory_order_release);
    }
    internal_publish_playback_snapshot();
}

void ResonancePlayer::internal_reclaim_voice_source(int32_t handle, uint32_t epoch) {
    std::lock_guard<std::mutex> lock(voice_source_reclaim_mutex_);
    PendingVoiceSourceReclaim item;
    item.handle = handle;
    item.epoch = epoch;
    voice_source_reclaims_.push_back(item);
}

void ResonancePlayer::internal_copy_internal_playbacks(std::vector<ResonanceStreamPlayback*>& out) const {
    std::lock_guard<std::mutex> lock(internal_playbacks_mutex_);
    out = internal_playbacks_;
}

bool ResonancePlayer::any_internal_playback_has_reverb_ring_data() const {
    PlaybackVoiceSnapshot snap;
    internal_get_playback_snapshot_for_audio(snap);
    for (int i = 0; i < snap.count; i++) {
        ResonanceStreamPlayback* pb = snap.voices[static_cast<size_t>(i)];
        if (pb && pb->get_reverb_ring_available_read() > 0)
            return true;
    }
    return false;
}

void ResonancePlayer::_broadcast_update_parameters(const PlaybackParameters& p) {
    std::vector<ResonanceStreamPlayback*> copy;
    {
        std::lock_guard<std::mutex> lock(internal_playbacks_mutex_);
        copy = internal_playbacks_;
    }
    ResonanceServer* srv = ResonanceServer::get_singleton();
    for (ResonanceStreamPlayback* pb : copy) {
        if (!pb)
            continue;
        PlaybackParameters voice = p;
        if (p.source_handle >= 0) {
            const int32_t owned = pb->get_voice_source_handle();
            // A voice with no bound handle must not Apply the player template source.
            if (owned >= 0) {
                voice.source_handle = owned;
                if (srv && srv->is_initialized()) {
                    IPLReflectionEffectParams params{};
                    voice.has_valid_reverb = srv->peek_reverb_params_likely_available(owned) || srv->fetch_reverb_params(owned, params);
                }
            } else {
                voice.source_handle = -1;
                voice.has_valid_reverb = false;
            }
        }
        pb->update_parameters(voice);
    }
}

void ResonancePlayer::_detach_playback_source_retains() {
    PlaybackParameters p;
    p.source_handle = -1;
    _broadcast_update_parameters(p);
    if (ResonanceStreamPlayback* pb = _get_resonance_playback()) {
        bool listed = false;
        {
            std::lock_guard<std::mutex> lock(internal_playbacks_mutex_);
            for (ResonanceStreamPlayback* q : internal_playbacks_) {
                if (q == pb) {
                    listed = true;
                    break;
                }
            }
        }
        if (!listed)
            pb->update_parameters(p);
    }
}

void ResonancePlayer::_aggregate_debug_signal_levels(float& out_direct, float& out_reverb, float& out_pathing) {
    out_direct = 0.0f;
    out_reverb = 0.0f;
    out_pathing = 0.0f;
    std::vector<ResonanceStreamPlayback*> copy;
    internal_copy_internal_playbacks(copy);
    for (ResonanceStreamPlayback* pb : copy) {
        if (!pb)
            continue;
        float d = 0.0f, r = 0.0f, p = 0.0f;
        pb->get_debug_signal_levels(d, r, p);
        out_direct = std::max(out_direct, d);
        out_reverb = std::max(out_reverb, r);
        out_pathing = std::max(out_pathing, p);
    }
    if (copy.empty()) {
        if (ResonanceStreamPlayback* pb = _get_resonance_playback())
            pb->get_debug_signal_levels(out_direct, out_reverb, out_pathing);
    }
}

bool ResonancePlayer::_want_player_debug_ui(const ResonanceServer* srv) const {
    if (exclude_from_debug_ || !srv)
        return false;
    return srv->is_debug_occlusion_enabled() || srv->is_debug_reflections_enabled();
}

void ResonancePlayer::_tick_debug_overlay_grace_timer(double delta, ResonanceServer* srv) {
    if (!_want_player_debug_ui(srv)) {
        debug_overlay_grace_timer_ = 0.0;
        return;
    }
    const bool pipeline_ok = is_playing() && srv && srv->is_simulating() && source_handle >= 0;
    if (pipeline_ok)
        debug_overlay_grace_timer_ = resonance::kDebugOverlayGraceSeconds;
    else
        debug_overlay_grace_timer_ -= delta;
}

bool ResonancePlayer::_compute_show_debug_hud(const ResonanceServer* srv) const {
    if (!_want_player_debug_ui(srv))
        return false;
    const bool pipeline_ok = is_playing() && srv && srv->is_simulating() && source_handle >= 0;
    return pipeline_ok || debug_overlay_grace_timer_ > 0.0;
}

void ResonancePlayer::_apply_debug_hud_overlay_hints(ResonanceDebugData& dbg_data, bool sim_hold) {
    dbg_data.sim_snapshot_hold = sim_hold;
}

void ResonancePlayer::_enrich_debug_overlay_signal_levels(ResonanceDebugData& dbg_data, ResonanceServer* srv, bool reflections_hud_active,
                                                          bool grace_refresh_meters) {
    _aggregate_debug_signal_levels(dbg_data.signal_direct, dbg_data.signal_reverb, dbg_data.signal_pathing);
    dbg_data.signal_reverb_from_shared_bus = false;
    if (!srv)
        return;
    const bool use_bus_rms = resonance::player_debug_hud_use_shared_reverb_bus_rms(srv->is_debug_reflections_enabled(), srv->get_reflection_type());
    const bool bus_for_hud = use_bus_rms && (reflections_hud_active || grace_refresh_meters);
    if (!bus_for_hud)
        return;
    const float bus_rms_pre_gain = srv->get_reverb_bus_output_rms_pre_gain();
    dbg_data.signal_reverb = std::clamp(resonance::sanitize_audio_float(bus_rms_pre_gain), 0.0f, 1.0f);
    dbg_data.signal_reverb_from_shared_bus = true;
}

void ResonancePlayer::_present_player_debug_overlay_from_cache(double delta, ResonanceServer* srv, bool show_debug_hud) {
    if (!show_debug_hud) {
        _sync_player_debug_drawer(delta, srv, ResonanceDebugData{}, false);
        return;
    }
    if (!debug_overlay_has_last_data_) {
        _sync_player_debug_drawer(delta, srv, ResonanceDebugData{}, false);
        return;
    }
    ResonanceDebugData dbg = debug_overlay_last_data_;
    const bool reflections_hud = srv && srv->is_debug_reflections_enabled();
    const bool pipeline_ok = is_playing() && srv && srv->is_simulating() && source_handle >= 0;
    const bool grace_refresh = debug_overlay_grace_timer_ > 0.0 && !pipeline_ok;
    _apply_debug_hud_overlay_hints(dbg, !pipeline_ok || dbg.sim_snapshot_hold);
    _enrich_debug_overlay_signal_levels(dbg, srv, reflections_hud, grace_refresh);
    _sync_player_debug_drawer(delta, srv, dbg, true);
}

void ResonancePlayer::_notification(int p_what) {
    if (p_what == NOTIFICATION_ENTER_TREE || p_what == NOTIFICATION_CHILD_ORDER_CHANGED) {
        Engine* eng = Engine::get_singleton();
        if (!eng || !eng->is_editor_hint())
            _sync_physics_ray_auto_exclude_rids();
    }
    if (p_what == NOTIFICATION_ENTER_TREE) {
        if (player_config.is_valid())
            _apply_player_config_bus_routing();
        Engine* eng = Engine::get_singleton();
        if (eng && !eng->is_editor_hint() && convert_anim_audio_runtime_ && player_config.is_valid())
            call_deferred("_nexus_deferred_spawn_anim_audio_helper");
    }
}

void ResonancePlayer::_clear_physics_ray_auto_exclude_rids() {
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (srv && !registered_physics_auto_exclude_rids_.empty()) {
        for (const RID& r : registered_physics_auto_exclude_rids_)
            srv->unregister_physics_ray_auto_exclude_rid(r);
    }
    registered_physics_auto_exclude_rids_.clear();
}

void ResonancePlayer::_sync_physics_ray_auto_exclude_rids() {
    if (!auto_exclude_colliders_)
        return;
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!srv || !srv->uses_custom_ray_tracer())
        return;
    _clear_physics_ray_auto_exclude_rids();
    std::vector<RID> found;
    collect_collision_object_rids_recursive(this, found);
    for (const RID& r : found) {
        srv->register_physics_ray_auto_exclude_rid(r);
        registered_physics_auto_exclude_rids_.push_back(r);
    }
}

void ResonancePlayer::_ensure_listener_pose_synced_for_playback(ResonanceServer* srv) {
    if (!resonance::playback_should_sync_viewport_listener_before_attenuation(srv && srv->is_initialized()))
        return;
    Viewport* vp = get_viewport();
    if (!vp)
        return;
    SceneTree* tree = get_tree();
    if (!tree)
        return;
    TypedArray<Node> listeners = tree->get_nodes_in_group(StringName("resonance_listener"));
    ResonanceListener::sync_viewport_listeners_to_server(vp, listeners);
}

void ResonancePlayer::_sync_playback_simulation_frame(double delta, ResonanceServer* srv, bool show_debug_hud) {
    if (!srv || !srv->is_simulating() || source_handle < 0)
        return;

    _ensure_listener_pose_synced_for_playback(srv);
    _ensure_config_valid();
    if (_reverb_send_wanted(srv) != reverb_send_active_)
        _sync_reverb_send_child(srv);
    const bool coeff_smooth_active = (config_cache_.playback_coeff_smoothing_time > 0.0f) &&
                                     ((config_cache_.occlusion_input == 0) || (config_cache_.transmission_input == 0));
    const bool lod_gate_open = _playback_lod_should_apply_playback_params(delta, show_debug_hud, get_global_position());
    const bool virtual_state_changed = _update_voice_virtual_state(srv, delta);
    const bool apply_playback =
        resonance::playback_should_apply_full_params(lod_gate_open, coeff_smooth_active, virtual_state_changed);

    ResonanceDebugData dbg_data;
    if (apply_playback)
        _apply_playback_params_from_simulation(srv, &dbg_data, delta);
    else if (resonance::playback_lod_should_push_coeff_refresh(false))
        _apply_playback_coeff_refresh_from_simulation(srv, delta);
    else
        dbg_data = debug_overlay_last_data_;

    const bool reflections_hud = show_debug_hud && srv && srv->is_debug_reflections_enabled();
    const bool pipeline_ok = is_playing() && srv && srv->is_simulating() && source_handle >= 0;
    const bool grace_refresh = show_debug_hud && debug_overlay_grace_timer_ > 0.0 && !pipeline_ok;
    _apply_debug_hud_overlay_hints(dbg_data, !apply_playback);
    _enrich_debug_overlay_signal_levels(dbg_data, srv, reflections_hud, grace_refresh);

    if (apply_playback) {
        debug_overlay_last_data_ = dbg_data;
        debug_overlay_has_last_data_ = true;
    }
    _sync_player_debug_drawer(delta, srv, dbg_data, show_debug_hud);
}

void ResonancePlayer::_physics_process(double delta) {
    if (!player_sync_uses_physics_)
        return;
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint())
        return;
    if (!player_config.is_valid() || !is_playing())
        return;
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!srv || !srv->is_simulating() || source_handle < 0)
        return;
    _prepare_source_for_simulation(srv);
    const bool show_debug_hud = _compute_show_debug_hud(srv);
    _sync_playback_simulation_frame(delta, srv, show_debug_hud);
}

void ResonancePlayer::_process(double delta) {
    _publish_playing_for_audio();
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint())
        return;
    if (!player_config.is_valid())
        return;
    _drain_voice_source_reclaims();

    {
        std::vector<ResonanceStreamPlayback*> resolve_voices;
        internal_copy_internal_playbacks(resolve_voices);
        if (resolve_voices.empty()) {
            if (ResonanceStreamPlayback* pb = _get_resonance_playback())
                resolve_voices.push_back(pb);
        }
        for (ResonanceStreamPlayback* pb : resolve_voices) {
            if (pb)
                pb->resolve_stale_steam_context_on_main();
        }
    }

    {
        ResonanceDirectivityDrawer::Params dp;
        dp.enabled = _config_bool("directivity_enabled", false);
        dp.input_mode = _config_int("directivity_input", 0);
        dp.weight = _config_float("directivity_weight", 0.0f);
        dp.power = _config_float("directivity_power", 1.0f);
        dp.user_value = _config_float("directivity_value", 1.0f);
        dp.size = 1.0f;
        directivity_drawer_.process(dp, show_directivity_gizmo_);
    }

    _refresh_effective_volume_cache();

    ResonanceServer* srv = ResonanceServer::get_singleton();
    // `finished` fires once at dry EOS. The node is stopped explicitly after the tail drain so Godot
    // does not emit `finished` a second time.
    if (is_playing()) {
        std::vector<ResonanceStreamPlayback*> voices;
        internal_copy_internal_playbacks(voices);
        // A replay's playback starts and registers only on the next physics tick; until then it is the
        // newest stream playback and must count as a run that has not started yet.
        if (ResonanceStreamPlayback* newest = _get_resonance_playback()) {
            if (std::find(voices.begin(), voices.end(), newest) == voices.end())
                voices.push_back(newest);
        }

        bool any_dry_playing = false;
        bool any_soft_stopped = false;
        bool all_tail_drained = !voices.empty();
        for (ResonanceStreamPlayback* pb : voices) {
            if (!pb) {
                all_tail_drained = false;
                continue;
            }
            if (!pb->has_started() || pb->is_base_playing())
                any_dry_playing = true;
            if (pb->stop_requested_.load(std::memory_order_acquire))
                any_soft_stopped = true;
            if (!pb->is_tail_drain_complete())
                all_tail_drained = false;
        }

        const bool dry_done_natural = !voices.empty() && !any_dry_playing && !any_soft_stopped;
        if (dry_done_natural && !dry_finished_emitted_) {
            dry_finished_emitted_ = true;
            // Deferred because a `finished` handler may call play() while this tick is still running.
            if (!dry_finished_deferred_queued_) {
                dry_finished_deferred_queued_ = true;
                dry_finished_deferred_serial_ = play_serial_;
                call_deferred("_nexus_deferred_emit_finished");
            }
        }

        // After a soft stop no `finished` is emitted; stop once every voice has drained its tail.
        if (all_tail_drained && (dry_finished_emitted_ || any_soft_stopped)) {
            soft_stop_elapsed_sec_ = -1.0;
            soft_stop_mix_stall_sec_ = 0.0;
            soft_stop_last_mix_calls_ = 0;
            AudioStreamPlayer3D::stop();
            return;
        }
        // Force-stop only when _mix stalls (Dummy driver) or the absolute cap is hit; an active tail drain
        // may run longer than the reverb duration.
        if (any_soft_stopped && soft_stop_elapsed_sec_ >= 0.0) {
            soft_stop_elapsed_sec_ += delta;
            uint64_t total_mix_calls = 0;
            for (ResonanceStreamPlayback* pb : voices) {
                if (pb)
                    total_mix_calls += pb->get_mix_call_count();
            }
            if (total_mix_calls == soft_stop_last_mix_calls_)
                soft_stop_mix_stall_sec_ += delta;
            else {
                soft_stop_mix_stall_sec_ = 0.0;
                soft_stop_last_mix_calls_ = total_mix_calls;
            }
            float max_rev = resonance::kDefaultReverbDurationSec;
            if (srv)
                max_rev = srv->get_realtime_simulation_duration();
            if (resonance::soft_stop_watchdog_should_force_stop(max_rev, soft_stop_elapsed_sec_, soft_stop_mix_stall_sec_)) {
                soft_stop_elapsed_sec_ = -1.0;
                soft_stop_mix_stall_sec_ = 0.0;
                soft_stop_last_mix_calls_ = 0;
                AudioStreamPlayer3D::stop();
                return;
            }
        }
    }
    if (auto_exclude_colliders_ && srv && srv->uses_custom_ray_tracer()) {
        if (++physics_auto_exclude_resync_counter_ >= 60) {
            physics_auto_exclude_resync_counter_ = 0;
            _sync_physics_ray_auto_exclude_rids();
        }
    } else {
        physics_auto_exclude_resync_counter_ = 0;
    }
    _tick_debug_overlay_grace_timer(delta, srv);
    const bool show_debug_hud = _compute_show_debug_hud(srv);

    if (!is_playing()) {
        _sync_source_simulation_activity(srv);
        _present_player_debug_overlay_from_cache(delta, srv, show_debug_hud);
        return;
    }

    if (srv)
        _invalidate_source_handle_if_stale(srv);

    if (srv && source_handle < 0 && srv->is_initialized())
        _try_ensure_source_and_sync(srv, true);

    if (!player_sync_uses_physics_)
        _prepare_source_for_simulation(srv);

    if (player_sync_uses_physics_) {
        _present_player_debug_overlay_from_cache(delta, srv, show_debug_hud);
        return;
    }

    if (!srv || !srv->is_simulating() || source_handle < 0) {
        _present_player_debug_overlay_from_cache(delta, srv, show_debug_hud);
        return;
    }

    _sync_playback_simulation_frame(delta, srv, show_debug_hud);
}

int32_t ResonancePlayer::_create_simulation_source(ResonanceServer* srv) {
    if (srv->simulation_source_cap_blocks_create())
        return -1;
    const float eff_radius = _config_float("source_radius", 1.0f);
    const int pathing_override = _config_int("pathing_enabled_override", -1);
    return srv->create_source_handle(get_global_position(), eff_radius, get_path(), pathing_override);
}

bool ResonancePlayer::_source_simulation_wanted() const {
    return is_playing() && !voice_virtual_state_.virtualized;
}

void ResonancePlayer::_sync_source_simulation_activity(ResonanceServer* srv) {
    if (source_simulation_active_pushed_ == _source_simulation_wanted())
        return;
    if (!srv || !srv->is_initialized() || source_handle < 0)
        return;
    if (_source_simulation_wanted()) {
        // Inline when the simulation mutex is free so direct runs before the first mix; a dropped
        // update is resent by the per-frame sync while playing.
        _prepare_source_for_simulation(srv);
        return;
    }
    // Queued, not try_update_source: the stop transition is sent once and must not be dropped.
    _ensure_config_and_apply_source(_resolve_pathing_probe_batch());
}

bool ResonancePlayer::_try_ensure_source_and_sync(ResonanceServer* srv, bool deferred_playback_push_if_playing) {
    _invalidate_source_handle_if_stale(srv);
    if (!player_config.is_valid() || source_handle >= 0)
        return false;
    if (!srv || !srv->is_initialized() || srv->simulation_source_cap_blocks_create())
        return false;
    resonance::ScopedSpawnPhase phase(&spawn_timing_, resonance::SpawnPhase::Source);

    const int32_t h = _create_simulation_source(srv);
    if (h < 0) {
        if (!warned_source_handle_create_failed_ && !srv->simulation_source_cap_blocks_create()) {
            warned_source_handle_create_failed_ = true;
            ResonanceLog::warn(
                "ResonancePlayer: create_source_handle failed (is the simulator ready?). Reverb/occlusion may stay dry until it succeeds.");
        }
        return false;
    }
    warned_source_handle_create_failed_ = false;
    source_handle = h;
    source_lifecycle_epoch_ = srv->get_source_lifecycle_epoch();
    attenuation_setup_cache_.valid = false;
    attenuation_callback_handles_.clear();
    _prepare_source_for_simulation(srv);
    if (deferred_playback_push_if_playing && is_playing())
        call_deferred("_deferred_push_playback_parameters");
    return true;
}

void ResonancePlayer::_collect_simulation_source_handles(std::vector<int32_t>& out) const {
    out.clear();
    std::vector<ResonanceStreamPlayback*> voices;
    internal_copy_internal_playbacks(voices);
    for (ResonanceStreamPlayback* pb : voices) {
        if (!pb)
            continue;
        const int32_t h = pb->get_voice_source_handle();
        if (h < 0)
            continue;
        bool seen = false;
        for (int32_t existing : out) {
            if (existing == h) {
                seen = true;
                break;
            }
        }
        if (!seen)
            out.push_back(h);
    }
    if (out.empty() && source_handle >= 0)
        out.push_back(source_handle);
}

bool ResonancePlayer::_live_voice_uses_source(int32_t handle) const {
    if (handle < 0)
        return false;
    std::vector<ResonanceStreamPlayback*> voices;
    internal_copy_internal_playbacks(voices);
    for (ResonanceStreamPlayback* pb : voices) {
        if (pb && pb->get_voice_source_handle() == handle)
            return true;
    }
    return false;
}

int32_t ResonancePlayer::_first_live_voice_source(uint32_t* out_epoch) const {
    if (out_epoch)
        *out_epoch = 0;
    std::vector<ResonanceStreamPlayback*> voices;
    internal_copy_internal_playbacks(voices);
    for (ResonanceStreamPlayback* pb : voices) {
        if (!pb)
            continue;
        const int32_t h = pb->get_voice_source_handle();
        if (h < 0)
            continue;
        if (out_epoch)
            *out_epoch = pb->get_voice_source_epoch();
        return h;
    }
    return -1;
}

void ResonancePlayer::_reclaim_voice_source_on_main(int32_t handle, uint32_t epoch) {
    if (handle < 0)
        return;
    if (_live_voice_uses_source(handle))
        return;
    uint32_t live_epoch = 0;
    const int32_t live = _first_live_voice_source(&live_epoch);
    if (live < 0 && (source_handle < 0 || source_handle == handle)) {
        source_handle = handle;
        source_lifecycle_epoch_ = epoch;
        return;
    }
    if (source_handle == handle) {
        source_handle = live;
        source_lifecycle_epoch_ = live >= 0 ? live_epoch : 0;
    }
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!srv || ResonanceServer::is_shutting_down())
        return;
    if (!resonance::source_handle_matches_lifecycle_epoch(handle, epoch, srv->get_source_lifecycle_epoch()))
        return;
    srv->destroy_source_handle(handle);
}

void ResonancePlayer::_drain_voice_source_reclaims() {
    std::vector<PendingVoiceSourceReclaim> local;
    {
        std::lock_guard<std::mutex> lock(voice_source_reclaim_mutex_);
        local.swap(voice_source_reclaims_);
    }
    for (const PendingVoiceSourceReclaim& item : local)
        _reclaim_voice_source_on_main(item.handle, item.epoch);
}

void ResonancePlayer::_block_live_convolution_and_wait() {
    std::vector<ResonanceStreamPlayback*> voices;
    internal_copy_internal_playbacks(voices);
    for (ResonanceStreamPlayback* pb : voices) {
        if (pb)
            pb->block_convolution_for_source_release();
    }
    for (ResonanceStreamPlayback* pb : voices) {
        if (!pb)
            continue;
        while (pb->convolution_apply_depth() > 0)
            std::this_thread::yield();
    }
}

void ResonancePlayer::_destroy_owned_sources_for_shutdown() {
    _block_live_convolution_and_wait();
    cleanup_shared_reflection();
    _detach_playback_source_retains();
    std::vector<PendingVoiceSourceReclaim> doomed;
    if (source_handle >= 0) {
        PendingVoiceSourceReclaim idle;
        idle.handle = source_handle;
        idle.epoch = source_lifecycle_epoch_;
        doomed.push_back(idle);
    }
    std::vector<ResonanceStreamPlayback*> voices;
    internal_copy_internal_playbacks(voices);
    for (ResonanceStreamPlayback* pb : voices) {
        if (!pb)
            continue;
        PendingVoiceSourceReclaim item;
        item.handle = pb->get_voice_source_handle();
        item.epoch = pb->get_voice_source_epoch();
        pb->set_voice_source_binding(-1, 0);
        if (item.handle >= 0)
            doomed.push_back(item);
    }
    {
        std::lock_guard<std::mutex> lock(voice_source_reclaim_mutex_);
        for (const PendingVoiceSourceReclaim& item : voice_source_reclaims_)
            doomed.push_back(item);
        voice_source_reclaims_.clear();
    }
    source_handle = -1;
    source_lifecycle_epoch_ = 0;
    attenuation_callback_handles_.clear();
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!srv || ResonanceServer::is_shutting_down())
        return;
    const uint32_t server_epoch = srv->get_source_lifecycle_epoch();
    std::vector<int32_t> destroyed;
    for (const PendingVoiceSourceReclaim& item : doomed) {
        if (item.handle < 0)
            continue;
        bool seen = false;
        for (int32_t prev : destroyed) {
            if (prev == item.handle) {
                seen = true;
                break;
            }
        }
        if (seen)
            continue;
        destroyed.push_back(item.handle);
        if (resonance::source_handle_matches_lifecycle_epoch(item.handle, item.epoch, server_epoch))
            srv->destroy_source_handle(item.handle);
    }
}

void ResonancePlayer::_zero_shared_reflection_accum() {
    if (!shared_refl_accum_.data || shared_refl_frame_size_ <= 0)
        return;
    for (int c = 0; c < shared_refl_accum_.numChannels; ++c) {
        if (shared_refl_accum_.data[c])
            memset(shared_refl_accum_.data[c], 0, static_cast<size_t>(shared_refl_frame_size_) * sizeof(float));
    }
}

void ResonancePlayer::shared_reflection_context_cleanup(void* userdata) {
    if (userdata)
        static_cast<ResonancePlayer*>(userdata)->cleanup_shared_reflection();
}

void ResonancePlayer::cleanup_shared_reflection() {
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (srv)
        srv->unregister_ipl_context_client(this);
    DetachedReflectionEffect detached = shared_reflection_.detach_effect();
    if (srv)
        srv->give_back_reflection_effect(shared_refl_context_, detached.recipe, detached.effect);
    else if (detached.effect)
        iplReflectionEffectRelease(&detached.effect);
    shared_reflection_.cleanup();
    if (shared_refl_context_ && shared_refl_accum_.data)
        iplAudioBufferFree(shared_refl_context_, &shared_refl_accum_);
    memset(&shared_refl_accum_, 0, sizeof(shared_refl_accum_));
    shared_refl_context_ = nullptr;
    shared_refl_frame_size_ = 0;
    shared_reflection_inited_ = false;
    shared_tail_have_ = false;
    shared_tail_source_ = -1;
    shared_tail_ir_gen_ = 0;
    refl_round_seen_ = 0;
    refl_round_closer_ = nullptr;
    refl_apply_reader_ = -1;
}

void ResonancePlayer::ensure_shared_reflection_on_main(ResonanceServer* srv) {
    if (!srv || !srv->is_initialized())
        return;
    if (!internal_wants_wet_effects())
        return;
    IPLContext ctx = srv->get_context_handle();
    const int frame = srv->get_audio_frame_size();
    if (shared_reflection_inited_ && shared_refl_context_ == ctx && shared_refl_frame_size_ == frame)
        return;
    resonance::ScopedSpawnPhase phase(&spawn_timing_, resonance::SpawnPhase::ReflectionEffect);
    cleanup_shared_reflection();
    if (!ctx || frame <= 0)
        return;
    shared_reflection_.initialize(ctx, srv->get_sample_rate(), frame, srv->get_ambisonic_order(), srv->get_reflection_type(),
                                  srv->get_realtime_simulation_duration(), srv->get_convolution_ir_max_samples(),
                                  srv->take_pooled_reflection_effect());
    if (iplAudioBufferAllocate(ctx, 2, frame, &shared_refl_accum_) != IPL_STATUS_SUCCESS) {
        shared_reflection_.cleanup();
        return;
    }
    shared_refl_context_ = ctx;
    shared_refl_frame_size_ = frame;
    shared_reflection_inited_ = true;
    // The server destroys the context on shutdown and reinit; the effect must be released before that.
    srv->register_ipl_context_client(this, &ResonancePlayer::shared_reflection_context_cleanup);
}

bool ResonancePlayer::shared_reflection_is_parametric() const {
    return shared_reflection_inited_ && shared_reflection_.is_parametric();
}

bool ResonancePlayer::shared_reflection_tail_applyable(const ResonanceServer* srv) const {
    if (!srv)
        return false;
    return resonance::emitter_tail_applyable(shared_tail_have_, shared_tail_source_, source_handle, shared_tail_ir_gen_,
                                             srv->get_source_ir_generation(source_handle));
}

int ResonancePlayer::shared_reflection_tail_samples() const {
    return shared_reflection_inited_ ? shared_reflection_.get_tail_size_samples() : 0;
}

IPLAudioBuffer* ResonancePlayer::shared_reflection_direct_output() {
    if (!shared_reflection_inited_)
        return nullptr;
    return shared_reflection_.get_direct_output_buffer();
}

void ResonancePlayer::reflection_round_enter(ResonanceStreamPlayback* voice) {
    if (!voice)
        return;
    // A repeated voice means the previous callback never reached its expected count.
    // Close that round so a missing playback cannot stall the single Apply.
    const int seen_before = std::min(refl_round_seen_, kReflectionRoundVoices);
    for (int i = 0; i < seen_before; ++i) {
        if (refl_round_voices_[i] != voice)
            continue;
        if (!refl_round_applied_) {
            if (!refl_round_closer_)
                refl_round_closer_ = voice;
            reflection_apply_outside_block(refl_round_closer_);
        }
        refl_round_seen_ = 0;
        refl_round_closer_ = nullptr;
        break;
    }
    if (refl_round_seen_ == 0) {
        const int n = playback_count_.load(std::memory_order_acquire);
        refl_round_expected_ = n > 0 ? n : 1;
        refl_round_applied_ = false;
        refl_round_have_dry_ = false;
        refl_round_closer_ = nullptr;
        refl_apply_reader_ = -1;
        refl_round_mix_ = 1.0f;
        refl_round_wet_occ_ = 1.0f;
        refl_round_eq_[0] = 1.0f;
        refl_round_eq_[1] = 1.0f;
        refl_round_eq_[2] = 1.0f;
        refl_round_delay_ = -1;
        _zero_shared_reflection_accum();
    }
    if (refl_round_seen_ < kReflectionRoundVoices)
        refl_round_voices_[refl_round_seen_] = voice;
    refl_round_seen_ += 1;
    if (refl_round_seen_ >= refl_round_expected_)
        refl_round_closer_ = voice;
}

void ResonancePlayer::reflection_round_add(ResonanceStreamPlayback* voice) {
    if (!voice || !shared_reflection_inited_ || !shared_refl_accum_.data)
        return;
    if (!voice->params_current.enable_reverb)
        return;
    if (source_handle < 0 || voice->get_voice_source_handle() != source_handle)
        return;
    const int n = std::min(shared_refl_frame_size_, voice->frame_size_);
    if (n <= 0 || !voice->sa_in_buffer.data)
        return;
    const int channels = std::min(shared_refl_accum_.numChannels, voice->sa_in_buffer.numChannels);
    for (int c = 0; c < channels; ++c) {
        float* dst = shared_refl_accum_.data[c];
        float* src = voice->sa_in_buffer.data[c];
        if (!dst || !src)
            continue;
        for (int i = 0; i < n; ++i)
            dst[i] = resonance::sanitize_audio_float(dst[i] + src[i]);
    }
    refl_round_have_dry_ = true;
    refl_round_wet_occ_ = voice->params_current.wet_occlusion_factor;
    refl_round_mix_ = voice->params_current.reflections_mix_level;
    refl_round_eq_[0] = voice->params_current.reflections_eq[0];
    refl_round_eq_[1] = voice->params_current.reflections_eq[1];
    refl_round_eq_[2] = voice->params_current.reflections_eq[2];
    refl_round_delay_ = voice->params_current.reflections_delay;
}

bool ResonancePlayer::reflection_try_apply(ResonanceStreamPlayback* voice, bool& out_reverb_to_player, float& out_dbg_reverb) {
    if (!voice || voice != refl_round_closer_ || refl_round_applied_)
        return false;
    refl_round_applied_ = true;
    out_reverb_to_player = false;
    out_dbg_reverb = 0.0f;
    if (voice->params_current.virtualized)
        return false;
    if (!shared_reflection_inited_ || source_handle < 0)
        return false;
    if (!voice->params_current.enable_reverb && !refl_round_have_dry_)
        return false;
    if (!resonance::claim_reflection_apply(&refl_apply_reader_, resonance::kEmitterReflectionReader))
        return false;

    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!srv || !srv->is_initialized()) {
        refl_apply_reader_ = -1;
        return false;
    }
    IPLReflectionEffectParams params{};
    const bool fetched = srv->fetch_reverb_params(source_handle, params, nullptr);
    if (!fetched) {
        if (!shared_reflection_tail_applyable(srv)) {
            refl_apply_reader_ = -1;
            return false;
        }
        params = shared_tail_params_;
    }
    const int refl_type = srv->get_reflection_type();
    const float wet_occ = resonance::sanitize_audio_float(refl_round_wet_occ_);
    const float curr_mix = resonance::sanitize_audio_float(resonance::conv_reflection_wet_mix_level(refl_round_mix_, wet_occ));
    bool fed = false;
    if (!voice->convolution_try_enter()) {
        refl_apply_reader_ = -1;
        return false;
    }
    if (refl_type == resonance::kReflectionConvolution || refl_type == resonance::kReflectionTan) {
        auto mixer_guard = srv->scoped_mixer_read();
        IPLReflectionMixer mixer = mixer_guard.get();
        if (mixer && shared_refl_accum_.data) {
            fed = shared_reflection_.process_mix(shared_refl_accum_, params, mixer, shared_prev_conv_mix_, curr_mix, 1.0f);
            if (fed) {
                shared_prev_conv_mix_ = curr_mix;
                srv->record_mixer_feed();
                out_dbg_reverb = curr_mix;
            }
        }
    } else {
        if (refl_type == resonance::kReflectionHybrid) {
            if (refl_round_eq_[0] != 1.0f || refl_round_eq_[1] != 1.0f || refl_round_eq_[2] != 1.0f) {
                params.eq[0] *= refl_round_eq_[0];
                params.eq[1] *= refl_round_eq_[1];
                params.eq[2] *= refl_round_eq_[2];
            }
            if (refl_round_delay_ >= 0)
                params.delay = refl_round_delay_;
        }
        const float parametric_mix = resonance::sanitize_audio_float(refl_round_mix_ * wet_occ);
        fed = shared_reflection_.process_mix_direct(shared_refl_accum_, params, shared_prev_param_mix_, parametric_mix);
        if (fed) {
            shared_prev_param_mix_ = parametric_mix;
            out_reverb_to_player = true;
            out_dbg_reverb = parametric_mix;
        }
    }
    voice->convolution_leave();
    if (fed) {
        shared_tail_params_ = params;
        shared_tail_have_ = true;
        shared_tail_source_ = source_handle;
        shared_tail_ir_gen_ = srv->get_source_ir_generation(source_handle);
    }
    refl_apply_reader_ = -1;
    return fed;
}

bool ResonancePlayer::reflection_try_drain_tail(ResonanceStreamPlayback* voice, bool& out_reverb_to_player) {
    out_reverb_to_player = false;
    if (!voice || !shared_reflection_inited_ || source_handle < 0)
        return false;
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!srv || !srv->is_initialized())
        return false;
    if (!resonance::reflection_round_should_drain_tail(voice == refl_round_closer_, refl_round_applied_, refl_round_have_dry_,
                                                       shared_reflection_tail_applyable(srv), shared_reflection_tail_samples()))
        return false;
    if (!resonance::claim_reflection_apply(&refl_apply_reader_, resonance::kEmitterReflectionReader))
        return false;
    refl_round_applied_ = true;

    IPLReflectionEffectParams params{};
    if (!srv->fetch_reverb_params(source_handle, params, nullptr))
        params = shared_tail_params_;
    const int refl_type = srv->get_reflection_type();
    resonance::reflection_eos_tail_refresh_tan_device(refl_type, params, srv->get_tan_device_for_reflection());
    if (!voice->convolution_try_enter()) {
        refl_apply_reader_ = -1;
        return false;
    }
    bool produced = false;
    if (refl_type == resonance::kReflectionConvolution || refl_type == resonance::kReflectionTan) {
        auto mixer_guard = srv->scoped_mixer_read();
        IPLReflectionMixer mixer = mixer_guard.get();
        if (mixer) {
            const IPLAudioEffectState state = shared_reflection_.tail_apply_to_mixer(&params, mixer);
            produced = resonance::reflection_eos_tail_produced(state == IPL_AUDIOEFFECTSTATE_TAILCOMPLETE);
            if (produced)
                srv->record_mixer_feed();
        } else {
            voice->instrumentation_conv_mixer_null_blocks.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        const IPLAudioEffectState state = shared_reflection_.tail_apply_direct(&params);
        produced = resonance::reflection_eos_tail_produced(state == IPL_AUDIOEFFECTSTATE_TAILCOMPLETE);
        out_reverb_to_player = produced;
    }
    voice->convolution_leave();
    refl_apply_reader_ = -1;
    return produced;
}

void ResonancePlayer::reflection_round_leave(ResonanceStreamPlayback* voice) {
    if (refl_round_seen_ <= 0)
        return;
    if (refl_round_seen_ >= refl_round_expected_) {
        reflection_apply_outside_block(voice);
        refl_round_seen_ = 0;
        refl_round_closer_ = nullptr;
    }
}

void ResonancePlayer::reflection_apply_outside_block(ResonanceStreamPlayback* voice) {
    const auto start = std::chrono::steady_clock::now();
    bool to_player = false;
    float dbg = 0.0f;
    reflection_try_apply(voice, to_player, dbg);
    instrumentation_reflection_outside_block_ns_.fetch_add(resonance::elapsed_ns(start), std::memory_order_relaxed);
}

void ResonancePlayer::_ensure_voice_sources(ResonanceServer* srv) {
    if (!player_config.is_valid() || !srv || !srv->is_initialized())
        return;
    _invalidate_source_handle_if_stale(srv);
    if (source_handle < 0) {
        const int32_t created = _create_simulation_source(srv);
        if (created < 0) {
            if (!warned_source_handle_create_failed_ && !srv->simulation_source_cap_blocks_create()) {
                warned_source_handle_create_failed_ = true;
                ResonanceLog::warn(
                    "ResonancePlayer: create_source_handle failed (is the simulator ready?). Reverb/occlusion may stay dry until it succeeds.");
            }
            return;
        }
        warned_source_handle_create_failed_ = false;
        source_handle = created;
        source_lifecycle_epoch_ = srv->get_source_lifecycle_epoch();
        attenuation_setup_cache_.valid = false;
        attenuation_callback_handles_.clear();
        _prepare_source_for_simulation(srv);
    }

    std::vector<ResonanceStreamPlayback*> voices;
    internal_copy_internal_playbacks(voices);
    for (ResonanceStreamPlayback* pb : voices) {
        if (!pb)
            continue;
        const int32_t previous = pb->get_voice_source_handle();
        if (previous >= 0 && previous != source_handle) {
            pb->block_convolution_for_source_release();
            while (pb->convolution_apply_depth() > 0)
                std::this_thread::yield();
            srv->destroy_source_handle(previous);
        }
        pb->set_voice_source_binding(source_handle, source_lifecycle_epoch_);
    }
    ensure_shared_reflection_on_main(srv);
}

void ResonancePlayer::_invalidate_source_handle_if_stale(ResonanceServer* srv) {
    const uint32_t server_epoch = srv ? srv->get_source_lifecycle_epoch() : 0u;
    bool stale = false;
    if (source_handle >= 0 &&
        !resonance::source_handle_matches_lifecycle_epoch(source_handle, source_lifecycle_epoch_, server_epoch))
        stale = true;
    std::vector<ResonanceStreamPlayback*> voices;
    internal_copy_internal_playbacks(voices);
    for (ResonanceStreamPlayback* pb : voices) {
        if (!pb)
            continue;
        const int32_t h = pb->get_voice_source_handle();
        if (h < 0)
            continue;
        if (!resonance::source_handle_matches_lifecycle_epoch(h, pb->get_voice_source_epoch(), server_epoch))
            stale = true;
    }
    if (!stale)
        return;
    _detach_playback_source_retains();
    for (ResonanceStreamPlayback* pb : voices) {
        if (pb)
            pb->set_voice_source_binding(-1, 0);
    }
    source_handle = -1;
    source_lifecycle_epoch_ = 0;
    attenuation_setup_cache_.valid = false;
    attenuation_callback_handles_.clear();
}

void ResonancePlayer::reload_source_after_reinit() {
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint())
        return;
    if (!player_config.is_valid())
        return;
    _detach_playback_source_retains();
    std::vector<ResonanceStreamPlayback*> voices;
    internal_copy_internal_playbacks(voices);
    for (ResonanceStreamPlayback* pb : voices) {
        if (pb)
            pb->set_voice_source_binding(-1, 0);
    }
    source_handle = -1;
    source_lifecycle_epoch_ = 0;
    attenuation_setup_cache_.valid = false;
    attenuation_callback_handles_.clear();
    ResonanceServer* srv = ResonanceServer::get_singleton();
    _try_ensure_source_and_sync(srv, is_playing());
}

void ResonancePlayer::_deferred_try_ensure_source_after_config() {
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint())
        return;
    if (!player_config.is_valid())
        return;
    _update_stream_setup();
    ResonanceServer* srv = ResonanceServer::get_singleton();
    _try_ensure_source_and_sync(srv, is_playing());
}

void ResonancePlayer::_ensure_source_exists() {
    ResonanceServer* srv = ResonanceServer::get_singleton();
    (void)_try_ensure_source_and_sync(srv, false);
}

void ResonancePlayer::_start_reverb_send_child_if_needed() {
    Node* reverb_child = get_node_or_null(NodePath("ResonanceReverbOutput"));
    if (reverb_child && reverb_child->is_class("AudioStreamPlayer")) {
        if (AudioStreamPlayer* rp = Object::cast_to<AudioStreamPlayer>(reverb_child)) {
            if (!rp->is_playing())
                rp->play();
        }
    }
}

void ResonancePlayer::set_stream(const Ref<AudioStream>& p_stream) {
    Engine* eng = Engine::get_singleton();
    const bool editor = eng && eng->is_editor_hint();
    if (!player_config.is_valid()) {
        logical_stream_.unref();
        internal_stream.unref();
        AudioStreamPlayer3D::set_stream(p_stream);
        return;
    }
    logical_stream_ = p_stream;
    if (editor) {
        internal_stream.unref();
        AudioStreamPlayer3D::set_stream(p_stream);
        return;
    }
    if (!internal_stream.is_valid())
        internal_stream.instantiate();
    internal_stream->set_base_stream(logical_stream_);
    internal_stream->set_stream_owner(this);
    AudioStreamPlayer3D::set_stream(internal_stream);
}

Ref<AudioStream> ResonancePlayer::get_stream() const {
    if (!player_config.is_valid())
        return AudioStreamPlayer3D::get_stream();
    if (logical_stream_.is_valid())
        return logical_stream_;
    return AudioStreamPlayer3D::get_stream();
}

Ref<AudioStream> ResonancePlayer::get_inner_stream() const {
    return get_stream();
}

Ref<AudioStreamPlayback> ResonancePlayer::get_inner_stream_playback() {
    if (!player_config.is_valid()) {
        return get_stream_playback();
    }

    Ref<AudioStreamPlayback> pb = get_stream_playback();
    if (pb.is_valid()) {
        if (ResonanceStreamPlayback* res_pb = Object::cast_to<ResonanceStreamPlayback>(pb.ptr())) {
            return res_pb->get_base_playback();
        }
        return pb;
    }

    PlaybackVoiceSnapshot snap;
    internal_get_playback_snapshot_for_audio(snap);
    for (int i = 0; i < snap.count; ++i) {
        ResonanceStreamPlayback* voice = snap.voices[static_cast<size_t>(i)];
        if (voice) {
            Ref<AudioStreamPlayback> base = voice->get_base_playback();
            if (base.is_valid()) {
                return base;
            }
        }
    }

    std::lock_guard<std::mutex> lock(internal_playbacks_mutex_);
    for (ResonanceStreamPlayback* res_pb : internal_playbacks_) {
        if (res_pb) {
            Ref<AudioStreamPlayback> base = res_pb->get_base_playback();
            if (base.is_valid()) {
                return base;
            }
        }
    }

    return Ref<AudioStreamPlayback>();
}

void ResonancePlayer::play(float from_position) {
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint()) {
        AudioStreamPlayer3D::play(from_position);
        _publish_playing_for_audio();
        return;
    }
    play_serial_++;
    voice_virtual_state_ = {};
    dry_finished_emitted_ = false;
    dry_finished_deferred_queued_ = false;
    dry_finished_deferred_serial_ = 0;
    soft_stop_elapsed_sec_ = -1.0;
    soft_stop_mix_stall_sec_ = 0.0;
    soft_stop_last_mix_calls_ = 0;
    if (player_config.is_valid()) {
        playback_lod_have_anchor_ = false;
        playback_lod_time_since_full_ = 0.0;
        _sync_bus_routing_with_config();
        _update_stream_setup();
        ResonanceServer* srv = ResonanceServer::get_singleton();
        _try_ensure_source_and_sync(srv, false);
    }
    AudioStreamPlayer3D::play(from_position);
    _publish_playing_for_audio();
    if (player_config.is_valid()) {
        _sync_source_simulation_activity(ResonanceServer::get_singleton());
        _refresh_effective_volume_cache();
        _start_reverb_send_child_if_needed();
        // Push now so the first _mix opens the params gate at the real position instead of waiting a frame.
        // Empty simulation caches still give correct positioning.
        ResonanceServer* srv = ResonanceServer::get_singleton();
        if (srv && srv->is_simulating() && source_handle >= 0) {
            resonance::ScopedSpawnPhase phase(&spawn_timing_, resonance::SpawnPhase::ParamsPush);
            _push_playback_parameters_from_simulation(srv, nullptr, 0.0, false);
        }
        // The source handle may not exist yet this tick.
        call_deferred("_deferred_push_playback_parameters");
    }
}

void ResonancePlayer::_update_stream_setup() {
    if (!player_config.is_valid())
        return;
    const Ref<AudioStream> engine_s = AudioStreamPlayer3D::get_stream();
    if (internal_stream.is_valid() && engine_s == internal_stream) {
        if (ResonanceStream* ris = Object::cast_to<ResonanceStream>(internal_stream.ptr())) {
            ris->set_stream_owner(this);
            ris->set_base_stream(logical_stream_);
        }
        return;
    }
    logical_stream_ = engine_s;
    if (!internal_stream.is_valid())
        internal_stream.instantiate();
    internal_stream->set_base_stream(logical_stream_);
    internal_stream->set_stream_owner(this);
    AudioStreamPlayer3D::set_stream(internal_stream);
}

void ResonancePlayer::play_stream(double from_pos) {
    // NOLINTNEXTLINE(bugprone-narrowing-conversions)
    play(static_cast<float>(from_pos));
}

void ResonancePlayer::play_animation_audio_clip(const Ref<AudioStream>& p_stream, float from_position) {
    set_stream(p_stream);
    play(from_position);
}

void ResonancePlayer::stop() {
    playback_lod_have_anchor_ = false;
    playback_lod_time_since_full_ = 0.0;
    last_pushed_playback_params_valid_ = false;
    coeff_smooth_initialized_ = false;
    coeff_smooth_source_handle_ = -1;

    Engine* eng = Engine::get_singleton();
    const bool editor = eng && eng->is_editor_hint();

    // In the editor or without a config, stop hard like a plain AudioStreamPlayer3D.
    if (editor || !player_config.is_valid()) {
        Node* reverb_child = get_node_or_null(NodePath("ResonanceReverbOutput"));
        if (reverb_child && reverb_child->is_class("AudioStreamPlayer")) {
            if (AudioStreamPlayer* rp = Object::cast_to<AudioStreamPlayer>(reverb_child))
                rp->stop();
        }
        AudioStreamPlayer3D::stop();
        _publish_playing_for_audio();
        return;
    }

    // Soft stop: no AudioStreamPlayer3D::stop(), so Godot keeps mixing each voice while its reverb and path
    // tails decay. The reverb send child also keeps running until its rings are empty.
    std::vector<ResonanceStreamPlayback*> copy;
    internal_copy_internal_playbacks(copy);
    bool any_soft_stopped = false;
    for (ResonanceStreamPlayback* pb : copy) {
        if (pb) {
            pb->request_soft_stop();
            any_soft_stopped = true;
        }
    }
    if (any_soft_stopped) {
        soft_stop_elapsed_sec_ = 0.0;
        soft_stop_mix_stall_sec_ = 0.0;
        soft_stop_last_mix_calls_ = 0;
    } else {
        // No Resonance voice to soft-stop, e.g. a native AudioStreamPolyphonic: stop hard.
        soft_stop_elapsed_sec_ = -1.0;
        Node* reverb_child = get_node_or_null(NodePath("ResonanceReverbOutput"));
        if (reverb_child && reverb_child->is_class("AudioStreamPlayer")) {
            if (AudioStreamPlayer* rp = Object::cast_to<AudioStreamPlayer>(reverb_child))
                rp->stop();
        }
        AudioStreamPlayer3D::stop();
        _publish_playing_for_audio();
    }
}

void ResonancePlayer::set_pathing_probe_volume(const NodePath& p_path) { pathing_probe_volume = p_path; }
NodePath ResonancePlayer::get_pathing_probe_volume() const { return pathing_probe_volume; }

void ResonancePlayer::clear_pathing_probe_immediate() {
    pathing_probe_volume = NodePath();
    detach_pathing_probe_immediate();
}

void ResonancePlayer::detach_pathing_probe_immediate() {
    ResonanceServer* srv = ResonanceServer::get_singleton();
    _invalidate_source_handle_if_stale(srv);
    if (!srv || !srv->is_initialized() || source_handle < 0)
        return;
    _ensure_config_and_apply_source(-1);
}

void ResonancePlayer::set_inputs(int flags) {
    (void)flags;
    if (!player_config.is_valid())
        return;
    ResonanceServer* srv = ResonanceServer::get_singleton();
    _invalidate_source_handle_if_stale(srv);
    if (!srv || !srv->is_initialized() || source_handle < 0)
        return;
    _ensure_config_valid();
    _setup_attenuation(srv);
    int32_t pathing_batch = -1;
    if (!pathing_probe_volume.is_empty()) {
        Node* node = get_node_or_null(pathing_probe_volume);
        if (ResonanceProbeVolume* pv = Object::cast_to<ResonanceProbeVolume>(node))
            pathing_batch = pv->get_probe_batch_handle();
    }
    _apply_update_source(pathing_batch, false);
}

Dictionary ResonancePlayer::get_outputs() const {
    Dictionary out;
    if (!player_config.is_valid() || source_handle < 0)
        return out;
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!srv || !srv->is_initialized())
        return out;
    out = srv->get_source_occlusion_data_dict(source_handle);
    if (config_cache_valid_) {
        const float master = resonance::sanitize_audio_float(config_cache_.master_mix_level);
        out["reflections_mix_level"] = resonance::sanitize_audio_float(config_cache_.reflections_mix_level * master);
        out["pathing_mix_level"] = resonance::sanitize_audio_float(config_cache_.pathing_mix_level * master);
        out["reflections_enabled"] = config_cache_.reflections_enabled != 0;
        out["distance_attenuation_enabled"] = config_cache_.distance_attenuation &&
                                              config_cache_.attenuation_mode != ATTENUATION_DISABLED;
    }
    return out;
}

void ResonancePlayer::set_auto_exclude_colliders(bool p_enable) {
    if (auto_exclude_colliders_ == p_enable)
        return;
    auto_exclude_colliders_ = p_enable;
    if (!p_enable)
        _clear_physics_ray_auto_exclude_rids();
    else
        _sync_physics_ray_auto_exclude_rids();
}

void ResonancePlayer::set_exclude_from_debug(bool p_exclude) { exclude_from_debug_ = p_exclude; }

void ResonancePlayer::set_convert_anim_audio_runtime(bool p_enable) {
    convert_anim_audio_runtime_ = p_enable;
    Engine* eng = Engine::get_singleton();
    if (p_enable && player_config.is_valid() && is_inside_tree() && eng && !eng->is_editor_hint())
        call_deferred("_nexus_deferred_spawn_anim_audio_helper");
}

void ResonancePlayer::set_player_config(const Ref<Resource>& p_config) {
    const bool had_config = player_config.is_valid();
    const Callable changed_cb = callable_mp(this, &ResonancePlayer::_on_player_config_changed_refresh_gizmo);
    if (had_config && player_config->is_connected("changed", changed_cb))
        player_config->disconnect("changed", changed_cb);
    player_config = p_config;
    config_cache_valid_ = false;
    if (had_config && !player_config.is_valid()) {
        set_process(false);
        const Ref<AudioStream> logical = logical_stream_;
        internal_stream.unref();
        logical_stream_.unref();
        AudioStreamPlayer3D::set_stream(logical);
    } else if (player_config.is_valid()) {
        set_process(true);
        _apply_steam_mode_asp3d_guards();
        _refresh_effective_volume_cache();
        call_deferred("_deferred_try_ensure_source_after_config");
        Engine* eng = Engine::get_singleton();
        if (eng && !eng->is_editor_hint() && convert_anim_audio_runtime_ && is_inside_tree())
            call_deferred("_nexus_deferred_spawn_anim_audio_helper");
    }
    if (player_config.is_valid() && !player_config->is_connected("changed", changed_cb))
        player_config->connect("changed", changed_cb);
    // Gaining or losing a config changes which inherited properties _validate_property hides.
    if (had_config != player_config.is_valid())
        notify_property_list_changed();
    _on_player_config_changed_refresh_gizmo();
}
Ref<Resource> ResonancePlayer::get_player_config() const { return player_config; }

void ResonancePlayer::_validate_property(PropertyInfo& p_property) const {
    // With a config, Steam Audio owns distance and the inherited AudioStreamPlayer3D spatial settings do
    // nothing, so hide them.
    if (!player_config.is_valid())
        return;

    static const StringName hidden[] = {
        StringName("bus"),
        StringName("attenuation_model"),
        StringName("max_distance"),
        StringName("unit_size"),
        StringName("panning_strength"),
        StringName("area_mask"),
        StringName("doppler_tracking"),
        StringName("emission_angle_enabled"),
        StringName("emission_angle_degrees"),
        StringName("emission_angle_filter_attenuation_db"),
        StringName("attenuation_filter_cutoff_hz"),
        StringName("attenuation_filter_db"),
        StringName("playback_type"),
    };
    for (const StringName& name : hidden) {
        if (p_property.name == name) {
            p_property.usage &= ~PROPERTY_USAGE_EDITOR;
            return;
        }
    }
}

void ResonancePlayer::set_show_directivity_gizmo(bool p_enable) {
    if (show_directivity_gizmo_ == p_enable)
        return;
    show_directivity_gizmo_ = p_enable;
    directivity_drawer_.mark_dirty();
    update_gizmos();
}

void ResonancePlayer::_on_player_config_changed_refresh_gizmo() {
    config_cache_valid_ = false;
    config_cache_frame_countdown = 0;
    Engine* eng = Engine::get_singleton();
    const bool editor = eng && eng->is_editor_hint();
    // Runtime mix toggles can emit `changed` on every interaction. While playing, invalidating the cache
    // is enough; skip the gizmo redraw and bus routing.
    if (!editor && is_playing())
        return;
    directivity_drawer_.mark_dirty();
    update_gizmos();
    _apply_player_config_bus_routing();
    if (editor && player_config.is_valid() &&
        player_config->has_method(StringName("get_editor_reverb_bus_override_warning"))) {
        Variant warn = player_config->call(StringName("get_editor_reverb_bus_override_warning"), this);
        if (warn.get_type() == Variant::STRING) {
            const String msg = warn;
            if (!msg.is_empty()) {
                UtilityFunctions::push_warning(msg);
            }
        }
    }
}

ResonancePlayer::BusRoutingKey ResonancePlayer::_bus_routing_key_from_config() const {
    if (!player_config.is_valid())
        return {};
    return {_config_int("bus_override", -1), player_config->get(StringName("bus_name")),
            _config_int("reverb_bus_override", -1), player_config->get(StringName("reverb_bus_name"))};
}

void ResonancePlayer::_sync_bus_routing_with_config() {
    if (!(_bus_routing_key_from_config() == bus_routing_key_))
        _apply_player_config_bus_routing();
}

void ResonancePlayer::_apply_player_config_bus_routing() {
    if (!is_inside_tree()) {
        return;
    }
    bus_routing_key_ = _bus_routing_key_from_config();
    SceneTree* tree = get_tree();
    if (!tree) {
        return;
    }
    TypedArray<Node> runtimes = tree->get_nodes_in_group(StringName("resonance_runtime"));
    for (int i = 0; i < runtimes.size(); i++) {
        Node* rt = Object::cast_to<Node>(runtimes[i]);
        if (rt && rt->has_method(StringName("apply_bus_to_player"))) {
            rt->call(StringName("apply_bus_to_player"), this);
            return;
        }
    }
}

PackedVector3Array ResonancePlayer::build_directivity_gizmo_lines(
    bool enabled, int input_mode, float weight, float power, float user_value, float size) {
    PackedVector3Array lines;
    if (size <= 0.0f)
        size = 1.0f;

    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = 2.0f * kPi;
    constexpr int kMeridianSegments = 64;
    constexpr int kRingSegments = 32;

    const bool is_dipole = enabled && input_mode == 0;
    const float w = std::clamp(weight, 0.0f, 1.0f);
    const float p = std::max(0.0f, power);
    const float uv = std::clamp(user_value, 0.0f, 1.0f);

    auto sample = [&](float theta) -> float {
        if (!is_dipole)
            return 1.0f;
        const float c = std::cos(theta);
        const float v = std::fabs((1.0f - w) + w * c);
        if (p == 0.0f)
            return 1.0f;
        return std::pow(v, p);
    };

    auto meridian_xz = [&](float theta) -> Vector3 {
        const float r = sample(theta) * size;
        return Vector3(std::sin(theta) * r, 0.0f, -std::cos(theta) * r);
    };
    auto meridian_yz = [&](float theta) -> Vector3 {
        const float r = sample(theta) * size;
        return Vector3(0.0f, std::sin(theta) * r, -std::cos(theta) * r);
    };

    for (int i = 0; i < kMeridianSegments; ++i) {
        const float t0 = (float)i / kMeridianSegments * kTwoPi;
        const float t1 = (float)(i + 1) / kMeridianSegments * kTwoPi;
        lines.push_back(meridian_xz(t0));
        lines.push_back(meridian_xz(t1));
        lines.push_back(meridian_yz(t0));
        lines.push_back(meridian_yz(t1));
    }

    // Rings at constant theta around the -Z axis.
    float ring_thetas[8];
    int ring_count = 0;
    if (is_dipole) {
        ring_thetas[ring_count++] = kPi / 6.0f;
        ring_thetas[ring_count++] = kPi / 3.0f;
        ring_thetas[ring_count++] = 2.0f * kPi / 3.0f;
        ring_thetas[ring_count++] = 5.0f * kPi / 6.0f;
    } else {
        ring_thetas[ring_count++] = kPi * 0.5f;
    }
    for (int idx = 0; idx < ring_count; ++idx) {
        const float theta = ring_thetas[idx];
        const float r = sample(theta) * size;
        if (r < 1e-4f)
            continue;
        const float ring_radius = std::sin(theta) * r;
        const float z = -std::cos(theta) * r;
        if (ring_radius < 1e-4f)
            continue;
        for (int i = 0; i < kRingSegments; ++i) {
            const float a0 = (float)i / kRingSegments * kTwoPi;
            const float a1 = (float)(i + 1) / kRingSegments * kTwoPi;
            const Vector3 p0(std::cos(a0) * ring_radius, std::sin(a0) * ring_radius, z);
            const Vector3 p1(std::cos(a1) * ring_radius, std::sin(a1) * ring_radius, z);
            lines.push_back(p0);
            lines.push_back(p1);
        }
    }

    // Forward arrow on -Z: the dipole's forward lobe, or scaled by the user value so a near-muted source shows.
    float arrow_len = size;
    if (is_dipole)
        arrow_len = sample(0.0f) * size;
    else if (enabled && input_mode == 1)
        arrow_len = size * std::max(0.05f, uv);
    const Vector3 tip(0.0f, 0.0f, -arrow_len);
    lines.push_back(Vector3(0, 0, 0));
    lines.push_back(tip);
    const float head = size * 0.08f;
    lines.push_back(tip);
    lines.push_back(tip + Vector3(head, 0.0f, head));
    lines.push_back(tip);
    lines.push_back(tip + Vector3(-head, 0.0f, head));
    lines.push_back(tip);
    lines.push_back(tip + Vector3(0.0f, head, head));
    lines.push_back(tip);
    lines.push_back(tip + Vector3(0.0f, -head, head));

    return lines;
}

void ResonancePlayer::set_reverb_send_bus(const StringName& p_bus) {
    reverb_send_bus_ = p_bus;
    _sync_reverb_send_child(ResonanceServer::get_singleton());
}

bool ResonancePlayer::_reverb_send_wanted(const ResonanceServer* srv) const {
    return srv && srv->is_listener_reverb_enabled() && !reverb_send_bus_.is_empty() && config_cache_.reverb_send;
}

void ResonancePlayer::_sync_reverb_send_child(const ResonanceServer* srv) {
    const bool wanted = _reverb_send_wanted(srv);
    Node* child = get_node_or_null(NodePath("ResonanceReverbOutput"));
    AudioStreamPlayer* send_player = child ? Object::cast_to<AudioStreamPlayer>(child) : nullptr;
    reverb_send_active_ = wanted;
    if (!wanted) {
        if (child)
            child->queue_free();
        return;
    }
    if (send_player) {
        send_player->set_bus(reverb_send_bus_);
        return;
    }
    send_player = memnew(AudioStreamPlayer);
    send_player->set_name("ResonanceReverbOutput");
    Ref<ResonanceReverbStream> send_stream;
    send_stream.instantiate();
    send_stream->set_parent_link(reverb_parent_link_);
    send_player->set_stream(send_stream);
    send_player->set_bus(reverb_send_bus_);
    add_child(send_player);
    if (is_playing())
        send_player->play();
}

void ResonancePlayer::_nexus_deferred_spawn_anim_audio_helper() {
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint())
        return;
    if (!convert_anim_audio_runtime_ || !player_config.is_valid())
        return;
    if (get_node_or_null(NodePath("NexusAnimationAudioRuntimeHelper")))
        return;
    Ref<Resource> res;
    if (ResourceLoader* rl = ResourceLoader::get_singleton())
        res = rl->load("res://addons/nexus_resonance/nexus_animation_audio_runtime_helper.gd");
    Ref<Script> script = res;
    if (script.is_null())
        return;
    Node* helper = memnew(Node);
    helper->set_name(String("NexusAnimationAudioRuntimeHelper"));
    helper->set_script(script);
    add_child(helper);
}

bool ResonancePlayer::is_spatial_output_ready() const {
    const ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!srv || !srv->is_initialized())
        return true;
    return source_handle >= 0 && srv->is_source_spatial_output_ready(source_handle);
}

Dictionary ResonancePlayer::get_spawn_timing() const {
    using resonance::SpawnPhase;
    Dictionary d;
    d["voice_dsp_us"] = static_cast<int64_t>(spawn_timing_.total_us(SpawnPhase::VoiceDsp));
    d["reflection_effect_us"] = static_cast<int64_t>(spawn_timing_.total_us(SpawnPhase::ReflectionEffect));
    d["source_us"] = static_cast<int64_t>(spawn_timing_.total_us(SpawnPhase::Source));
    d["params_push_us"] = static_cast<int64_t>(spawn_timing_.total_us(SpawnPhase::ParamsPush));
    return d;
}

// Worst blocks of all voices: longest lap per stage, merged block-time histogram and the audio thread id.
static void add_block_spike_fields(Dictionary& d, const std::vector<ResonanceStreamPlayback*>& playbacks) {
    std::array<uint64_t, resonance::kBlockStageCount> max_stage_ns{};
    std::array<uint64_t, resonance::kBlockTimeBucketCount> histogram{};
    int64_t audio_thread_id = resonance::kUnknownOsThreadId;
    for (const ResonanceStreamPlayback* res_pb : playbacks) {
        if (!res_pb)
            continue;
        const resonance::BlockStageTotals& stages = res_pb->get_block_stage_totals();
        for (size_t i = 0; i < resonance::kBlockStageCount; ++i)
            max_stage_ns[i] = std::max(max_stage_ns[i], stages.max_ns(static_cast<resonance::BlockStage>(i)));
        const resonance::BlockTimeHistogram& block_times = res_pb->get_block_time_histogram();
        for (size_t i = 0; i < resonance::kBlockTimeBucketCount; ++i)
            histogram[i] += block_times.count(i);
        if (audio_thread_id == resonance::kUnknownOsThreadId)
            audio_thread_id = res_pb->get_audio_thread_id();
    }

    for (size_t i = 0; i < resonance::kBlockStageCount; ++i) {
        const char* name = resonance::block_stage_name(static_cast<resonance::BlockStage>(i));
        d[String("max_stage_") + name + "_us"] = resonance::ns_to_us(max_stage_ns[i]);
    }
    PackedInt64Array counts;
    PackedInt64Array upper_us;
    for (size_t i = 0; i < resonance::kBlockTimeBucketCount; ++i) {
        counts.push_back(static_cast<int64_t>(histogram[i]));
        upper_us.push_back(static_cast<int64_t>(resonance::block_time_bucket_upper_us(i)));
    }
    d["block_time_histogram"] = counts;
    d["block_time_histogram_upper_us"] = upper_us;
    d["audio_thread_id"] = audio_thread_id;
}

Dictionary ResonancePlayer::get_audio_instrumentation() {
    Dictionary d;
    if (!player_config.is_valid())
        return d;
    d["godot_attenuation_model"] = (int)get_attenuation_model();
    d["godot_pitch_scale"] = get_pitch_scale();
    d["godot_max_distance"] = get_max_distance();
    d["godot_max_db"] = get_max_db();
    d["godot_volume_db"] = get_volume_db();
    d["godot_unit_size"] = get_unit_size();
    d["effective_volume_linear"] = get_effective_volume_linear_cached();

    std::vector<ResonanceStreamPlayback*> copy;
    internal_copy_internal_playbacks(copy);
    if (copy.empty()) {
        if (ResonanceStreamPlayback* res_pb = _get_resonance_playback())
            copy.push_back(res_pb);
    }
    if (copy.empty())
        return d;

    uint64_t sum_input_dropped = 0, sum_output_underrun = 0, sum_output_blocked = 0, sum_mix_calls = 0, sum_blocks = 0;
    uint64_t sum_passthrough = 0, sum_reverb_miss = 0, max_block_us = 0, max_last_block_us = 0;
    uint64_t sum_late_mix = 0, sum_param_syncs = 0, sum_zero_input = 0;
    uint64_t max_last_mix_gap_us = 0, max_mix_gap_us = 0, max_expected_mix_gap_us = 0;
    int32_t agg_mix_frames_min = std::numeric_limits<int32_t>::max();
    int32_t agg_mix_frames_max = 0;
    uint64_t sum_silent_blocks = 0;
    float max_last_rms = 0.0f;
    float max_path_sh_rms = 0.0f, max_path_sh_energy = 0.0f, max_path_out_rms = 0.0f;
    int32_t max_path_order = -1;
    uint64_t sum_reverb_ring_samples = 0; // Test hook: split-reverb tail residue across all voices.
    uint64_t sum_conv_mixer_null = 0, sum_conv_mix_failed = 0, sum_enable_reverb_false = 0;
    uint64_t sum_block_time_us = 0;
    std::array<uint64_t, resonance::kBlockStageCount> sum_stage_ns{};

    for (ResonanceStreamPlayback* res_pb : copy) {
        if (!res_pb)
            continue;
        uint64_t input_dropped = 0, output_underrun = 0, output_blocked = 0, mix_calls = 0, blocks = 0;
        uint64_t passthrough = 0, reverb_miss = 0, block_us = 0, last_block_us = 0;
        uint64_t late_mix = 0, last_mix_gap_us = 0, max_mix_gap_us_local = 0, expected_mix_gap_us = 0;
        uint64_t param_syncs = 0, zero_input = 0;
        int32_t mix_frames_min = std::numeric_limits<int32_t>::max(), mix_frames_max = 0;
        uint64_t silent_blocks = 0;
        float last_rms = 0.0f;
        float path_sh_rms = 0.0f, path_sh_energy = 0.0f, path_out_rms = 0.0f;
        int32_t path_order = -1;
        uint64_t conv_mixer_null = 0, conv_mix_failed = 0, enable_reverb_false = 0;
        res_pb->get_instrumentation_snapshot(input_dropped, output_underrun, output_blocked, mix_calls, blocks,
                                             passthrough, reverb_miss, block_us, last_block_us, late_mix, last_mix_gap_us, max_mix_gap_us_local,
                                             expected_mix_gap_us, param_syncs, zero_input,
                                             mix_frames_min, mix_frames_max, silent_blocks, last_rms,
                                             path_sh_rms, path_sh_energy, path_out_rms, path_order,
                                             conv_mixer_null, conv_mix_failed, enable_reverb_false);
        sum_input_dropped += input_dropped;
        sum_output_underrun += output_underrun;
        sum_output_blocked += output_blocked;
        sum_mix_calls += mix_calls;
        sum_blocks += blocks;
        sum_passthrough += passthrough;
        sum_reverb_miss += reverb_miss;
        max_block_us = std::max(max_block_us, block_us);
        max_last_block_us = std::max(max_last_block_us, last_block_us);
        sum_late_mix += late_mix;
        sum_param_syncs += param_syncs;
        sum_zero_input += zero_input;
        max_last_mix_gap_us = std::max(max_last_mix_gap_us, last_mix_gap_us);
        max_mix_gap_us = std::max(max_mix_gap_us, max_mix_gap_us_local);
        max_expected_mix_gap_us = std::max(max_expected_mix_gap_us, expected_mix_gap_us);
        if (mix_frames_min < agg_mix_frames_min)
            agg_mix_frames_min = mix_frames_min;
        agg_mix_frames_max = std::max(agg_mix_frames_max, mix_frames_max);
        sum_silent_blocks += silent_blocks;
        max_last_rms = std::max(max_last_rms, last_rms);
        max_path_sh_rms = std::max(max_path_sh_rms, path_sh_rms);
        max_path_sh_energy = std::max(max_path_sh_energy, path_sh_energy);
        max_path_out_rms = std::max(max_path_out_rms, path_out_rms);
        max_path_order = std::max(max_path_order, path_order);
        sum_reverb_ring_samples += (uint64_t)res_pb->get_reverb_ring_available_read();
        sum_conv_mixer_null += conv_mixer_null;
        sum_conv_mix_failed += conv_mix_failed;
        sum_enable_reverb_false += enable_reverb_false;
        sum_block_time_us += res_pb->get_block_time_total_us();
        const resonance::BlockStageTotals& stages = res_pb->get_block_stage_totals();
        for (size_t i = 0; i < resonance::kBlockStageCount; ++i)
            sum_stage_ns[i] += stages.ns(static_cast<resonance::BlockStage>(i));
    }

    if (agg_mix_frames_min == std::numeric_limits<int32_t>::max())
        agg_mix_frames_min = 0;

    d["input_dropped"] = (int64_t)sum_input_dropped;
    d["output_underrun"] = (int64_t)sum_output_underrun;
    d["output_blocked"] = (int64_t)sum_output_blocked;
    d["mix_calls"] = (int64_t)sum_mix_calls;
    d["blocks_processed"] = (int64_t)sum_blocks;
    d["passthrough_blocks"] = (int64_t)sum_passthrough;
    d["reverb_miss_blocks"] = (int64_t)sum_reverb_miss;
    d["max_block_time_us"] = (int64_t)max_block_us;
    d["last_block_time_us"] = (int64_t)max_last_block_us;
    d["block_time_total_us"] = (int64_t)sum_block_time_us;
    d["late_mix_count"] = (int64_t)sum_late_mix;
    d["last_mix_gap_us"] = (int64_t)max_last_mix_gap_us;
    d["max_mix_gap_us"] = (int64_t)max_mix_gap_us;
    d["expected_mix_gap_us"] = (int64_t)max_expected_mix_gap_us;
    d["param_sync_count"] = (int64_t)sum_param_syncs;
    d["zero_input_count"] = (int64_t)sum_zero_input;
    d["mix_frames_min"] = (int)agg_mix_frames_min;
    d["mix_frames_max"] = (int)agg_mix_frames_max;
    d["silent_output_blocks"] = (int64_t)sum_silent_blocks;
    d["last_output_rms"] = max_last_rms;
    d["pathing_sh_rms"] = max_path_sh_rms;
    d["pathing_sh_energy"] = max_path_sh_energy;
    d["pathing_out_rms"] = max_path_out_rms;
    d["pathing_sh_order"] = (int)max_path_order;
    d["polyphony_voice_count"] = (int)copy.size();
    d["reverb_ring_samples"] = (int64_t)sum_reverb_ring_samples;
    d["conv_mixer_null_blocks"] = (int64_t)sum_conv_mixer_null;
    d["conv_mix_failed_blocks"] = (int64_t)sum_conv_mix_failed;
    d["enable_reverb_false_blocks"] = (int64_t)sum_enable_reverb_false;
    for (size_t i = 0; i < resonance::kBlockStageCount; ++i) {
        const char* name = resonance::block_stage_name(static_cast<resonance::BlockStage>(i));
        d[String("stage_") + name + "_us"] = resonance::ns_to_us(sum_stage_ns[i]);
    }
    add_block_spike_fields(d, copy);
    const resonance::DurationTotals& apply_times = shared_reflection_.get_effect_apply_times();
    d["reflection_effect_apply_us"] = resonance::ns_to_us(apply_times.total_ns());
    d["reflection_effect_apply_max_us"] = resonance::ns_to_us(apply_times.max_ns());
    d["reflection_outside_block_us"] = resonance::ns_to_us(instrumentation_reflection_outside_block_ns_.load(std::memory_order_relaxed));
    return d;
}

void ResonancePlayer::reset_audio_instrumentation() {
    std::vector<ResonanceStreamPlayback*> copy;
    internal_copy_internal_playbacks(copy);
    if (copy.empty()) {
        if (ResonanceStreamPlayback* res_pb = _get_resonance_playback())
            copy.push_back(res_pb);
    }
    for (ResonanceStreamPlayback* res_pb : copy) {
        if (res_pb)
            res_pb->reset_instrumentation();
    }
    shared_reflection_.reset_effect_apply_times();
    instrumentation_reflection_outside_block_ns_.store(0, std::memory_order_relaxed);
}

void ResonancePlayer::_nexus_deferred_emit_finished() {
    // A restart since this was queued advanced play_serial_; do not emit for the old run.
    if (!dry_finished_deferred_queued_ || dry_finished_deferred_serial_ != play_serial_)
        return;
    dry_finished_deferred_queued_ = false;
    emit_signal(StringName("finished"));
}