#include "resonance_audio_effect.h"
#include "resonance_fmod_source_sync_policy.h"
#include "resonance_log.h"
#include "resonance_math.h"
#include "resonance_reflection_mixer_policy.h"
#include "resonance_server.h"
#include "resonance_utils.h"
#include <chrono>
#include <climits>
#include <cstring>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <thread>

using namespace godot;

// Listener pose for simulation (seqlock), reflection mixer swap/teardown, and auto frame-size reinit from the mix callback.

IPLCoordinateSpace3 ResonanceServer::_read_listener_coords_seqlock() const {
    for (;;) {
        const uint32_t s1 = listener_seq_.load(std::memory_order_acquire);
        if (s1 & 1u)
            continue;
        IPLCoordinateSpace3 out = listener_coords_latest_;
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t s2 = listener_seq_.load(std::memory_order_acquire);
        if (s1 == s2)
            return out;
    }
}

IPLCoordinateSpace3 ResonanceServer::get_current_listener_coords() {
    return _read_listener_coords_seqlock();
}

IPLReflectionMixer ResonanceServer::get_reflection_mixer_handle() const {
    return reflection_mixer_.load(std::memory_order_acquire);
}

uint64_t ResonanceServer::get_reflection_mixer_release_deferred_count() const {
    return reflection_mixer_release_deferred_.load(std::memory_order_relaxed);
}

uint64_t ResonanceServer::get_reflection_mixer_release_forced_count() const {
    return get_reflection_mixer_release_deferred_count();
}

void ResonanceServer::_defer_reflection_mixer_release(IPLReflectionMixer mixer) const {
    if (!mixer)
        return;
    _try_release_deferred_reflection_mixers();

    std::lock_guard<std::mutex> lock(deferred_reflection_mixer_releases_mutex_);
    while (resonance::reflection_mixer_defer_should_evict_oldest(deferred_reflection_mixer_releases_.size(),
                                                                 reflection_mixer_readers_.load(std::memory_order_acquire))) {
        IPLReflectionMixer oldest = deferred_reflection_mixer_releases_.front();
        deferred_reflection_mixer_releases_.erase(deferred_reflection_mixer_releases_.begin());
        reflection_mixer_deferred_overflow_.fetch_add(1, std::memory_order_relaxed);
        if (oldest) {
            iplReflectionMixerRelease(&oldest);
            reflection_mixer_deferred_retired_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (resonance::reflection_mixer_defer_exceeds_soft_cap(deferred_reflection_mixer_releases_.size(),
                                                           reflection_mixer_readers_.load(std::memory_order_acquire))) {
        reflection_mixer_deferred_overflow_.fetch_add(1, std::memory_order_relaxed);
        reflection_mixer_deferred_soft_cap_exceeded_.fetch_add(1, std::memory_order_relaxed);
    }
    deferred_reflection_mixer_releases_.push_back(mixer);
    reflection_mixer_deferred_enqueued_.fetch_add(1, std::memory_order_relaxed);
}

void ResonanceServer::_drain_deferred_reflection_mixers_if_pending() const {
    if (!reflection_mixer_deferred_release_pending_.load(std::memory_order_acquire))
        return;
    if (reflection_mixer_readers_.load(std::memory_order_acquire) > 0)
        return;
    reflection_mixer_deferred_release_pending_.store(false, std::memory_order_release);
    _try_release_deferred_reflection_mixers();
}

void ResonanceServer::_try_release_deferred_reflection_mixers() const {
    if (reflection_mixer_readers_.load(std::memory_order_acquire) > 0)
        return;
    std::lock_guard<std::mutex> lock(deferred_reflection_mixer_releases_mutex_);
    if (deferred_reflection_mixer_releases_.empty())
        return;
    size_t retired = 0;
    for (IPLReflectionMixer mixer : deferred_reflection_mixer_releases_) {
        if (!mixer)
            continue;
        IPLReflectionMixer tmp = mixer;
        iplReflectionMixerRelease(&tmp);
        retired++;
    }
    if (retired > 0)
        reflection_mixer_deferred_retired_.fetch_add(retired, std::memory_order_relaxed);
    deferred_reflection_mixer_releases_.clear();
}

void ResonanceServer::_flush_deferred_reflection_mixers_on_shutdown() const {
    // Audio is quiesced and ipl_teardown_active_; never busy-wait 5s on Alt+F4.
    if (reflection_mixer_readers_.load(std::memory_order_acquire) > 0) {
        ResonanceLog::warn_cstr(
            "Reflection mixer flush on shutdown: active readers after audio drain; deferring release (no busy-wait).");
    }
    _try_release_deferred_reflection_mixers();
}

void ResonanceServer::_release_reflection_mixer_when_unused(IPLReflectionMixer mixer) const {
    if (!mixer)
        return;
    _try_release_deferred_reflection_mixers();
    // Reflection mixer swaps are rare (init/reinit/shutdown). Avoid blocking the audio thread; the writer waits.
    // During IPL teardown, skip the 5s reader busy-wait (readers should already be 0 after AudioServer drain).
    const bool teardown = ipl_audio_teardown_active();
    if (!teardown) {
        const auto wait_start = std::chrono::steady_clock::now();
        for (;;) {
            const int readers = reflection_mixer_readers_.load(std::memory_order_acquire);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - wait_start);
            if (resonance::reflection_mixer_release_wait_finished(readers, static_cast<uint64_t>(elapsed.count())))
                break;
            std::this_thread::yield();
        }
    }
    const int readers_after_wait = reflection_mixer_readers_.load(std::memory_order_acquire);
    if (!resonance::reflection_mixer_safe_to_release(readers_after_wait)) {
        reflection_mixer_release_deferred_.fetch_add(1, std::memory_order_relaxed);
        ResonanceLog::warn_cstr(
            "Reflection mixer release deferred: active readers after wait timeout (avoiding UAF).");
        _defer_reflection_mixer_release(mixer);
        return;
    }
    IPLReflectionMixer tmp = mixer;
    iplReflectionMixerRelease(&tmp);
}

void ResonanceServer::_set_reflection_mixer(IPLReflectionMixer new_mixer) {
    IPLReflectionMixer old = reflection_mixer_.exchange(new_mixer, std::memory_order_acq_rel);
    if (old)
        _release_reflection_mixer_when_unused(old);
    _try_release_deferred_reflection_mixers();
    if (new_mixer)
        ResonanceAudioEffectInstance::try_prewarm_all_live_instances();
}

void ResonanceServer::fill_reflection_mixer_apply_params(IPLReflectionEffectParams* p) const {
    if (!p)
        return;
    std::memset(p, 0, sizeof(IPLReflectionEffectParams));
    p->numChannels = get_num_channels_for_order();
    if (reflection_type == resonance::kReflectionTan) {
        p->type = IPL_REFLECTIONEFFECTTYPE_TAN;
        p->tanDevice = _tan();
        p->tanSlot = 0;
    } else {
        p->type = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
        // Mixer-return style params: (type, numChannels, tanDevice) before iplReflectionMixerApply.
        // Passing irSize here is unnecessary and can change behavior across Steam Audio versions.
    }
}

// Map arbitrary mix buffer sizes to the nearest supported IPL frame size (auto frame_size only).
static int snap_to_supported_frame_size(int value) {
    const int supported[] = {256, resonance::kGodotDefaultFrameSize, 1024, resonance::kMaxAudioFrameSize};
    int best = resonance::kGodotDefaultFrameSize;
    int best_dist = INT_MAX;
    for (int s : supported) {
        int d = (value > s) ? (value - s) : (s - value);
        if (d < best_dist) {
            best_dist = d;
            best = s;
        }
    }
    return best;
}
void ResonanceServer::request_reinit_with_frame_size(int detected_frame_count) {
    if (detected_frame_count <= 0)
        return;
    if (!audio_frame_size_was_auto_.load(std::memory_order_acquire))
        return; // Should stay true; host-derived Auto only.
    int snapped = snap_to_supported_frame_size(detected_frame_count);
    if (snapped == frame_size)
        return; // Already at nearest supported; avoid redundant reinit
    int prev = pending_reinit_frame_size_.exchange(snapped, std::memory_order_release);
    (void)prev; // Ignore overwrites; main thread consumes once
}

int ResonanceServer::consume_pending_reinit_frame_size() {
    return pending_reinit_frame_size_.exchange(0, std::memory_order_acq_rel);
}

void ResonanceServer::set_listener_valid(bool valid) {
    pending_listener_valid.store(valid);
}

void ResonanceServer::notify_listener_changed() {
    // Contract: no cached listener node on ResonanceServer. Manual VR/splitscreen drivers must call
    // notify_listener_changed_to(active_listener_node) or update_listener(pos, forward, up) plus set_listener_valid(...)
    // each frame. ResonanceRuntime / ResonanceListener normally publish pose and validity automatically.
}

void ResonanceServer::notify_listener_changed_to(Node* listener_node) {
    if (!listener_node || !_ctx())
        return;
    Node3D* n3d = Object::cast_to<Node3D>(listener_node);
    if (!n3d)
        return;
    Transform3D tr = n3d->get_global_transform();
    Vector3 pos = tr.origin;
    Vector3 forward = -tr.basis.get_column(2);
    Vector3 up = tr.basis.get_column(1);
    update_listener(pos, forward, up);
}

void ResonanceServer::update_listener(Vector3 pos, Vector3 dir, Vector3 up) {
    if (!_ctx())
        return;

    // Orthonormalize basis for safety; use safe_unit_vector to avoid NaN from degenerate transforms
    Vector3 dir_n = ResonanceUtils::safe_unit_vector(dir, Vector3(0, 0, -1));
    Vector3 up_raw = ResonanceUtils::safe_unit_vector(up, Vector3(0, 1, 0));
    Vector3 right_n = ResonanceUtils::safe_unit_vector(dir_n.cross(up_raw), Vector3(1, 0, 0));
    Vector3 up_n = ResonanceUtils::safe_unit_vector(right_n.cross(dir_n), Vector3(0, 1, 0));

    IPLCoordinateSpace3 listener;
    listener.origin = ResonanceUtils::to_ipl_vector3(pos);
    listener.ahead = ResonanceUtils::to_ipl_vector3(dir_n);
    listener.up = ResonanceUtils::to_ipl_vector3(up_n);
    listener.right = ResonanceUtils::to_ipl_vector3(right_n);

    // Seqlock: odd = write in progress; readers spin until even and s1==s2.
    listener_seq_.fetch_add(1, std::memory_order_acq_rel);
    listener_coords_latest_ = listener;
    listener_seq_.fetch_add(1, std::memory_order_release);

    // FMOD Bridge: keep reverb IPLSource in sync with listener. Use try_update_source so the main thread
    // never blocks on simulation_mutex while the worker holds it during RunReflections/RunPathing.
    if (fmod_reverb_source_handle_ >= 0) {
        SourceUpdateParams params;
        params.position = pos;
        params.radius = 1.0f;
        params.source_forward = dir_n;
        params.source_up = up_n;
        if (resonance::fmod_source_sync_should_enqueue_on_try_update_failure(
                try_update_source(fmod_reverb_source_handle_, params)))
            enqueue_source_update(fmod_reverb_source_handle_, params);
    }
}
