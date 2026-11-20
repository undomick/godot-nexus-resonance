#ifndef RESONANCE_MIX_LATENESS_POLICY_H
#define RESONANCE_MIX_LATENESS_POLICY_H

#include <algorithm>
#include <cstdint>

namespace resonance {

/// Decides whether a mix call came too late to keep up with real time. Audio drivers often request
/// several blocks back to back and then wait (a PipeWire quantum of 1024 frames against 512-frame mix
/// blocks), so a single long gap is normal. A call is late only when the audio mixed so far falls more
/// than half a block behind the wall time that passed.
class MixLatenessTracker {
  public:
    /// gap_us: wall time since the previous call. previous_block_us: audio duration that call produced.
    bool note_mix(uint64_t gap_us, uint64_t previous_block_us) {
        const int64_t block_us = static_cast<int64_t>(previous_block_us);
        const int64_t ahead_us = ahead_us_ + block_us - static_cast<int64_t>(gap_us);
        if (ahead_us < -block_us / 2) {
            ahead_us_ = 0;
            return true;
        }
        // A deficit within tolerance is jitter, not debt: carrying it would turn the next small jitter late.
        ahead_us_ = std::clamp<int64_t>(ahead_us, 0, kMaxAheadUs);
        return false;
    }

    void reset() { ahead_us_ = 0; }

  private:
    /// Caps the lead so clock drift between the audio device and the CPU cannot hide a real stall.
    static constexpr int64_t kMaxAheadUs = 100000;
    int64_t ahead_us_ = 0;
};

} // namespace resonance

#endif // RESONANCE_MIX_LATENESS_POLICY_H
