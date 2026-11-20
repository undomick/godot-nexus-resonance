#ifndef RESONANCE_LATEST_VALUE_SLOT_H
#define RESONANCE_LATEST_VALUE_SLOT_H

#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>

namespace resonance {

/// Lock-free triple buffer for one producer thread and one consumer thread. The latest published value
/// wins; values published between two consumes are skipped. Neither side blocks, retries or allocates.
template <typename T>
class LatestValueSlot {
    static_assert(std::is_trivially_copyable_v<T>, "LatestValueSlot copies T between threads");

  public:
    /// Producer thread only.
    void publish(const T& value) {
        slots_[back_] = value;
        const uint8_t old_middle = middle_.exchange(static_cast<uint8_t>(back_ | kFreshBit), std::memory_order_acq_rel);
        back_ = old_middle & kIndexMask;
    }

    /// Consumer thread only. Returns true when a value newer than the last consumed one became front().
    bool consume() {
        if ((middle_.load(std::memory_order_relaxed) & kFreshBit) == 0)
            return false;
        const uint8_t old_middle = middle_.exchange(front_, std::memory_order_acq_rel);
        front_ = old_middle & kIndexMask;
        return true;
    }

    /// Consumer thread only. Value-initialized T until the first successful consume().
    const T& front() const { return slots_[front_]; }

  private:
    static constexpr uint8_t kIndexMask = 0x3;
    static constexpr uint8_t kFreshBit = 0x4;

    std::array<T, 3> slots_{};
    std::atomic<uint8_t> middle_{1};
    uint8_t back_ = 0;
    uint8_t front_ = 2;
};

} // namespace resonance

#endif // RESONANCE_LATEST_VALUE_SLOT_H
