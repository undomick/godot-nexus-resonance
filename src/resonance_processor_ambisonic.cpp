#include "resonance_processor_ambisonic.h"
#include "resonance_ambisonic_tail_policy.h"
#include "resonance_log.h"
#include "resonance_server.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

namespace godot {

namespace {

IPLCoordinateSpace3 identity_orientation() {
    IPLCoordinateSpace3 u{};
    u.origin = {0.0f, 0.0f, 0.0f};
    u.ahead = {0.0f, 0.0f, -1.0f};
    u.up = {0.0f, 1.0f, 0.0f};
    u.right = {1.0f, 0.0f, 0.0f};
    return u;
}

void apply_ambisonic_decoder_output_gain(bool apply_gain, int frame_size, IPLAudioBuffer& out_buffer) {
    if (!apply_gain || !out_buffer.data || !out_buffer.data[0] || !out_buffer.data[1])
        return;
    const float s = resonance::kAmbisonicDecoderOutputScalar;
    for (int i = 0; i < frame_size; i++) {
        out_buffer.data[0][i] *= s;
        out_buffer.data[1][i] *= s;
    }
}

} // namespace

ResonanceAmbisonicProcessor::~ResonanceAmbisonicProcessor() {
    cleanup();
}

bool ResonanceAmbisonicProcessor::matches_config(int p_order, bool p_use_rotation_effect, bool p_apply_hrtf,
                                                 bool p_input_is_sn3d, bool p_apply_output_gain) const {
    return ambisonic_order == p_order && use_ip_ambisonics_rotation_effect == p_use_rotation_effect &&
           apply_hrtf == p_apply_hrtf && input_is_sn3d == p_input_is_sn3d && apply_output_gain == p_apply_output_gain;
}

void ResonanceAmbisonicProcessor::initialize(IPLContext p_context, int p_sample_rate, int p_frame_size, int p_order,
                                             bool p_use_rotation_effect, bool p_apply_hrtf, bool p_input_is_sn3d,
                                             bool p_apply_output_gain, IPLHRTF p_hrtf) {
    if (init_flags != AmbisonicInitFlags::NONE)
        return;

    if (!p_context) {
        ResonanceLog::error("ResonanceAmbisonicProcessor: Context is null.");
        return;
    }

    context = p_context;
    frame_size = p_frame_size;
    sample_rate = p_sample_rate;
    ambisonic_order = p_order;
    use_ip_ambisonics_rotation_effect = p_use_rotation_effect;
    apply_hrtf = p_apply_hrtf;
    input_is_sn3d = p_input_is_sn3d;
    apply_output_gain = p_apply_output_gain;

    IPLAudioSettings audioSettings{};
    audioSettings.samplingRate = sample_rate;
    audioSettings.frameSize = frame_size;

    int num_channels = (ambisonic_order + 1) * (ambisonic_order + 1);

    // 1. Rotation effect (optional): world-space HOA -> listener-space (combined-matrix decode path skips this)
    if (use_ip_ambisonics_rotation_effect) {
        IPLAmbisonicsRotationEffectSettings rotSettings{};
        rotSettings.maxOrder = ambisonic_order;
        IPLerror rot_status = iplAmbisonicsRotationEffectCreate(context, &audioSettings, &rotSettings, &rotation_effect);
        if (rot_status == IPL_STATUS_SUCCESS) {
            init_flags = init_flags | AmbisonicInitFlags::ROTATION;
        } else {
            ResonanceLog::warn_cstr(
                "ResonanceAmbisonicProcessor: iplAmbisonicsRotationEffectCreate failed; "
                "listener orientation is applied in the decode matrix instead (world HOA beds may sound mis-oriented).");
            use_ip_ambisonics_rotation_effect = false;
        }
    }

    const IPLHRTF hrtf_for_create = (apply_hrtf && p_hrtf) ? p_hrtf : nullptr;

    // 2. Decode effect: HOA -> stereo (panning or binaural)
    IPLAmbisonicsDecodeEffectSettings decSettings{};
    decSettings.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
    decSettings.speakerLayout.numSpeakers = 2;
    decSettings.maxOrder = ambisonic_order;
    decSettings.hrtf = hrtf_for_create;

    IPLerror dec_status = iplAmbisonicsDecodeEffectCreate(context, &audioSettings, &decSettings, &ambisonics_dec_effect);
    if (dec_status != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceAmbisonicProcessor: iplAmbisonicsDecodeEffectCreate failed.");
        cleanup();
        return;
    }
    init_flags = init_flags | AmbisonicInitFlags::DECODE;

    IPLerror buf_status = iplAudioBufferAllocate(context, num_channels, frame_size, &sa_in_buffer);
    if (buf_status != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceAmbisonicProcessor: Ambisonic buffer allocation failed.");
        cleanup();
        return;
    }

    if (input_is_sn3d) {
        buf_status = iplAudioBufferAllocate(context, num_channels, frame_size, &sa_n3d_buffer);
        if (buf_status != IPL_STATUS_SUCCESS) {
            ResonanceLog::error("ResonanceAmbisonicProcessor: N3D buffer allocation failed.");
            cleanup();
            return;
        }
    }

    buf_status = iplAudioBufferAllocate(context, num_channels, frame_size, &sa_rotated_buffer);
    if (buf_status != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceAmbisonicProcessor: Ambisonic rotated buffer allocation failed.");
        cleanup();
        return;
    }
    init_flags = init_flags | AmbisonicInitFlags::BUFFERS;
}

void ResonanceAmbisonicProcessor::cleanup() {
    if (rotation_effect)
        iplAmbisonicsRotationEffectRelease(&rotation_effect);
    if (ambisonics_dec_effect)
        iplAmbisonicsDecodeEffectRelease(&ambisonics_dec_effect);
    if (context) {
        if (sa_in_buffer.data)
            iplAudioBufferFree(context, &sa_in_buffer);
        if (sa_n3d_buffer.data)
            iplAudioBufferFree(context, &sa_n3d_buffer);
        if (sa_rotated_buffer.data)
            iplAudioBufferFree(context, &sa_rotated_buffer);
    }
    memset(&sa_in_buffer, 0, sizeof(sa_in_buffer));
    memset(&sa_n3d_buffer, 0, sizeof(sa_n3d_buffer));
    memset(&sa_rotated_buffer, 0, sizeof(sa_rotated_buffer));

    rotation_effect = nullptr;
    ambisonics_dec_effect = nullptr;
    context = nullptr;
    init_flags = AmbisonicInitFlags::NONE;
}

void ResonanceAmbisonicProcessor::process(const std::vector<float>& input_data, IPLAudioBuffer& out_buffer,
                                          bool combined_matrix_decode, const IPLCoordinateSpace3& listener_orient,
                                          const IPLCoordinateSpace3& combined_decode_orientation, IPLHRTF runtime_hrtf) {
    process(input_data.data(), input_data.size(), out_buffer, combined_matrix_decode, listener_orient, combined_decode_orientation,
            runtime_hrtf);
}

void ResonanceAmbisonicProcessor::process(const float* input_data, size_t sample_count, IPLAudioBuffer& out_buffer,
                                          bool combined_matrix_decode, const IPLCoordinateSpace3& listener_orient,
                                          const IPLCoordinateSpace3& combined_decode_orientation, IPLHRTF runtime_hrtf) {

    bool init_ok = (init_flags & AmbisonicInitFlags::DECODE) && (init_flags & AmbisonicInitFlags::BUFFERS) && ambisonics_dec_effect && out_buffer.data;
    if (!init_ok) {
        int num_channels = (ambisonic_order + 1) * (ambisonic_order + 1);
        size_t required = (size_t)frame_size * (size_t)num_channels;
        if (input_data && sample_count >= required && out_buffer.data && out_buffer.numChannels >= 2 &&
            out_buffer.data[0] && out_buffer.data[1]) {
            for (int i = 0; i < frame_size; i++) {
                float w = input_data[i * num_channels + 0];
                const float g = apply_output_gain ? (w * resonance::kAmbisonicWChannelScale * resonance::kAmbisonicDecoderOutputScalar)
                                                  : (w * resonance::kAmbisonicWChannelScale);
                out_buffer.data[0][i] = g;
                out_buffer.data[1][i] = g;
            }
        } else {
            for (int i = 0; i < out_buffer.numChannels && out_buffer.data && out_buffer.data[i]; i++) {
                memset(out_buffer.data[i], 0, frame_size * sizeof(float));
            }
        }
        return;
    }

    int num_channels_full = (ambisonic_order + 1) * (ambisonic_order + 1);
    size_t required_samples = (size_t)frame_size * (size_t)num_channels_full;
    if (!input_data || sample_count < required_samples) {
        for (int i = 0; i < out_buffer.numChannels && out_buffer.data && out_buffer.data[i]; i++) {
            memset(out_buffer.data[i], 0, frame_size * sizeof(float));
        }
        return;
    }

    iplAudioBufferDeinterleave(context, const_cast<float*>(input_data), &sa_in_buffer);

    IPLAudioBuffer* post_convert = &sa_in_buffer;
    if (input_is_sn3d) {
        iplAudioBufferConvertAmbisonics(context, IPL_AMBISONICSTYPE_SN3D, IPL_AMBISONICSTYPE_N3D, &sa_in_buffer, &sa_n3d_buffer);
        post_convert = &sa_n3d_buffer;
    }

    IPLAudioBuffer* decode_input = post_convert;
    IPLCoordinateSpace3 decode_orientation{};
    if (combined_matrix_decode)
        decode_orientation = combined_decode_orientation;
    else
        decode_orientation = listener_orient;

    last_used_rotation_effect_ = false;
    if (!combined_matrix_decode && rotation_effect && (init_flags & AmbisonicInitFlags::ROTATION)) {
        IPLAmbisonicsRotationEffectParams rotParams{};
        rotParams.orientation = listener_orient;
        rotParams.order = ambisonic_order;
        iplAmbisonicsRotationEffectApply(rotation_effect, &rotParams, post_convert, &sa_rotated_buffer);
        decode_input = &sa_rotated_buffer;
        decode_orientation = identity_orientation();
        last_used_rotation_effect_ = true;
    }

    IPLAmbisonicsDecodeEffectParams decParams{};
    decParams.order = ambisonic_order;
    const bool use_bin = apply_hrtf && runtime_hrtf;
    decParams.hrtf = use_bin ? runtime_hrtf : nullptr;
    decParams.binaural = use_bin ? IPL_TRUE : IPL_FALSE;
    decParams.orientation = decode_orientation;

    iplAmbisonicsDecodeEffectApply(ambisonics_dec_effect, &decParams, decode_input, &out_buffer);
    apply_ambisonic_decoder_output_gain(apply_output_gain, frame_size, out_buffer);
}

int ResonanceAmbisonicProcessor::get_tail_size_samples() const {
    if (!(init_flags & AmbisonicInitFlags::DECODE) || !ambisonics_dec_effect)
        return 0;
    int rotation_tail = 0;
    if (last_used_rotation_effect_ && rotation_effect && (init_flags & AmbisonicInitFlags::ROTATION))
        rotation_tail = iplAmbisonicsRotationEffectGetTailSize(rotation_effect);
    const int decode_tail = iplAmbisonicsDecodeEffectGetTailSize(ambisonics_dec_effect);
    return std::max(rotation_tail, decode_tail);
}

bool ResonanceAmbisonicProcessor::process_tail(IPLAudioBuffer& out_buffer) {
    if (!(init_flags & AmbisonicInitFlags::DECODE) || !ambisonics_dec_effect || !out_buffer.data || !out_buffer.data[0] ||
        !out_buffer.data[1])
        return false;

    ResonanceServer* srv = ResonanceServer::get_singleton();
    IPLHRTF runtime_hrtf = (srv && apply_hrtf) ? srv->get_hrtf_handle() : nullptr;

    if (last_used_rotation_effect_ && rotation_effect && (init_flags & AmbisonicInitFlags::ROTATION) &&
        iplAmbisonicsRotationEffectGetTailSize(rotation_effect) > 0) {
        const IPLAudioEffectState rot_state = iplAmbisonicsRotationEffectGetTail(rotation_effect, &sa_rotated_buffer);
        if (resonance::ambisonic_rotation_tail_needs_decode_apply(rot_state)) {
            IPLAmbisonicsDecodeEffectParams decParams{};
            decParams.order = ambisonic_order;
            const bool use_bin = apply_hrtf && runtime_hrtf;
            decParams.hrtf = use_bin ? runtime_hrtf : nullptr;
            decParams.binaural = use_bin ? IPL_TRUE : IPL_FALSE;
            decParams.orientation = identity_orientation();
            iplAmbisonicsDecodeEffectApply(ambisonics_dec_effect, &decParams, &sa_rotated_buffer, &out_buffer);
            apply_ambisonic_decoder_output_gain(apply_output_gain, frame_size, out_buffer);
            return true;
        }
    }

    if (iplAmbisonicsDecodeEffectGetTailSize(ambisonics_dec_effect) <= 0)
        return false;
    const IPLAudioEffectState dec_state = iplAmbisonicsDecodeEffectGetTail(ambisonics_dec_effect, &out_buffer);
    apply_ambisonic_decoder_output_gain(apply_output_gain, frame_size, out_buffer);
    return resonance::ambisonic_decode_tail_produced(dec_state);
}

void ResonanceAmbisonicProcessor::reset_for_new_playback() {
    if (rotation_effect)
        iplAmbisonicsRotationEffectReset(rotation_effect);
    if (ambisonics_dec_effect)
        iplAmbisonicsDecodeEffectReset(ambisonics_dec_effect);
    last_used_rotation_effect_ = false;
}

} // namespace godot
