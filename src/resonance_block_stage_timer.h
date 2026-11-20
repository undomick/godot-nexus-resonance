#pragma once

#include "resonance_atomic_max.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace resonance {

enum class BlockStage : uint8_t {
    Input,
    Direct,
    Reflections,
    DirectPost,
    Pathing,
    Output,
    Count,
};

constexpr size_t kBlockStageCount = static_cast<size_t>(BlockStage::Count);

constexpr std::array<const char*, kBlockStageCount> kBlockStageNames = {
    "input",
    "direct",
    "reflections",
    "direct_post",
    "pathing",
    "output",
};

constexpr const char* block_stage_name(BlockStage stage) {
    return kBlockStageNames[static_cast<size_t>(stage)];
}

inline uint64_t elapsed_ns(std::chrono::steady_clock::time_point start,
                           std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now()) {
    if (end <= start)
        return 0;
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

constexpr double kNanosecondsPerMicrosecond = 1000.0;

constexpr double ns_to_us(uint64_t ns) {
    return static_cast<double>(ns) / kNanosecondsPerMicrosecond;
}

// Total and longest single duration of a timed call. Written by the audio thread, read and reset from the main thread.
class DurationTotals {
  public:
    void add(uint64_t ns) {
        total_.fetch_add(ns, std::memory_order_relaxed);
        store_max(max_, ns);
    }
    uint64_t total_ns() const { return total_.load(std::memory_order_relaxed); }
    uint64_t max_ns() const { return max_.load(std::memory_order_relaxed); }
    void reset() {
        total_.store(0, std::memory_order_relaxed);
        max_.store(0, std::memory_order_relaxed);
    }

  private:
    std::atomic<uint64_t> total_{0};
    std::atomic<uint64_t> max_{0};
};

// Written by the audio thread, read and reset from the main thread.
class BlockStageTotals {
  public:
    void add(BlockStage stage, uint64_t ns) {
        const size_t i = static_cast<size_t>(stage);
        totals_[i].fetch_add(ns, std::memory_order_relaxed);
        store_max(max_[i], ns);
    }
    uint64_t ns(BlockStage stage) const { return totals_[static_cast<size_t>(stage)].load(std::memory_order_relaxed); }
    // Longest single lap of the stage since the last reset.
    uint64_t max_ns(BlockStage stage) const { return max_[static_cast<size_t>(stage)].load(std::memory_order_relaxed); }
    void reset() {
        for (std::atomic<uint64_t>& total : totals_)
            total.store(0, std::memory_order_relaxed);
        for (std::atomic<uint64_t>& max : max_)
            max.store(0, std::memory_order_relaxed);
    }

  private:
    std::array<std::atomic<uint64_t>, kBlockStageCount> totals_{};
    std::array<std::atomic<uint64_t>, kBlockStageCount> max_{};
};

// Charges the time since the previous lap to a stage, so consecutive laps cover the block without gaps.
class StageLapClock {
  public:
    explicit StageLapClock(std::chrono::steady_clock::time_point start) : last_(start) {}

    void lap(BlockStageTotals& totals, BlockStage stage,
             std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        totals.add(stage, elapsed_ns(last_, now));
        last_ = now;
    }

  private:
    std::chrono::steady_clock::time_point last_;
};

} // namespace resonance
