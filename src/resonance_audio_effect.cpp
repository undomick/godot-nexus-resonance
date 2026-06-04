#include "resonance_audio_effect.h"
#include "resonance_constants.h"
#include "resonance_log.h"
#include "resonance_math.h"
#include "resonance_server.h"
#include <algorithm>
#include <chrono>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

// One warning if Godot’s `frame_count` != server `audio_frame_size` (avoids log spam).
static bool s_frame_size_mismatch_warned = false;

// Copy prior bus chain into `dst` (IPL mix_return: dry/chain first, then add wet). `src` may alias `dst` or be null (silence).
static void copy_bus_input_to_dst(const void* src_buffer, AudioFrame* dst_buffer, int32_t frame_count) {
    if (!dst_buffer || frame_count <= 0)
        return;
    if (src_buffer == static_cast<const void*>(dst_buffer)) {
        return;
    }
    if (src_buffer) {
        const AudioFrame* src = static_cast<const AudioFrame*>(src_buffer);
        for (int32_t i = 0; i < frame_count; i++) {
            dst_buffer[i] = src[i];
        }
        return;
    }
    for (int32_t i = 0; i < frame_count; i++) {
        dst_buffer[i].left = 0.0f;
        dst_buffer[i].right = 0.0f;
    }
}

// Reinit: server unregisters this client under `AudioServer` lock before `iplContextRelease`.

void ResonanceAudioEffectInstance::ipl_context_reinit_cleanup(void* userdata) {
    if (!userdata)
        return;
    static_cast<ResonanceAudioEffectInstance*>(userdata)->_reset_ipl_mixer_for_context_lifecycle();
}

void ResonanceAudioEffectInstance::_reset_ipl_mixer_for_context_lifecycle() {
    if (ResonanceServer* srv = ResonanceServer::get_singleton())
        srv->unregister_ipl_context_client(this);
    processor.cleanup();
    initialized_processor = false;
}

ResonanceAudioEffectInstance::~ResonanceAudioEffectInstance() {
    _reset_ipl_mixer_for_context_lifecycle();
}

bool ResonanceAudioEffectInstance::try_prewarm_processor() {
    if (initialized_processor)
        return true;
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!srv || !srv->is_initialized() || ResonanceServer::ipl_audio_teardown_active())
        return false;

    const int rt = srv->get_reflection_type();
    if ((rt == resonance::kReflectionConvolution || rt == resonance::kReflectionTan) &&
        srv->get_reflection_mixer_handle() == nullptr)
        return false;

    const int server_frame_size = srv->get_audio_frame_size();
    ResonanceLog::info("AudioEffect: Initializing MixerProcessor with frame size: " + String::num(server_frame_size));
    processor.initialize(
        srv->get_context_handle(),
        srv->get_sample_rate(),
        server_frame_size,
        srv->get_ambisonic_order());
    if (!processor.is_ready()) {
        ResonanceLog::error("ResonanceAudioEffect: MixerProcessor initialization failed. Reverb will be silent until init succeeds.");
        return false;
    }
    srv->register_ipl_context_client(this, &ResonanceAudioEffectInstance::ipl_context_reinit_cleanup);
    initialized_processor = true;
    return true;
}

void ResonanceAudioEffectInstance::_process(const void* src_buffer, AudioFrame* dst_buffer, int32_t frame_count) {
    ResonanceServer* srv = ResonanceServer::get_singleton();

    // Exit, teardown, or pre-mixer state: output silence (recheck before heavy work).
    if (!srv || !srv->is_initialized() || ResonanceServer::ipl_audio_teardown_active()) {
        for (int i = 0; i < frame_count; i++) {
            dst_buffer[i].left = 0.0f;
            dst_buffer[i].right = 0.0f;
        }
        return;
    }

    // Context exists before reflection mixer is created - passthrough chain only (not an error path).
    {
        const int rt_init = srv->get_reflection_type();
        if ((rt_init == resonance::kReflectionConvolution || rt_init == resonance::kReflectionTan) &&
            srv->get_reflection_mixer_handle() == nullptr) {
            copy_bus_input_to_dst(src_buffer, dst_buffer, frame_count);
            return;
        }
    }

    int server_frame_size = srv->get_audio_frame_size();

    if (!initialized_processor && !try_prewarm_processor()) {
        for (int i = 0; i < frame_count; i++) {
            dst_buffer[i].left = 0.0f;
            dst_buffer[i].right = 0.0f;
        }
        return;
    }

    // Mismatch → possible crackle/overrun; Auto frame size can request reinit.
    if (frame_count != server_frame_size) {
        if (srv->get_audio_frame_size_was_auto()) {
            srv->request_reinit_with_frame_size(frame_count);
        }
        if (!s_frame_size_mismatch_warned) {
            s_frame_size_mismatch_warned = true;
            ResonanceLog::warn("Reverb bus frame_count (" + String::num_int64(frame_count) + ") != audio_frame_size (" +
                               String::num_int64(server_frame_size) +
                               "). Set ResonanceRuntimeConfig.audio_frame_size to Auto (0) to derive from Project Settings, or match manually.");
        }
    }

    auto mixer_guard = srv->scoped_mixer_read();
    IPLReflectionMixer mixer = mixer_guard.get();
    if (ResonanceServer::ipl_audio_teardown_active()) {
        for (int i = 0; i < frame_count; i++) {
            dst_buffer[i].left = 0.0f;
            dst_buffer[i].right = 0.0f;
        }
        return;
    }
    // Parametric/Hybrid: no shared mixer (wet per source). Null mixer here: rare ordering/teardown - passthrough only.
    if (!mixer) {
        copy_bus_input_to_dst(src_buffer, dst_buffer, frame_count);
        srv->update_reverb_effect_instrumentation(false, true, frame_count, 0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    copy_bus_input_to_dst(src_buffer, dst_buffer, frame_count);

    if (ResonanceServer::ipl_audio_teardown_active()) {
        for (int i = 0; i < frame_count; i++) {
            dst_buffer[i].left = 0.0f;
            dst_buffer[i].right = 0.0f;
        }
        return;
    }

    IPLCoordinateSpace3 listener_coords = srv->get_current_listener_coords();

    // One pull: `iplReflectionMixerApply` + decode; hold-last in `MixerProcessor` if no source fed this tick.
    const auto bus_t0 = std::chrono::steady_clock::now();
    bool success = processor.process_mixer_return(mixer, listener_coords, dst_buffer, frame_count);
    const auto bus_t1 = std::chrono::steady_clock::now();
    srv->record_convolution_reverb_bus_usec(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(bus_t1 - bus_t0).count()));

    float peak_pre_gain = 0.0f;
    float rms_pre_gain = 0.0f;
    float peak = 0.0f;
    float rms_post_gain = 0.0f;
    int32_t frames_written = 0;
    if (success) {
        // Never `iplReflectionMixerReset` here - would clear state between source Apply and bus Apply (choppy wet).
        frames_written = frame_count;

        float gain = 1.0f;
        if (effect_ref.is_valid()) {
            gain = static_cast<float>(UtilityFunctions::db_to_linear(effect_ref->get_gain_db()));
        }

        // Pre-gain RMS/peak (decoded wet as written by MixerProcessor; bus input was already copied into dst_buffer).
        double sum_sq_pre = 0.0;
        for (int i = 0; i < frame_count; i++) {
            const float l = resonance::sanitize_audio_float(dst_buffer[i].left);
            const float r = resonance::sanitize_audio_float(dst_buffer[i].right);
            const float v = std::max(std::abs(l), std::abs(r));
            if (v > peak_pre_gain)
                peak_pre_gain = v;
            sum_sq_pre += static_cast<double>(l) * static_cast<double>(l);
            sum_sq_pre += static_cast<double>(r) * static_cast<double>(r);
        }
        if (frame_count > 0) {
            const double mean_sq = sum_sq_pre / static_cast<double>(frame_count * 2);
            rms_pre_gain = static_cast<float>(std::sqrt(std::max(0.0, mean_sq)));
        }

        // Gain + sanitize. (No hard clip: Unity-style mixer chains typically clip/limit later in the engine.)
        double sum_sq_post = 0.0;
        for (int i = 0; i < frame_count; i++) {
            const float left = resonance::sanitize_audio_float(dst_buffer[i].left * gain);
            const float right = resonance::sanitize_audio_float(dst_buffer[i].right * gain);
            dst_buffer[i].left = left;
            dst_buffer[i].right = right;
            float v = std::max(std::abs(dst_buffer[i].left), std::abs(dst_buffer[i].right));
            if (v > peak)
                peak = v;
            sum_sq_post += static_cast<double>(dst_buffer[i].left) * static_cast<double>(dst_buffer[i].left);
            sum_sq_post += static_cast<double>(dst_buffer[i].right) * static_cast<double>(dst_buffer[i].right);
        }
        if (frame_count > 0) {
            const double mean_sq = sum_sq_post / static_cast<double>(frame_count * 2);
            rms_post_gain = static_cast<float>(std::sqrt(std::max(0.0, mean_sq)));
        }
    }

    // Loud → ~silent in one block: linear fade out (Godot can drop the last fed block vs. IPL timing). Ignore slow decay (threshold gap).
    const float prev_loud_thr = 1.0e-2f;
    const float now_silent_thr = 1.0e-7f;
    if (frames_written > 0 && prev_peak_ >= prev_loud_thr && peak < now_silent_thr) {
        srv->record_reverb_effect_click_guard_trigger();
        if (srv->is_reverb_bus_click_guard_enabled()) {
            for (int i = 0; i < frame_count; i++) {
                const float fade = 1.0f - (float(i + 1) / float(frame_count));
                dst_buffer[i].left *= fade;
                dst_buffer[i].right *= fade;
            }
            peak = 0.0f;
            for (int i = 0; i < frame_count; i++) {
                const float v = std::max(std::abs(dst_buffer[i].left), std::abs(dst_buffer[i].right));
                if (v > peak)
                    peak = v;
            }
        }
    }
    prev_peak_ = peak;

    srv->update_reverb_effect_instrumentation(false, success, frames_written, peak, rms_post_gain, peak_pre_gain, rms_pre_gain);
}

ResonanceAudioEffect::ResonanceAudioEffect() { set_name("Resonance Reverb"); } // Audio bus strip label

Ref<AudioEffectInstance> ResonanceAudioEffect::_instantiate() {
    Ref<ResonanceAudioEffectInstance> ins;
    ins.instantiate();
    ins->set_effect(Ref<ResonanceAudioEffect>(this));
    ins->try_prewarm_processor();
    return ins;
}

void ResonanceAudioEffect::set_gain_db(float p_db) { gain_db = p_db; }
float ResonanceAudioEffect::get_gain_db() const { return gain_db; }

void ResonanceAudioEffect::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_gain_db", "p_db"), &ResonanceAudioEffect::set_gain_db);
    ClassDB::bind_method(D_METHOD("get_gain_db"), &ResonanceAudioEffect::get_gain_db);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gain_db", PROPERTY_HINT_RANGE, "-60,24,0.1,suffix:dB"), "set_gain_db", "get_gain_db");
}