#include "resonance_player.h"
#include "resonance_constants.h"
#include "resonance_log.h"
#include "resonance_math.h"
#include "resonance_probe_volume.h"
#include "resonance_server.h"
#include "resonance_source_handle_policy.h"
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

using namespace godot;

// ResonancePlayer + ResonanceStreamPlayback: Godot audio I/O, rings, IPL direct/reflection/path pipeline, tails and instrumentation.

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

// ============================================================================
// RESONANCE INTERNAL PLAYBACK
// ============================================================================

ResonanceStreamPlayback::ResonanceStreamPlayback() {
    params_next.apply_air_absorption = false;
    params_next.air_absorption[0] = 1.0f;
    params_next.air_absorption[1] = 1.0f;
    params_next.air_absorption[2] = 1.0f;
    params_next.apply_directivity = false;
    params_next.directivity_value = 1.0f;
    params_next.occlusion = 0.0f;
    params_next.transmission[0] = 1.0f;
    params_next.transmission[1] = 1.0f;
    params_next.transmission[2] = 1.0f;
    params_next.attenuation = 1.0f;
    params_next.listener_orientation.ahead = {0, 0, -1};
    params_next.listener_orientation.up = {0, 1, 0};
    params_next.listener_orientation.right = {1, 0, 0};
    params_next.listener_orientation.origin = {0, 0, 0};
    params_current = params_next;

    input_ring_l.resize(resonance::kRingBufferCapacity);
    input_ring_r.resize(resonance::kRingBufferCapacity);
    output_ring_l.resize(resonance::kRingBufferCapacity);
    output_ring_r.resize(resonance::kRingBufferCapacity);
    output_ring_reverb_l.resize(resonance::kRingBufferCapacity);
    output_ring_reverb_r.resize(resonance::kRingBufferCapacity);

    // Temp buffers resized in _lazy_init to match frame_size_ from ResonanceServer
    temp_process_buffer_l.resize(resonance::kGodotDefaultFrameSize);
    temp_process_buffer_r.resize(resonance::kGodotDefaultFrameSize);

    temp_reverb_buffer_l.resize(resonance::kMaxAudioFrameSize);
    temp_reverb_buffer_r.resize(resonance::kMaxAudioFrameSize);

    // Clean struct init
    memset(&sa_in_buffer, 0, sizeof(IPLAudioBuffer));
    memset(&sa_direct_out_buffer, 0, sizeof(IPLAudioBuffer));
    memset(&sa_final_mix_buffer, 0, sizeof(IPLAudioBuffer));

    parametric_path_sh_coeffs[0] = resonance::kAmbisonicWChannelScale;
    parametric_path_sh_coeffs[1] = parametric_path_sh_coeffs[2] = parametric_path_sh_coeffs[3] = 0.0f;
}

ResonanceStreamPlayback::~ResonanceStreamPlayback() {
    if (owner_player_)
        owner_player_->internal_unregister_playback(this);
    _cleanup_steam_audio();
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
    params_next = p_params;
    params_dirty.store(true, std::memory_order_release);
    // Opens the _mix gate. Before this point the audio thread must not emit samples because
    // params_current still holds defaults (no 3D position, no attenuation, no occlusion).
    params_ever_synced_.store(true, std::memory_order_release);
}

void ResonanceStreamPlayback::_start(double from_pos) {
    input_started = false;
    // Re-arm the first-params gate for the new playback run so restarted players do not leak
    // a full-volume burst with stale or default parameters before the main thread pushes fresh ones.
    params_ever_synced_.store(false, std::memory_order_release);
    // Cancel any in-flight tail-drain from a previous run so the fresh playback starts cleanly.
    stop_requested_.store(false, std::memory_order_release);
    tail_grace_blocks_remaining_.store(-1, std::memory_order_release);
    tail_drain_complete_.store(false, std::memory_order_release);
    // Zero prev mix weights so ramps rebuild from silence on the next blocks.
    prev_direct_weight = 0.0f;
    prev_conv_reflections_mix_level_ = -1.0f;
    prev_parametric_reflections_mix_level_ = 0.0f;
    prev_pathing_mix_level_ = 0.0f;
    reflection_processor.reset_effect();
    path_processor.reset_effect();
    reflection_tail_have_params_ = false;
    conv_reverb_eos_silence_apply_done_ = false;
    memset(&reflection_tail_params_, 0, sizeof(reflection_tail_params_));
    pathing_tail_have_params_ = false;
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
    if (base_playback.is_valid()) {
        base_playback->start(from_pos);
    }
    if (owner_player_)
        owner_player_->internal_register_playback(this);
    // Belt-and-suspenders: base `start` must not leave a prior host fade-out countdown for the first `_mix`.
    playback_host_fade_out_remaining_ = 0;
    // Ordering: `ResonancePlayer::play()` often runs `_push_playback_parameters_from_simulation` on the main
    // thread before this audio-thread `_start` runs. `_start` clears `params_ever_synced_` (burst guard),
    // which would otherwise leave the first-params gate closed until the next `_process` - extra silence
    // vs native `AudioStreamPlayer3D`, worse when `_mix` does not call `mix_audio` while gated. Reschedule
    // the same deferred push used in `play()` so the gate opens on the next main-thread flush without
    // discarding decoder samples at the gate.
    if (owner_player_)
        owner_player_->call_deferred("_deferred_push_playback_parameters");
}
void ResonanceStreamPlayback::_stop() {
    // Soft stop: halt the dry input so mix_audio() returns 0 from now on, but keep the
    // playback alive while the reverb / pathing tail decays. _is_playing() stays true via
    // has_active_tail_residue() until the effect tails are exhausted or the grace block
    // budget runs out. The destructor performs the final unregister + IPL cleanup once Godot
    // detaches the playback (after _is_playing returns false). We intentionally do NOT call
    // direct_processor.reset_for_new_playback() here - that would clear the in-flight tail.
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
    if (reflection_processor.get_tail_size_samples() > 0)
        return true;
    if (path_processor.get_tail_size_samples() > 0)
        return true;
    return false;
}

bool ResonanceStreamPlayback::_is_playing() const {
    // Never report "naturally finished" after tail drain; the parent node stops explicitly.
    if (tail_drain_complete_.load(std::memory_order_acquire))
        return true;
    if (base_playback.is_valid() && base_playback->is_playing())
        return true;
    // Dry signal ended (natural completion or explicit stop). Stay alive while output rings
    // or IPL tails hold residue, or while tail_grace_blocks_remaining_ still budgets tail
    // generation in the samples_read==0 path (grace counts production iterations, not ring depth).
    if (!is_initialized)
        return false;
    if (has_active_tail_residue())
        return true;
    const int64_t grace = tail_grace_blocks_remaining_.load(std::memory_order_acquire);
    if (grace == 0)
        return true;
    // Grace is armed (>= 0) on the first samples_read==0 mix. Until then it stays -1; Godot can
    // query _is_playing() after base EOS but before that callback runs - keep the playback alive
    // so the tail branch executes once.
    if (grace < 0)
        return true;
    // grace > 0: tail-grace budget from the first `samples_read==0` mix (EOS/stop tail). We must keep
    // reporting playing until grace reaches 0 inside `_mix`; otherwise Godot omits further mix callbacks,
    // grace never decrements, and synthetic EOS fades / final ring drains never run - audible hard cut.
    return true;
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
    // Animation TYPE_AUDIO uses AudioStreamPolyphonic. Returning ResonanceStreamPlayback here crashes on Windows
    // before playback starts. Native playback is stable.
    // Steam for TYPE_AUDIO: enable [member ResonancePlayer.convert_anim_audio_runtime] or convert in editor.
    if (base_stream.is_valid() && base_stream->is_class("AudioStreamPolyphonic")) {
        return base_stream->instantiate_playback();
    }
    Ref<ResonanceStreamPlayback> playback;
    playback.instantiate();
    playback->set_owner_player(stream_owner_);
    if (base_stream.is_valid()) {
        playback->set_base_playback(base_stream->instantiate_playback());
    }
    // Prewarm each voice on instantiate so polyphony does not hit lazy IPL alloc on the audio thread.
    playback->prewarm_steam_audio();
    return playback;
}

void ResonanceReverbPlayback::set_parent_player(ResonancePlayer* p_player) {
    parent_player = p_player;
}

int32_t ResonanceReverbPlayback::_mix(AudioFrame* buffer, float _rate_scale, int32_t frames) {
    if (!parent_player || frames <= 0 || !buffer)
        return 0;
    ResonancePlayer::PlaybackVoiceSnapshot snap;
    parent_player->internal_get_playback_snapshot_for_audio(snap);
    size_t total_reverb_samples = 0;
    for (int vi = 0; vi < snap.count; vi++) {
        ResonanceStreamPlayback* pb = snap.voices[static_cast<size_t>(vi)];
        if (pb)
            total_reverb_samples += pb->get_reverb_ring_available_read();
    }
    ResonanceStreamPlayback* fallback_pb = nullptr;
    if (snap.count == 0)
        fallback_pb = Object::cast_to<ResonanceStreamPlayback>(parent_player->get_stream_playback().ptr());
    if (snap.count == 0 && !fallback_pb)
        return 0;
    if (snap.count == 0) {
        if (!parent_player->is_playing() && fallback_pb->get_reverb_ring_available_read() == 0)
            return 0;
        return fallback_pb->read_reverb_frames(buffer, frames);
    }
    if (!parent_player->is_playing() && total_reverb_samples == 0)
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
    if (!parent_player)
        return false;
    if (parent_player->is_playing())
        return true;
    // Parent stopped, but the split-reverb ring may still hold tail samples that have not
    // been drained yet; keep this child alive until the rings are empty so the wet tail
    // reaches the reverb bus instead of being cut off.
    return parent_player->any_internal_playback_has_reverb_ring_data();
}
int ResonanceReverbPlayback::_get_loop_count() const { return 0; }
void ResonanceReverbPlayback::_seek(double _position) {}

void ResonanceReverbStream::set_parent_player(ResonancePlayer* p_player) {
    parent_player = p_player;
}

Ref<AudioStreamPlayback> ResonanceReverbStream::_instantiate_playback() const {
    Ref<ResonanceReverbPlayback> pb;
    pb.instantiate();
    pb->set_parent_player(parent_player);
    return pb;
}

void ResonancePlayer::_enter_tree() {
    set_process_priority(resonance::kResonancePlayerProcessPriority);
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
    // Godot can still apply a linear sphere cutoff when max_distance > 0 even with ATTENUATION_DISABLED.
    // Steam distance lives on ResonancePlayerConfig; clear both so saved ASP3D knobs cannot double-shape the mix.
    set_attenuation_model(AttenuationModel::ATTENUATION_DISABLED);
    set_max_distance(0.0f);
}

void ResonancePlayer::_refresh_effective_volume_cache() {
    const float lin = resonance::effective_asp3d_volume_linear(get_volume_db(), get_max_db());
    cached_effective_volume_linear_.store(lin, std::memory_order_relaxed);
}

void ResonancePlayer::_exit_tree() {
    _clear_physics_ray_auto_exclude_rids();
    // Hard cutoff: soft-stop would race destruction. Base stop only marks FADE_OUT_TO_DELETION;
    // the unref is deferred - use ResonanceRuntime.prepare_for_shutdown() before quitting.
    warned_source_handle_create_failed_ = false;
    playback_lod_have_anchor_ = false;
    playback_lod_time_since_full_ = 0.0;
    coeff_smooth_initialized_ = false;
    coeff_smooth_source_handle_ = -1;
    Node* reverb_child = get_node_or_null(NodePath("ResonanceReverbOutput"));
    if (reverb_child && reverb_child->is_class("AudioStreamPlayer")) {
        if (AudioStreamPlayer* rp = Object::cast_to<AudioStreamPlayer>(reverb_child))
            rp->stop();
    }
    AudioStreamPlayer3D::stop();
    debug_drawer.cleanup();
    directivity_drawer_.cleanup();

    if (source_handle >= 0) {
        _detach_playback_source_retains();
        ResonanceServer* srv = ResonanceServer::get_singleton();
        // Only destroy if this handle still belongs to this player's lifecycle epoch.
        // After reinit, IDs are recycled; destroying a recycled ID would free another player's source.
        if (srv && !ResonanceServer::is_shutting_down() &&
            resonance::source_handle_matches_lifecycle_epoch(source_handle, source_lifecycle_epoch_, srv->get_source_lifecycle_epoch()))
            srv->destroy_source_handle(source_handle);
        source_handle = -1;
        source_lifecycle_epoch_ = 0;
    }
}

ResonancePlayer::~ResonancePlayer() {
    std::vector<ResonanceStreamPlayback*> copy;
    {
        std::lock_guard<std::mutex> lock(internal_playbacks_mutex_);
        copy.swap(internal_playbacks_);
    }
    for (ResonanceStreamPlayback* p : copy) {
        if (p)
            p->internal_orphan_owner_player();
    }
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
    }
    internal_publish_playback_snapshot();
}

void ResonancePlayer::internal_unregister_playback(ResonanceStreamPlayback* p) {
    if (!p)
        return;
    {
        std::lock_guard<std::mutex> lock(internal_playbacks_mutex_);
        internal_playbacks_.erase(std::remove(internal_playbacks_.begin(), internal_playbacks_.end(), p), internal_playbacks_.end());
    }
    internal_publish_playback_snapshot();
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
    for (ResonanceStreamPlayback* pb : copy) {
        if (pb)
            pb->update_parameters(p);
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

void ResonancePlayer::_notification(int p_what) {
    if (p_what == NOTIFICATION_ENTER_TREE || p_what == NOTIFICATION_CHILD_ORDER_CHANGED) {
        Engine* eng = Engine::get_singleton();
        if (!eng || !eng->is_editor_hint())
            _sync_physics_ray_auto_exclude_rids();
    }
    if (p_what == NOTIFICATION_ENTER_TREE) {
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

void ResonancePlayer::_process(double delta) {
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint())
        return;
    if (!player_config.is_valid())
        return;

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

    // Update audio-thread readable snapshot values.
    // Volume is source loudness before Steam DSP (see owner_effective_volume_linear / effective_asp3d_volume_linear).
    _refresh_effective_volume_cache();

    ResonanceServer* srv = ResonanceServer::get_singleton();
    // `finished` should fire exactly once when the dry/base playback ends.
    // Wet/pathing tails may keep running; we stop the node explicitly once tail drain completes
    // so Godot does not emit `finished` again at the wet/tail end.
    if (is_playing()) {
        std::vector<ResonanceStreamPlayback*> voices;
        internal_copy_internal_playbacks(voices);
        if (voices.empty()) {
            if (ResonanceStreamPlayback* res_pb = _get_resonance_playback())
                voices.push_back(res_pb);
        }

        bool any_dry_playing = false;
        bool any_soft_stopped = false;
        bool all_tail_drained = !voices.empty();
        for (ResonanceStreamPlayback* pb : voices) {
            if (!pb) {
                all_tail_drained = false;
                continue;
            }
            if (pb->base_playback.is_valid() && pb->base_playback->is_playing())
                any_dry_playing = true;
            if (pb->stop_requested_.load(std::memory_order_acquire))
                any_soft_stopped = true;
            if (!pb->is_tail_drain_complete())
                all_tail_drained = false;
        }

        const bool dry_done_natural = !voices.empty() && !any_dry_playing && !any_soft_stopped;
        if (dry_done_natural && !dry_finished_emitted_) {
            dry_finished_emitted_ = true;
            // Defer to avoid re-entrancy: user code may call play() inside `finished`,
            // while the engine is still mid-frame / mid-audio bookkeeping.
            if (!dry_finished_deferred_queued_) {
                dry_finished_deferred_queued_ = true;
                dry_finished_deferred_serial_ = play_serial_;
                call_deferred("_nexus_deferred_emit_finished");
            }
        }

        // Natural EOS: stop after finished was emitted and wet/path tails drained.
        // Soft-stop: stop as soon as every voice reports tail_drain_complete (no finished emit).
        if (all_tail_drained && (dry_finished_emitted_ || any_soft_stopped)) {
            soft_stop_elapsed_sec_ = -1.0;
            AudioStreamPlayer3D::stop();
            return;
        }
        // Watchdog: Dummy audio / stalled mix may never set tail_drain_complete. Cap wait at
        // max reverb duration (+ margin) so stop() still clears playing.
        if (any_soft_stopped && soft_stop_elapsed_sec_ >= 0.0) {
            soft_stop_elapsed_sec_ += delta;
            float max_rev = resonance::kDefaultReverbDurationSec;
            if (srv)
                max_rev = srv->get_max_reverb_duration();
            if (soft_stop_elapsed_sec_ >= static_cast<double>(max_rev) + 0.5) {
                soft_stop_elapsed_sec_ = -1.0;
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
    const bool dbg_occ = srv && srv->is_debug_occlusion_enabled();
    const bool dbg_ref = srv && srv->is_debug_reflections_enabled();
    const bool want_player_debug_ui = !exclude_from_debug_ && (dbg_occ || dbg_ref);
    const bool pipeline_ok = is_playing() && srv && srv->is_simulating() && source_handle >= 0;

    if (want_player_debug_ui) {
        if (pipeline_ok)
            debug_overlay_grace_timer_ = resonance::kDebugOverlayGraceSeconds;
        else
            debug_overlay_grace_timer_ -= delta;
    } else {
        debug_overlay_grace_timer_ = 0.0;
    }

    const bool show_debug_hud = want_player_debug_ui && (pipeline_ok || debug_overlay_grace_timer_ > 0.0);

    if (!is_playing()) {
        if (show_debug_hud && debug_overlay_has_last_data_)
            _sync_player_debug_drawer(delta, srv, debug_overlay_last_data_, true);
        else
            _sync_player_debug_drawer(delta, srv, ResonanceDebugData{}, false);
        return;
    }

    if (srv)
        _invalidate_source_handle_if_stale(srv);

    if (srv && source_handle < 0 && srv->is_initialized())
        _try_ensure_source_and_sync(srv, true);

    if (!srv || !srv->is_simulating() || source_handle < 0) {
        if (show_debug_hud && debug_overlay_has_last_data_)
            _sync_player_debug_drawer(delta, srv, debug_overlay_last_data_, true);
        else
            _sync_player_debug_drawer(delta, srv, ResonanceDebugData{}, false);
        return;
    }

    _prepare_source_for_simulation(srv);
    _ensure_config_valid();
    const bool coeff_smooth_active = (config_cache_.playback_coeff_smoothing_time > 0.0f) &&
                                     ((config_cache_.occlusion_input == 0) || (config_cache_.transmission_input == 0));
    const bool apply_playback = _playback_lod_should_apply_playback_params(delta, show_debug_hud, get_global_position()) || coeff_smooth_active;

    ResonanceDebugData dbg_data;
    if (apply_playback)
        _apply_playback_params_from_simulation(srv, &dbg_data, delta);
    else
        dbg_data = debug_overlay_last_data_;

    // --- DEBUG DRAWING ---
    _aggregate_debug_signal_levels(dbg_data.signal_direct, dbg_data.signal_reverb, dbg_data.signal_pathing);

    // Convolution reflections are mixed on the global reverb bus; the per-playback "reverb signal" is only a feed/send scalar.
    // For debugging loudness mismatches, show bus output RMS (pre bus gain) instead.
    if (srv && dbg_ref && srv->get_reflection_type() == resonance::kReflectionConvolution) {
        const float bus_rms_pre_gain = srv->get_reverb_bus_output_rms_pre_gain();
        dbg_data.signal_reverb = std::clamp(resonance::sanitize_audio_float(bus_rms_pre_gain), 0.0f, 1.0f);
    }

    if (apply_playback) {
        debug_overlay_last_data_ = dbg_data;
        debug_overlay_has_last_data_ = true;
    }
    _sync_player_debug_drawer(delta, srv, dbg_data, show_debug_hud);
}

bool ResonancePlayer::_try_ensure_source_and_sync(ResonanceServer* srv, bool deferred_playback_push_if_playing) {
    _invalidate_source_handle_if_stale(srv);
    if (!player_config.is_valid() || source_handle >= 0)
        return false;
    if (!srv || !srv->is_initialized())
        return false;

    const float eff_radius = _config_float("source_radius", 1.0f);
    const int32_t h = srv->create_source_handle(get_global_position(), eff_radius);
    if (h < 0) {
        if (!warned_source_handle_create_failed_) {
            warned_source_handle_create_failed_ = true;
            ResonanceLog::warn(
                "ResonancePlayer: create_source_handle failed (is the simulator ready?). Reverb/occlusion may stay dry until it succeeds.");
        }
        return false;
    }
    warned_source_handle_create_failed_ = false;
    source_handle = h;
    source_lifecycle_epoch_ = srv->get_source_lifecycle_epoch();
    _prepare_source_for_simulation(srv);
    if (deferred_playback_push_if_playing && is_playing())
        call_deferred("_deferred_push_playback_parameters");
    return true;
}

void ResonancePlayer::_invalidate_source_handle_if_stale(ResonanceServer* srv) {
    if (source_handle < 0)
        return;
    const uint32_t server_epoch = srv ? srv->get_source_lifecycle_epoch() : 0u;
    if (resonance::source_handle_matches_lifecycle_epoch(source_handle, source_lifecycle_epoch_, server_epoch))
        return;
    _detach_playback_source_retains();
    source_handle = -1;
    source_lifecycle_epoch_ = 0;
}

void ResonancePlayer::reload_source_after_reinit() {
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint())
        return;
    if (!player_config.is_valid())
        return;
    _detach_playback_source_retains();
    source_handle = -1;
    source_lifecycle_epoch_ = 0;
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

void ResonancePlayer::_start_reverb_split_child_if_needed() {
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
        return;
    }
    play_serial_++;
    dry_finished_emitted_ = false;
    dry_finished_deferred_queued_ = false;
    dry_finished_deferred_serial_ = 0;
    soft_stop_elapsed_sec_ = -1.0;
    if (player_config.is_valid()) {
        playback_lod_have_anchor_ = false;
        playback_lod_time_since_full_ = 0.0;
        _update_stream_setup();
        ResonanceServer* srv = ResonanceServer::get_singleton();
        _try_ensure_source_and_sync(srv, false);
    }
    AudioStreamPlayer3D::play(from_position);
    if (player_config.is_valid()) {
        _refresh_effective_volume_cache();
        _start_reverb_split_child_if_needed();
        // Synchronously seed the freshly instantiated ResonanceStreamPlayback(s) with valid
        // spatial parameters so the audio thread's first _mix block opens the params gate
        // immediately and mixes from the source's real world position / attenuation instead
        // of waiting a frame for the deferred broadcast. _try_ensure_source_and_sync above
        // has already ensured source_handle >= 0 when possible; _push_playback_parameters_
        // _from_simulation tolerates empty simulation caches (defaults to occlusion=1 / no
        // reverb) which still produces correct 3D positioning.
        ResonanceServer* srv = ResonanceServer::get_singleton();
        if (srv && srv->is_simulating() && source_handle >= 0)
            _push_playback_parameters_from_simulation(srv, nullptr, 0.0);
        // Keep the deferred push as safety net in case source_handle was not yet available
        // this tick (worker still spinning up, late attach, etc.).
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
    // GDExtension may narrow to float internally; keep double at call site.
    // NOLINTNEXTLINE(bugprone-narrowing-conversions)
    play(static_cast<float>(from_pos));
}

void ResonancePlayer::play_animation_audio_clip(const Ref<AudioStream>& p_stream, float from_position) {
    set_stream(p_stream);
    play(from_position);
}

void ResonancePlayer::stop() {
    warned_source_handle_create_failed_ = false;
    playback_lod_have_anchor_ = false;
    playback_lod_time_since_full_ = 0.0;
    coeff_smooth_initialized_ = false;
    coeff_smooth_source_handle_ = -1;

    Engine* eng = Engine::get_singleton();
    const bool editor = eng && eng->is_editor_hint();

    // Editor or fallback (no player_config / non-Resonance playback): use the original
    // hard-cutoff behaviour to match plain AudioStreamPlayer3D semantics.
    if (editor || !player_config.is_valid()) {
        Node* reverb_child = get_node_or_null(NodePath("ResonanceReverbOutput"));
        if (reverb_child && reverb_child->is_class("AudioStreamPlayer")) {
            if (AudioStreamPlayer* rp = Object::cast_to<AudioStreamPlayer>(reverb_child))
                rp->stop();
        }
        AudioStreamPlayer3D::stop();
        return;
    }

    // Soft-stop: halt the dry input on each active ResonanceStreamPlayback voice but keep
    // the playbacks themselves alive while the reverb / pathing tail decays. We deliberately
    // do NOT call AudioStreamPlayer3D::stop() so Godot's audio engine keeps invoking _mix
    // on each voice while ResonanceStreamPlayback::_is_playing() returns true (which it does
    // while effect tails or output rings still hold residue, capped by max_reverb_duration).
    // Once _is_playing() finally returns false the AudioServer detaches the playback.
    // The reverb-split child is left running too; ResonanceReverbPlayback::_is_playing()
    // returns false on its own once both the parent and the split-reverb rings are empty.
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
    } else {
        // No active ResonanceStreamPlayback voices (e.g. AudioStreamPolyphonic via runtime
        // animation conversion, or the player was never played). Fall back to the plain
        // hard-cutoff path so callers see the expected stop() semantics immediately.
        soft_stop_elapsed_sec_ = -1.0;
        Node* reverb_child = get_node_or_null(NodePath("ResonanceReverbOutput"));
        if (reverb_child && reverb_child->is_class("AudioStreamPlayer")) {
            if (AudioStreamPlayer* rp = Object::cast_to<AudioStreamPlayer>(reverb_child))
                rp->stop();
        }
        AudioStreamPlayer3D::stop();
    }
}

void ResonancePlayer::set_pathing_probe_volume(const NodePath& p_path) { pathing_probe_volume = p_path; }
NodePath ResonancePlayer::get_pathing_probe_volume() const { return pathing_probe_volume; }

void ResonancePlayer::clear_pathing_probe_immediate() {
    pathing_probe_volume = NodePath();
    ResonanceServer* srv = ResonanceServer::get_singleton();
    _invalidate_source_handle_if_stale(srv);
    if (!srv || !srv->is_initialized() || source_handle < 0)
        return;
    _ensure_config_and_apply_source(-1);
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
    // Toggling between plain and config-driven mode changes which inherited 3D knobs are inert,
    // so refresh the inspector property list (see _validate_property).
    if (had_config != player_config.is_valid())
        notify_property_list_changed();
    _on_player_config_changed_refresh_gizmo();
}
Ref<Resource> ResonancePlayer::get_player_config() const { return player_config; }

void ResonancePlayer::_validate_property(PropertyInfo& p_property) const {
    // With a player_config the spatialization runs through Steam Audio: distance/attenuation come
    // from the config; _apply_steam_mode_asp3d_guards forces ATTENUATION_DISABLED and clears Godot
    // max_distance. Inherited ASP3D distance/rolloff/filter knobs are then inert - hide them
    // (Godot 4.6 ASP3D spatial set). Without a config the node is plain AudioStreamPlayer3D.
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
    directivity_drawer_.mark_dirty();
    update_gizmos();
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

    // Azimuth rings (circles in the XY plane at constant theta; axis-symmetry around -Z).
    float ring_thetas[8];
    int ring_count = 0;
    if (is_dipole) {
        ring_thetas[ring_count++] = kPi / 6.0f;
        ring_thetas[ring_count++] = kPi / 3.0f;
        ring_thetas[ring_count++] = 2.0f * kPi / 3.0f;
        ring_thetas[ring_count++] = 5.0f * kPi / 6.0f;
    } else {
        ring_thetas[ring_count++] = kPi * 0.5f; // equator only
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

    // Forward arrow on -Z: for dipole the length matches the forward lobe, for user-defined we scale
    // by the directivity_value so a near-muted scalar shows visually.
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

void ResonancePlayer::set_reverb_split_output(bool p_enable, const StringName& p_reverb_bus) {
    if (reverb_split_output_ != p_enable) {
        reverb_split_output_ = p_enable;
        _update_reverb_split_child(p_reverb_bus);
    } else if (reverb_split_output_ && !p_reverb_bus.is_empty()) {
        Node* child = get_node_or_null(NodePath("ResonanceReverbOutput"));
        if (child && child->is_class("AudioStreamPlayer")) {
            if (AudioStreamPlayer* rp = Object::cast_to<AudioStreamPlayer>(child))
                rp->set_bus(p_reverb_bus);
        }
    }
}

void ResonancePlayer::_update_reverb_split_child(const StringName& p_reverb_bus) {
    const NodePath child_path = NodePath("ResonanceReverbOutput");
    Node* child = get_node_or_null(child_path);
    if (reverb_split_output_) {
        if (!child) {
            AudioStreamPlayer* reverb_player = memnew(AudioStreamPlayer);
            reverb_player->set_name("ResonanceReverbOutput");
            Ref<ResonanceReverbStream> reverb_stream;
            reverb_stream.instantiate();
            reverb_stream->set_parent_player(this);
            reverb_player->set_stream(reverb_stream);
            if (!p_reverb_bus.is_empty())
                reverb_player->set_bus(p_reverb_bus);
            add_child(reverb_player);
            if (is_playing())
                reverb_player->play();
        } else if (!p_reverb_bus.is_empty() && child->is_class("AudioStreamPlayer")) {
            if (AudioStreamPlayer* rp = Object::cast_to<AudioStreamPlayer>(child))
                rp->set_bus(p_reverb_bus);
        }
    } else if (child) {
        child->queue_free();
    }
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
        // Debugging signal completion: whether the playback ever returns 0 (EOS) and how long it stayed gated.
        // (Aggregated like the other counters; only surfaced in the dictionary below.)
        // Reuse sum_* locals for aggregation.
        // NOTE: We intentionally do not cap these; they are for troubleshooting.
        // (Declared above as sum_* variables.)
        // --- aggregation ---
        // (see vars declared at top of function)
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
}

void ResonancePlayer::_nexus_deferred_emit_finished() {
    // Emit only for the playback run that queued this callback. If user code restarted
    // immediately (e.g. in the finished handler), play_serial_ has advanced and we must
    // not emit for the previous run.
    if (!dry_finished_deferred_queued_ || dry_finished_deferred_serial_ != play_serial_)
        return;
    dry_finished_deferred_queued_ = false;
    emit_signal(StringName("finished"));
}