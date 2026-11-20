#ifndef RESONANCE_WRITER_PREFERRING_SHARED_MUTEX_H
#define RESONANCE_WRITER_PREFERRING_SHARED_MUTEX_H

#include <atomic>
#include <chrono>
#include <shared_mutex>
#include <thread>

namespace resonance {

/// Shared mutex where a waiting exclusive locker keeps new shared lockers out. std::shared_mutex on
/// glibc prefers readers, so two threads whose shared holds overlap could starve an exclusive locker.
/// Works with std::shared_lock and std::unique_lock.
class WriterPreferringSharedMutex {
  public:
    void lock() {
        writers_waiting_.fetch_add(1, std::memory_order_acq_rel);
        mutex_.lock();
        writers_waiting_.fetch_sub(1, std::memory_order_acq_rel);
    }

    bool try_lock() { return mutex_.try_lock(); }

    void unlock() { mutex_.unlock(); }

    void lock_shared() {
        for (;;) {
            while (writers_waiting_.load(std::memory_order_acquire) > 0)
                std::this_thread::sleep_for(kSharedRetryInterval);
            mutex_.lock_shared();
            if (writers_waiting_.load(std::memory_order_acquire) == 0)
                return;
            mutex_.unlock_shared();
        }
    }

    bool try_lock_shared() {
        if (writers_waiting_.load(std::memory_order_acquire) > 0)
            return false;
        return mutex_.try_lock_shared();
    }

    void unlock_shared() { mutex_.unlock_shared(); }

  private:
    static constexpr std::chrono::microseconds kSharedRetryInterval{500};
    std::shared_mutex mutex_;
    std::atomic<int> writers_waiting_{0};
};

} // namespace resonance

#endif // RESONANCE_WRITER_PREFERRING_SHARED_MUTEX_H
