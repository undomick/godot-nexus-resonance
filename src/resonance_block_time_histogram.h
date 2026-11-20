#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace resonance {

// Log-scale buckets: four per power of two, exact below 4 us. The last bucket also takes every longer block.
constexpr size_t kBlockTimeSubBucketBits = 2;
constexpr size_t kBlockTimeSubBuckets = size_t{1} << kBlockTimeSubBucketBits;
constexpr size_t kBlockTimeBucketCount = 64;

constexpr size_t floor_log2(uint64_t value) {
    size_t log = 0;
    while (value >>= 1)
        ++log;
    return log;
}

constexpr size_t block_time_bucket(uint64_t us) {
    if (us < kBlockTimeSubBuckets)
        return static_cast<size_t>(us);
    const size_t octave = floor_log2(us);
    const size_t shift = octave - kBlockTimeSubBucketBits;
    const size_t sub = static_cast<size_t>(us >> shift) & (kBlockTimeSubBuckets - 1);
    const size_t bucket = (octave - kBlockTimeSubBucketBits + 1) * kBlockTimeSubBuckets + sub;
    return bucket < kBlockTimeBucketCount ? bucket : kBlockTimeBucketCount - 1;
}

// Exclusive upper edge of a bucket in us.
constexpr uint64_t block_time_bucket_upper_us(size_t bucket) {
    if (bucket < kBlockTimeSubBuckets)
        return bucket + 1;
    const size_t shift = bucket / kBlockTimeSubBuckets - 1;
    const uint64_t sub = bucket % kBlockTimeSubBuckets;
    return (kBlockTimeSubBuckets + sub + 1) << shift;
}

// Written by the audio thread, read and reset from the main thread.
class BlockTimeHistogram {
  public:
    void record(uint64_t us) { counts_[block_time_bucket(us)].fetch_add(1, std::memory_order_relaxed); }
    uint64_t count(size_t bucket) const { return counts_[bucket].load(std::memory_order_relaxed); }
    void reset() {
        for (std::atomic<uint64_t>& count : counts_)
            count.store(0, std::memory_order_relaxed);
    }

  private:
    std::array<std::atomic<uint64_t>, kBlockTimeBucketCount> counts_{};
};

} // namespace resonance
