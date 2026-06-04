#include "../resonance_reflection_ir_fingerprint.h"

namespace godot {

// Unit tests link without phonon; production implementations live in resonance_reflection_ir_fingerprint.cpp.
float reflection_energy_field_total(const void* field) {
    (void)field;
    return 0.0f;
}

uint16_t reflection_conv_ir_fir_energy_q16(const void* ir, int32_t ir_size, int32_t num_channels) {
    (void)ir;
    (void)ir_size;
    (void)num_channels;
    return 0;
}

} // namespace godot
