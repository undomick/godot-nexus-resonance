#include "resonance_reflection_ir_fingerprint.h"

#include "resonance_energy_field_query_policy.h"

#include <algorithm>
#include <cmath>
#include <phonon.h>

namespace godot {

float reflection_energy_field_total(const void* field) {
    if (!field)
        return 0.0f;
    IPLEnergyField ef = reinterpret_cast<IPLEnergyField>(const_cast<void*>(field));
    IPLfloat32* data = iplEnergyFieldGetData(ef);
    if (!data)
        return 0.0f;
    const IPLint32 channels = iplEnergyFieldGetNumChannels(ef);
    const IPLint32 bins = iplEnergyFieldGetNumBins(ef);
    if (channels <= 0 || bins <= 0)
        return 0.0f;
    return resonance::energy_field_total_from_samples(data, resonance::energy_field_sample_count(channels, bins));
}

uint16_t reflection_conv_ir_fir_energy_q16(const void* ir, int32_t ir_size, int32_t num_channels) {
    (void)ir;
    (void)ir_size;
    (void)num_channels;
    // IPLReflectionParams.ir is IPLReflectionEffectIR (overlap-save), not IPLImpulseResponse.
    return 0;
}

} // namespace godot
