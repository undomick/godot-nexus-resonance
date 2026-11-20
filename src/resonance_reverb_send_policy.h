#ifndef RESONANCE_REVERB_SEND_POLICY_H
#define RESONANCE_REVERB_SEND_POLICY_H

namespace resonance {

/// The reverb send child outlives a stopped parent until its voices' send rings drain, so the last
/// sent samples reach the reverb bus instead of being cut off.
inline bool reverb_send_child_should_play(bool parent_playing, bool reverb_tail_pending) {
    return parent_playing || reverb_tail_pending;
}

} // namespace resonance

#endif // RESONANCE_REVERB_SEND_POLICY_H
