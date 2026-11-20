#ifndef RESONANCE_SPAWN_TIMING_H
#define RESONANCE_SPAWN_TIMING_H

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace resonance {

enum class SpawnPhase : int {
    VoiceDsp = 0,
    ReflectionEffect,
    Source,
    ParamsPush,
    Count,
};

/// Main-thread cost of starting a player, split by phase. Nested phases are exclusive: time spent in an inner
/// phase is not charged to the outer one.
class SpawnTiming {
  public:
    void enter(SpawnPhase phase, uint64_t now_us) {
        if (depth_ >= kMaxDepth) {
            overflow_++;
            return;
        }
        if (depth_ > 0)
            charge(stack_[depth_ - 1], now_us);
        stack_[depth_++] = phase;
        phase_start_us_ = now_us;
    }

    void leave(uint64_t now_us) {
        if (overflow_ > 0) {
            overflow_--;
            return;
        }
        if (depth_ == 0)
            return;
        charge(stack_[--depth_], now_us);
        phase_start_us_ = now_us;
    }

    uint64_t total_us(SpawnPhase phase) const { return totals_[static_cast<std::size_t>(phase)]; }

  private:
    static constexpr int kMaxDepth = 8;

    void charge(SpawnPhase phase, uint64_t now_us) {
        if (now_us > phase_start_us_)
            totals_[static_cast<std::size_t>(phase)] += now_us - phase_start_us_;
    }

    std::array<uint64_t, static_cast<std::size_t>(SpawnPhase::Count)> totals_{};
    std::array<SpawnPhase, kMaxDepth> stack_{};
    int depth_ = 0;
    int overflow_ = 0;
    uint64_t phase_start_us_ = 0;
};

inline uint64_t spawn_timing_now_us() {
    const auto since_epoch = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(since_epoch).count());
}

class ScopedSpawnPhase {
  public:
    ScopedSpawnPhase(SpawnTiming* timing, SpawnPhase phase) : timing_(timing) {
        if (timing_)
            timing_->enter(phase, spawn_timing_now_us());
    }
    ~ScopedSpawnPhase() {
        if (timing_)
            timing_->leave(spawn_timing_now_us());
    }

    ScopedSpawnPhase(const ScopedSpawnPhase&) = delete;
    ScopedSpawnPhase& operator=(const ScopedSpawnPhase&) = delete;
    ScopedSpawnPhase(ScopedSpawnPhase&&) = delete;
    ScopedSpawnPhase& operator=(ScopedSpawnPhase&&) = delete;

  private:
    SpawnTiming* timing_;
};

} // namespace resonance

#endif // RESONANCE_SPAWN_TIMING_H
