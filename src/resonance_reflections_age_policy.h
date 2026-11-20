#ifndef RESONANCE_REFLECTIONS_AGE_POLICY_H
#define RESONANCE_REFLECTIONS_AGE_POLICY_H

#include "resonance_atomic_max.h"

#include <atomic>
#include <cstdint>
#include <optional>

namespace resonance {

/// Reflections age: time between two completed reflections passes. A pass skipped because its inputs
/// did not change still counts as completed, since its result stays current.
class ReflectionsAgeTracker {
  public:
    /// Returns the age this pass produces. No sample for the first pass after reset() or while realtime
    /// reflections are off; a pass without realtime reflections also ends the chain.
    std::optional<uint64_t> note_pass(uint64_t now_us, bool realtime_reflections_active) {
        if (!realtime_reflections_active) {
            have_last_pass_ = false;
            return std::nullopt;
        }
        std::optional<uint64_t> age;
        if (have_last_pass_ && now_us >= last_pass_end_us_)
            age = now_us - last_pass_end_us_;
        last_pass_end_us_ = now_us;
        have_last_pass_ = true;
        return age;
    }

    void reset() { have_last_pass_ = false; }

  private:
    bool have_last_pass_ = false;
    uint64_t last_pass_end_us_ = 0;
};

} // namespace resonance

#endif // RESONANCE_REFLECTIONS_AGE_POLICY_H
