#ifndef RESONANCE_POLYPHONIC_VOICE_POLICY_H
#define RESONANCE_POLYPHONIC_VOICE_POLICY_H

#include <cstdint>

namespace resonance {

/// One reflection Apply reader per source. A different reader id is rejected until the holder clears it.
inline bool claim_reflection_apply(int32_t* reader, int32_t reader_id) {
    if (*reader >= 0 && *reader != reader_id)
        return false;
    *reader = reader_id;
    return true;
}

/// The single reader id used by a player's shared reflection effect.
constexpr int32_t kEmitterReflectionReader = 1;

/// A voice may feed the emitter it is bound to. A copied handle from another source must not.
inline bool voice_may_convolve_source(int32_t owned_handle, int32_t param_handle) {
    return owned_handle >= 0 && param_handle == owned_handle;
}

/// Tail params keep an IR pointer. Reuse them only for the source that published them.
inline bool voice_tail_ir_matches_source(int32_t tail_source_handle, int32_t owned_handle) {
    return owned_handle >= 0 && tail_source_handle == owned_handle;
}

/// Recycled handles must not Apply the previous source's IR. Generation 0 is never live.
inline bool voice_reflection_ir_generation_live(uint32_t published_generation, uint32_t live_generation) {
    return published_generation != 0u && published_generation == live_generation;
}

/// The emitter's last applied params may stand in for a fetch miss only while they still belong to that source.
inline bool emitter_tail_applyable(bool tail_have, int32_t tail_source_handle, int32_t owned_handle,
                                   uint32_t tail_ir_generation, uint32_t live_ir_generation) {
    return tail_have && voice_tail_ir_matches_source(tail_source_handle, owned_handle) &&
           voice_reflection_ir_generation_live(tail_ir_generation, live_ir_generation);
}

} // namespace resonance

#endif // RESONANCE_POLYPHONIC_VOICE_POLICY_H
