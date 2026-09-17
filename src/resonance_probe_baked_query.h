#ifndef RESONANCE_PROBE_BAKED_QUERY_H
#define RESONANCE_PROBE_BAKED_QUERY_H

#include <godot_cpp/variant/dictionary.hpp>
#include <phonon.h>

namespace godot {

Dictionary probe_query_failure(const char* reason);

Dictionary probe_baked_pack_energy_field(IPLEnergyField field);
Dictionary probe_baked_pack_impulse_response(IPLImpulseResponse ir);
bool probe_baked_reconstruct_ir(IPLContext context, IPLEnergyField energy_field, int ambisonics_order, int sampling_rate,
                                IPLfloat32 duration, IPLImpulseResponse out_ir);

} // namespace godot

#endif
