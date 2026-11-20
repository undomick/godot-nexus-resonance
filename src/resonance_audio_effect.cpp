#include "resonance_audio_effect.h"
#include "resonance_constants.h"
#include "resonance_log.h"
#include "resonance_math.h"
#include "resonance_server.h"
#include <algorithm>
#include <chrono>
#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <mutex>
#include <vector>

using namespace godot;

static bool s_frame_size_mismatch_warned = false;

namespace {
std::mutex g_bus_fx_mu;
std::vector<ResonanceAudioEffectInstance*> g_bus_fx_live;

void bus_fx_register(ResonanceAudioEffectInstance* inst) {
    if (!inst)
        return;
    std::lock_guard<std::mutex> lock(g_bus_fx_mu);
    for (ResonanceAudioEffectInstance* p : g_bus_fx_live) {
        if (p == inst)
            return;
    }
    g_bus_fx_live.push_back(inst);
}

void bus_fx_unregister(ResonanceAudioEffectInstance* inst) {
    if (!inst)
        return;
    std::lock_guard<std::mutex> lock(g_bus_fx_mu);
    g_bus_fx_live.erase(std::remove(g_bus_fx_live.begin(), g_bus_fx_live.end(), inst), g_bus_fx_live.end());
}
} // namespace

void ResonanceAudioEffectInstance::try_prewarm_all_live_instances() {
    std::vector<ResonanceAudioEffectInstance*> copy;
    {
        std::lock_guard<std::mutex> lock(g_bus_fx_mu);
        copy = g_bus_fx_live;
    }
    for (ResonanceAudioEffectInstance* inst : copy) {
        if (inst)
            inst->try_prewarm_processor();
    }
}

static void write_silence(AudioFrame* dst_buffer, int32_t frame_count) {
    for (int32_t i = 0; i < frame_count; i++) {
        dst_buffer[i].left = 0.0f;
        dst_buffer[i].right = 0.0f;
    }
}

// Called on reinit under the AudioServer lock, before iplContextRelease.
void ResonanceAudioEffectInstance::ipl_context_reinit_cleanup(void* userdata) {
    if (!userdata)
        return;
    static_cast<ResonanceAudioEffectInstance*>(userdata)->_reset_ipl_mixer_for_context_lifecycle();
}

void ResonanceAudioEffectInstance::_reset_ipl_mixer_for_context_lifecycle() {
    if (ResonanceServer* srv = ResonanceServer::get_singleton())
        srv->unregister_ipl_context_client(this);
    processor.cleanup();
    listener_reverb_.cleanup();
    initialized_processor = false;
}

ResonanceAudioEffectInstance::~ResonanceAudioEffectInstance() {
    bus_fx_unregister(this);
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
    if (!listener_reverb_.initialize(*srv))
        ResonanceLog::error("ResonanceAudioEffect: listener reverb init failed. Listener reverb will be silent.");
    srv->register_ipl_context_client(this, &ResonanceAudioEffectInstance::ipl_context_reinit_cleanup);
    initialized_processor = true;
    return true;
}

void ResonanceAudioEffectInstance::_process(const void* src_buffer, AudioFrame* dst_buffer, int32_t frame_count) {
    ResonanceServer* srv = ResonanceServer::get_singleton();

    if (!srv || !srv->is_initialized() || ResonanceServer::ipl_audio_teardown_active()) {
        for (int i = 0; i < frame_count; i++) {
            dst_buffer[i].left = 0.0f;
            dst_buffer[i].right = 0.0f;
        }
        return;
    }

    // The context can exist before the reflection mixer does; that is a normal startup state.
    {
        const int rt_init = srv->get_reflection_type();
        if ((rt_init == resonance::kReflectionConvolution || rt_init == resonance::kReflectionTan) &&
            srv->get_reflection_mixer_handle() == nullptr) {
            write_silence(dst_buffer, frame_count);
            return;
        }
    }

    int server_frame_size = srv->get_audio_frame_size();

    if (!initialized_processor) {
        // IPL objects are created only by try_prewarm_processor on the main thread.
        write_silence(dst_buffer, frame_count);
        return;
    }

    // A mismatched block size can crackle or overrun.
    if (frame_count != server_frame_size) {
        if (srv->get_audio_frame_size_was_auto()) {
            srv->request_reinit_with_frame_size(frame_count);
        }
        if (!s_frame_size_mismatch_warned) {
            s_frame_size_mismatch_warned = true;
            ResonanceLog::warn_cstr(
                "Reverb bus frame_count != server audio_frame_size. Auto reinit will snap to the host mix buffer.");
        }
    }

    if (ResonanceServer::ipl_audio_teardown_active()) {
        for (int i = 0; i < frame_count; i++) {
            dst_buffer[i].left = 0.0f;
            dst_buffer[i].right = 0.0f;
        }
        return;
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
    listener_reverb_.capture_input(static_cast<const AudioFrame*>(src_buffer), frame_count);
    write_silence(dst_buffer, frame_count);

    if (ResonanceServer::ipl_audio_teardown_active()) {
        for (int i = 0; i < frame_count; i++) {
            dst_buffer[i].left = 0.0f;
            dst_buffer[i].right = 0.0f;
        }
        return;
    }

    IPLCoordinateSpace3 listener_coords = srv->get_current_listener_coords();

    // The listener reverb feeds the mixer before the mixer return pulls it.
    const auto bus_t0 = std::chrono::steady_clock::now();
    listener_reverb_.apply(*srv, dst_buffer, frame_count, mixer, listener_coords);
    const int reflection_type = srv->get_reflection_type();
    const bool feeds_mixer =
        reflection_type == resonance::kReflectionConvolution || reflection_type == resonance::kReflectionTan;
    // Parametric and Hybrid wet is already in dst.
    bool success = !feeds_mixer || (mixer && processor.process_mixer_return(mixer, listener_coords, dst_buffer, frame_count));
    const auto bus_t1 = std::chrono::steady_clock::now();
    srv->record_convolution_reverb_bus_usec(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(bus_t1 - bus_t0).count()));

    float peak_pre_gain = 0.0f;
    float rms_pre_gain = 0.0f;
    float peak = 0.0f;
    float rms_post_gain = 0.0f;
    int32_t frames_written = 0;
    if (success) {
        // Never call iplReflectionMixerReset here: it clears state between source and bus Apply and chops the wet.
        frames_written = frame_count;

        float gain = 1.0f;
        if (effect_ref.is_valid()) {
            gain = static_cast<float>(UtilityFunctions::db_to_linear(effect_ref->get_gain_db()));
        }

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

        // No hard clip: the host mix chain limits later.
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

    // A drop from loud to silent within one block clicks; Godot can drop the last fed block against IPL timing.
    // The gap between the thresholds keeps slow decays out.
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

ResonanceAudioEffect::ResonanceAudioEffect() { set_name("Resonance Reverb"); }

Ref<AudioEffectInstance> ResonanceAudioEffect::_instantiate() {
    Ref<ResonanceAudioEffectInstance> ins;
    ins.instantiate();
    ins->set_effect(Ref<ResonanceAudioEffect>(this));
    bus_fx_register(ins.ptr());
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

void godot::detach_resonance_bus_effects() {
    AudioServer* audio = AudioServer::get_singleton();
    if (!audio)
        return;
    for (int bus = 0; bus < audio->get_bus_count(); bus++) {
        for (int idx = audio->get_bus_effect_count(bus) - 1; idx >= 0; idx--) {
            Ref<AudioEffect> effect = audio->get_bus_effect(bus, idx);
            if (effect.is_valid() && Object::cast_to<ResonanceAudioEffect>(effect.ptr()))
                audio->remove_bus_effect(bus, idx);
        }
    }
}
