#ifndef RESONANCE_RESOURCE_POOL_H
#define RESONANCE_RESOURCE_POOL_H

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace resonance {

struct ResourcePoolLimits {
    int min_ready = 2;
    int max_kept = 8;
    bool effects_from_start = false;
};

struct ResourcePoolStats {
    uint64_t source_misses = 0;
    uint64_t effect_misses = 0;
    int ready_sources = 0;
    int ready_effects = 0;
};

/// Sources are handed out once; effects come back and are reset on the pool thread before reuse.
/// Backend release() may also run on the thread that calls give_back_effect() or stop().
template <typename Backend>
class ResourcePool {
  public:
    using Source = typename Backend::Source;
    using Effect = typename Backend::Effect;

    ResourcePool() = default;
    ~ResourcePool() { stop(); }

    ResourcePool(const ResourcePool&) = delete;
    ResourcePool& operator=(const ResourcePool&) = delete;
    ResourcePool(ResourcePool&&) = delete;
    ResourcePool& operator=(ResourcePool&&) = delete;

    void start(const Backend& backend, ResourcePoolLimits limits) {
        stop();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            backend_ = backend;
            limits_ = limits;
            if (limits_.max_kept < limits_.min_ready)
                limits_.max_kept = limits_.min_ready;
            running_ = true;
            effects_wanted_ = limits.effects_from_start;
            source_refill_failed_ = false;
            effect_refill_failed_ = false;
        }
        thread_ = std::thread(&ResourcePool::run, this);
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
        }
        work_cv_.notify_all();
        if (thread_.joinable())
            thread_.join();
        std::lock_guard<std::mutex> lock(mutex_);
        for (Source source : ready_sources_)
            backend_.release(source);
        for (Effect effect : ready_effects_)
            backend_.release(effect);
        for (Effect effect : returned_effects_)
            backend_.release(effect);
        ready_sources_.clear();
        ready_effects_.clear();
        returned_effects_.clear();
    }

    bool is_running() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_;
    }

    /// Empty on a miss; the caller then builds the source itself.
    Source take_source() {
        Source source{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_)
                return source;
            if (ready_sources_.empty()) {
                source_misses_++;
                return source;
            }
            source = ready_sources_.back();
            ready_sources_.pop_back();
        }
        work_cv_.notify_one();
        return source;
    }

    /// Empty on a miss; the caller then builds the effect itself.
    Effect take_effect() {
        Effect effect{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_)
                return effect;
            effects_wanted_ = true;
            if (ready_effects_.empty()) {
                effect_misses_++;
            } else {
                effect = ready_effects_.back();
                ready_effects_.pop_back();
            }
        }
        work_cv_.notify_one();
        return effect;
    }

    /// Takes ownership. Released at once when the pool is stopped or already holds max_kept effects.
    void give_back_effect(Effect effect) {
        if (!effect)
            return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const size_t held = ready_effects_.size() + returned_effects_.size();
            if (running_ && held < static_cast<size_t>(limits_.max_kept)) {
                returned_effects_.push_back(effect);
                work_cv_.notify_one();
                return;
            }
        }
        backend_.release(effect);
    }

    ResourcePoolStats get_stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        ResourcePoolStats stats;
        stats.source_misses = source_misses_;
        stats.effect_misses = effect_misses_;
        stats.ready_sources = static_cast<int>(ready_sources_.size());
        stats.ready_effects = static_cast<int>(ready_effects_.size());
        return stats;
    }

  private:
    void run() {
        backend_.on_pool_thread_start();
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            work_cv_.wait(lock, [this] { return !running_ || has_work_locked(); });
            if (!running_)
                return;
            if (!returned_effects_.empty()) {
                recycle_one_locked(lock);
                continue;
            }
            if (sources_need_refill_locked()) {
                build_one_locked(lock, ready_sources_, source_refill_failed_, [this] { return backend_.create_source(); });
                continue;
            }
            build_one_locked(lock, ready_effects_, effect_refill_failed_, [this] { return backend_.create_effect(); });
        }
    }

    bool has_work_locked() const {
        return !returned_effects_.empty() || sources_need_refill_locked() || effects_need_refill_locked();
    }

    bool sources_need_refill_locked() const {
        return !source_refill_failed_ && ready_sources_.size() < static_cast<size_t>(limits_.min_ready);
    }

    bool effects_need_refill_locked() const {
        return effects_wanted_ && !effect_refill_failed_ && ready_effects_.size() < static_cast<size_t>(limits_.min_ready);
    }

    void recycle_one_locked(std::unique_lock<std::mutex>& lock) {
        Effect effect = returned_effects_.back();
        returned_effects_.pop_back();
        lock.unlock();
        backend_.reset(effect);
        lock.lock();
        ready_effects_.push_back(effect);
    }

    template <typename T, typename Create>
    void build_one_locked(std::unique_lock<std::mutex>& lock, std::vector<T>& ready, bool& refill_failed, Create create) {
        lock.unlock();
        T item = create();
        lock.lock();
        if (!item) {
            refill_failed = true;
            return;
        }
        ready.push_back(item);
    }

    mutable std::mutex mutex_;
    std::condition_variable work_cv_;
    std::thread thread_;
    Backend backend_{};
    ResourcePoolLimits limits_;
    bool running_ = false;
    bool effects_wanted_ = false;
    bool source_refill_failed_ = false;
    bool effect_refill_failed_ = false;
    std::vector<Source> ready_sources_;
    std::vector<Effect> ready_effects_;
    std::vector<Effect> returned_effects_;
    uint64_t source_misses_ = 0;
    uint64_t effect_misses_ = 0;
};

} // namespace resonance

#endif // RESONANCE_RESOURCE_POOL_H
