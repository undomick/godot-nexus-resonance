#ifndef RESONANCE_PLAYBACK_INPUT_STARTED_POLICY_H
#define RESONANCE_PLAYBACK_INPUT_STARTED_POLICY_H

namespace resonance {

/// Steam path may start on the first ingested decode block; do not wait for non-zero samples (leading digital silence).
inline bool playback_steam_input_path_should_run(bool input_started, bool first_block_ingested) {
    return input_started || first_block_ingested;
}

} // namespace resonance

#endif // RESONANCE_PLAYBACK_INPUT_STARTED_POLICY_H
