#ifndef RESONANCE_REVERB_DATA_POINT_QUERY_POLICY_H
#define RESONANCE_REVERB_DATA_POINT_QUERY_POLICY_H

namespace resonance {

/// One-shot play-mode query after ResonanceServer has an IPL context. No per-frame IPL.
inline bool reverb_data_point_should_query_once(bool editor_hint, bool has_probe_data, bool server_initialized,
                                                bool already_queried) {
    if (editor_hint)
        return false;
    if (!has_probe_data)
        return false;
    if (!server_initialized)
        return false;
    if (already_queried)
        return false;
    return true;
}

} // namespace resonance

#endif // RESONANCE_REVERB_DATA_POINT_QUERY_POLICY_H
