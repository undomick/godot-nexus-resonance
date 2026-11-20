#pragma once

#include <atomic>
#include <cstdint>

namespace resonance {

inline void store_max(std::atomic<uint64_t>& target, uint64_t value) {
    uint64_t current = target.load(std::memory_order_relaxed);
    while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

} // namespace resonance
