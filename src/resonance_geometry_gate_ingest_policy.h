#ifndef RESONANCE_GEOMETRY_GATE_INGEST_POLICY_H
#define RESONANCE_GEOMETRY_GATE_INGEST_POLICY_H

#include "resonance_constants.h"

namespace resonance {

inline bool geometry_gate_should_pause_input_ingest(int source_handle, bool spatial_output_ready) {
    return spatial_audio_geometry_gate_should_hold_decode(source_handle, spatial_output_ready);
}

inline bool geometry_gate_should_hold_decoder_advance(int source_handle, bool spatial_output_ready) {
    return geometry_gate_should_pause_input_ingest(source_handle, spatial_output_ready);
}

/// Ambisonic beds have no IPL source handle, so only the global geometry gate applies.
inline bool geometry_gate_should_pause_ambisonic_input_ingest(bool spatial_output_ready) {
    return !spatial_output_ready;
}

inline bool geometry_gate_should_hold_ambisonic_decoder_advance(bool spatial_output_ready) {
    return geometry_gate_should_pause_ambisonic_input_ingest(spatial_output_ready);
}

} // namespace resonance

#endif // RESONANCE_GEOMETRY_GATE_INGEST_POLICY_H
