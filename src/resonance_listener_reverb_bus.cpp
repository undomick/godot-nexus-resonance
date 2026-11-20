#include "resonance_listener_reverb_bus.h"
#include "resonance_constants.h"
#include "resonance_server.h"
#include <algorithm>

namespace godot {

namespace {

constexpr float kFullMix = 1.0f;

} // namespace

bool ResonanceListenerReverbBus::initialize(ResonanceServer& srv) {
    cleanup();
    IPLContext ctx = srv.get_context_handle();
    const int frame = srv.get_audio_frame_size();
    if (!ctx || frame <= 0)
        return false;
    if (iplAudioBufferAllocate(ctx, 2, frame, &input_) != IPL_STATUS_SUCCESS)
        return false;
    context_ = ctx;
    frame_size_ = frame;
    reflection_.initialize(ctx, srv.get_sample_rate(), frame, srv.get_ambisonic_order(), srv.get_reflection_type(),
                           srv.get_realtime_simulation_duration(), srv.get_convolution_ir_max_samples());
    decoder_.initialize(ctx, srv.get_sample_rate(), frame, srv.get_ambisonic_order());
    ready_ = decoder_.is_ready();
    return ready_;
}

void ResonanceListenerReverbBus::cleanup() {
    ready_ = false;
    reflection_.cleanup();
    decoder_.cleanup();
    if (context_ && input_.data)
        iplAudioBufferFree(context_, &input_);
    input_ = {};
    context_ = nullptr;
    frame_size_ = 0;
}

void ResonanceListenerReverbBus::apply(ResonanceServer& srv, AudioFrame* dst, int frame_count, IPLReflectionMixer mixer,
                                       const IPLCoordinateSpace3& listener) {
    if (!ready_ || !srv.is_listener_reverb_enabled())
        return;
    const int32_t handle = srv.get_listener_reverb_source_handle();
    if (handle < 0)
        return;
    IPLReflectionEffectParams params{};
    if (!srv.fetch_reverb_params(handle, params))
        return;

    const int type = srv.get_reflection_type();
    const bool feeds_mixer = type == resonance::kReflectionConvolution || type == resonance::kReflectionTan;
    if (feeds_mixer) {
        if (mixer && reflection_.process_mix(input_, params, mixer, kFullMix, kFullMix, kFullMix))
            srv.record_mixer_feed();
        return;
    }
    if (reflection_.process_mix_direct(input_, params, kFullMix, kFullMix))
        _add_direct_wet(srv, dst, frame_count, listener);
}

void ResonanceListenerReverbBus::capture_input(const AudioFrame* src, int frame_count) {
    if (!ready_)
        return;
    const int n = src ? std::min(frame_count, frame_size_) : 0;
    for (int i = 0; i < n; i++) {
        input_.data[0][i] = src[i].left;
        input_.data[1][i] = src[i].right;
    }
    for (int i = n; i < frame_size_; i++) {
        input_.data[0][i] = 0.0f;
        input_.data[1][i] = 0.0f;
    }
}

void ResonanceListenerReverbBus::_add_direct_wet(ResonanceServer& srv, AudioFrame* dst, int frame_count,
                                                 const IPLCoordinateSpace3& listener) {
    IPLAudioBuffer* wet = reflection_.get_direct_output_buffer();
    const int n = std::min(frame_count, frame_size_);
    if (reflection_.is_parametric()) {
        for (int i = 0; i < n; i++) {
            dst[i].left += wet->data[0][i];
            dst[i].right += wet->data[0][i];
        }
        return;
    }
    decoder_.decode_ambisonic_to_stereo(wet, listener, dst, frame_count, srv.use_reverb_binaural());
}

} // namespace godot
