#include "resonance_probe_baked_query.h"

#include "resonance_constants.h"
#include "resonance_energy_field_query_policy.h"
#include "resonance_ipl_guard.h"
#include "resonance_reflection_ir_fingerprint.h"
#include <algorithm>
#include <cstring>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot {

Dictionary probe_query_failure(const char* reason) {
    Dictionary d;
    d["ok"] = false;
    if (reason)
        d["error"] = reason;
    return d;
}

Dictionary probe_baked_pack_energy_field(IPLEnergyField field) {
    Dictionary d;
    const IPLint32 channels = field ? iplEnergyFieldGetNumChannels(field) : 0;
    const IPLint32 bins = field ? iplEnergyFieldGetNumBins(field) : 0;
    IPLfloat32* raw = field ? iplEnergyFieldGetData(field) : nullptr;
    if (resonance::energy_field_pack_skip(field != nullptr, channels, bins, raw != nullptr))
        return d;
    const int64_t count = resonance::energy_field_sample_count(channels, bins);
    PackedFloat32Array data;
    data.resize(static_cast<int>(count));
    if (count > 0)
        std::memcpy(data.ptrw(), raw, static_cast<size_t>(count) * sizeof(float));
    d["channels"] = channels;
    d["bins"] = bins;
    d["bands"] = resonance::kEnergyFieldDiffuseBands;
    d["data"] = data;
    d["total_energy"] = reflection_energy_field_total(field);
    return d;
}

Dictionary probe_baked_pack_impulse_response(IPLImpulseResponse ir) {
    Dictionary d;
    if (!ir)
        return d;
    const IPLint32 channels = iplImpulseResponseGetNumChannels(ir);
    const IPLint32 samples = iplImpulseResponseGetNumSamples(ir);
    if (channels <= 0 || samples <= 0)
        return d;
    IPLfloat32* raw = iplImpulseResponseGetData(ir);
    if (!raw)
        return d;
    const int64_t count = static_cast<int64_t>(channels) * samples;
    PackedFloat32Array data;
    data.resize(static_cast<int>(count));
    if (count > 0)
        std::memcpy(data.ptrw(), raw, static_cast<size_t>(count) * sizeof(float));
    d["channels"] = channels;
    d["num_samples"] = samples;
    d["data"] = data;
    return d;
}

bool probe_baked_reconstruct_ir(IPLContext context, IPLEnergyField energy_field, int ambisonics_order, int sampling_rate,
                                IPLfloat32 duration, IPLImpulseResponse out_ir) {
    if (!resonance::energy_field_reconstruct_ir_args_ok(context != nullptr, energy_field != nullptr, out_ir != nullptr))
        return false;
    const int order = resonance::clamp_bake_ambisonics_order(ambisonics_order);
    const int sr = (sampling_rate > 0) ? sampling_rate : 48000;
    const IPLfloat32 dur = (duration > 0.0f) ? duration : resonance::kBakerSimulatedDuration;

    IPLReconstructorSettings rec_settings{};
    rec_settings.maxDuration = dur;
    rec_settings.maxOrder = order;
    rec_settings.samplingRate = sr;
    IPLReconstructor reconstructor = nullptr;
    if (iplReconstructorCreate(context, &rec_settings, &reconstructor) != IPL_STATUS_SUCCESS)
        return false;
    IPLScopedRelease<IPLReconstructor> rec_guard(reconstructor, iplReconstructorRelease);

    IPLReconstructorInputs input{};
    input.energyField = energy_field;
    IPLReconstructorSharedInputs shared{};
    shared.duration = dur;
    shared.order = order;
    IPLReconstructorOutputs output{};
    output.impulseResponse = out_ir;
    iplReconstructorReconstruct(reconstructor, 1, &input, &shared, &output);
    return true;
}

} // namespace godot
