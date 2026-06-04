#ifndef RESONANCE_REFLECTION_IR_FINGERPRINT_H
#define RESONANCE_REFLECTION_IR_FINGERPRINT_H

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace godot {

/// Sum of energy-field bins (Steam Audio baked layer readback). [field] is IPLEnergyField.
float reflection_energy_field_total(const void* field);

/// q16 fingerprint from total energy (log-scaled for wide dynamic range).
inline uint16_t reflection_baked_energy_to_q16(float total_energy) {
    if (!(total_energy > 0.0f) || !std::isfinite(total_energy))
        return 0;
    const float scaled = std::min(std::log1p(total_energy) / 8.0f, 1.0f);
    return static_cast<uint16_t>(std::lround(scaled * 65535.0f));
}

/// Attempt IPLImpulseResponse tap-sum. Not for IPLReflectionEffectIR (runtime overlap-save handles); returns 0.
uint16_t reflection_conv_ir_fir_energy_q16(const void* ir, int32_t ir_size, int32_t num_channels);

} // namespace godot

#endif
