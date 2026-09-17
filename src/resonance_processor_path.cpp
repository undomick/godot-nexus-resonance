#include "resonance_processor_path.h"
#include "resonance_log.h"
#include "resonance_math.h"
#include "resonance_processor_hrtf_policy.h"
#include "resonance_server.h"
#include <algorithm>
#include <cstring>

namespace godot {

ResonancePathProcessor::~ResonancePathProcessor() { cleanup(); }

void ResonancePathProcessor::initialize(IPLContext p_context, int p_sample_rate, int p_frame_size, int p_ambisonic_order) {
    if (init_flags != PathInitFlags::NONE)
        return;
    if (!p_context) {
        ResonanceLog::error("PathProcessor: Context is null!");
        return;
    }

    context = p_context;
    frame_size = p_frame_size;
    sample_rate = p_sample_rate;
    ambisonic_order = p_ambisonic_order;

    ResonanceServer* srv = ResonanceServer::get_singleton();
    IPLHRTF hrtf = (srv && srv->is_initialized()) ? srv->get_hrtf_handle() : nullptr;

    if (!create_path_effect(hrtf)) {
        ResonanceLog::error("PathProcessor: Failed to create IPLPathEffect");
        return;
    }
    init_flags = init_flags | PathInitFlags::PATHEFFECT;

    if (iplAudioBufferAllocate(context, 1, frame_size, &internal_mono_buffer) != IPL_STATUS_SUCCESS ||
        !internal_mono_buffer.data) {
        ResonanceLog::error("PathProcessor: Buffer allocation failed.");
        cleanup();
        return;
    }
    init_flags = init_flags | PathInitFlags::BUFFERS;
    ResonanceLog::info("PathProcessor initialized successfully.");
}

void ResonancePathProcessor::cleanup() {
    if (path_effect) {
        iplPathEffectRelease(&path_effect);
        path_effect = nullptr;
    }
    if (context && internal_mono_buffer.data) {
        iplAudioBufferFree(context, &internal_mono_buffer);
    }
    memset(&internal_mono_buffer, 0, sizeof(internal_mono_buffer));
    bound_hrtf_ = nullptr;
    context = nullptr;
    init_flags = PathInitFlags::NONE;
}

bool ResonancePathProcessor::create_path_effect(IPLHRTF hrtf) {
    if (!context)
        return false;

    IPLPathEffect new_effect = nullptr;
    IPLAudioSettings audioSettings{};
    audioSettings.samplingRate = sample_rate;
    audioSettings.frameSize = frame_size;

    IPLPathEffectSettings pathSettings{};
    pathSettings.maxOrder = ambisonic_order;
    pathSettings.spatialize = IPL_TRUE;
    pathSettings.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
    pathSettings.speakerLayout.numSpeakers = 2;
    pathSettings.hrtf = hrtf;

    if (iplPathEffectCreate(context, &audioSettings, &pathSettings, &new_effect) != IPL_STATUS_SUCCESS)
        return false;

    if (path_effect)
        iplPathEffectRelease(&path_effect);
    path_effect = new_effect;
    bound_hrtf_ = hrtf;
    return true;
}

bool ResonancePathProcessor::hrtf_needs_main_sync(IPLHRTF runtime_hrtf) const {
    const bool has_effect = (init_flags & PathInitFlags::PATHEFFECT) && path_effect != nullptr;
    return resonance::path_needs_hrtf_effect_recreate(has_effect, bound_hrtf_, runtime_hrtf);
}

void ResonancePathProcessor::ensure_hrtf_on_main(IPLHRTF runtime_hrtf) {
    if (!(init_flags & PathInitFlags::PATHEFFECT) || !context)
        return;
    if (!runtime_hrtf)
        return;
    if (!hrtf_needs_main_sync(runtime_hrtf))
        return;
    if (!create_path_effect(runtime_hrtf)) {
        ResonanceLog::error("PathProcessor: Failed to recreate IPLPathEffect for new HRTF");
        if (path_effect) {
            iplPathEffectRelease(&path_effect);
            path_effect = nullptr;
        }
        bound_hrtf_ = nullptr;
        init_flags = static_cast<PathInitFlags>(static_cast<int>(init_flags) & ~static_cast<int>(PathInitFlags::PATHEFFECT));
    }
}

void ResonancePathProcessor::process(const IPLAudioBuffer& in_buffer, const IPLPathEffectParams& params, IPLAudioBuffer& out_buffer,
                                     float path_mix_ramp_start, float path_mix_ramp_end) {
    if (!(init_flags & PathInitFlags::PATHEFFECT) || !(init_flags & PathInitFlags::BUFFERS) || !path_effect || !params.shCoeffs)
        return;
    if (!in_buffer.data || !resonance::path_spatialize_stereo_out_ready(out_buffer, frame_size))
        return;

    // IPL API has non-const param; input is read-only
    iplAudioBufferDownmix(context, const_cast<IPLAudioBuffer*>(&in_buffer), &internal_mono_buffer);
    if (internal_mono_buffer.data && internal_mono_buffer.data[0]) {
        resonance::apply_volume_ramp(path_mix_ramp_start, path_mix_ramp_end, frame_size, internal_mono_buffer.data[0]);
    }

    IPLPathEffectParams effective_params = params;
    for (int i = 0; i < IPL_NUM_BANDS; i++) {
        effective_params.eqCoeffs[i] = std::max(resonance::kPathEQCoeffMin, std::min(resonance::kPathEQCoeffMax, params.eqCoeffs[i]));
    }

    iplPathEffectApply(path_effect, &effective_params, &internal_mono_buffer, &out_buffer);
}

int ResonancePathProcessor::get_tail_size_samples() const {
    if (!(init_flags & PathInitFlags::PATHEFFECT) || !path_effect)
        return 0;
    return iplPathEffectGetTailSize(path_effect);
}

void ResonancePathProcessor::reset_effect() {
    if (path_effect)
        iplPathEffectReset(path_effect);
}

bool ResonancePathProcessor::process_tail(IPLAudioBuffer& out_stereo) {
    if (!(init_flags & PathInitFlags::PATHEFFECT) || !(init_flags & PathInitFlags::BUFFERS) || !path_effect)
        return false;
    if (iplPathEffectGetTailSize(path_effect) <= 0)
        return false;
    if (!resonance::path_spatialize_stereo_out_ready(out_stereo, frame_size))
        return false;
    const IPLAudioEffectState state = iplPathEffectGetTail(path_effect, &out_stereo);
    return state != IPL_AUDIOEFFECTSTATE_TAILCOMPLETE;
}

} // namespace godot
