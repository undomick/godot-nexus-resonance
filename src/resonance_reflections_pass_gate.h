#ifndef RESONANCE_REFLECTIONS_PASS_GATE_H
#define RESONANCE_REFLECTIONS_PASS_GATE_H

#include <chrono>
#include <mutex>
#include <thread>

namespace resonance {

enum class DirectPassMode {
    None,
    // Holds the simulation mutex: inputs, commits and scene changes are allowed.
    Full,
    // A reflections pass is running and holds the simulation mutex. Only direct inputs,
    // RunDirect and direct outputs are allowed.
    DirectOnly,
};

struct DirectPassLock {
    DirectPassMode mode = DirectPassMode::None;
    std::unique_lock<std::mutex> simulation;
    std::unique_lock<std::mutex> direct_only;
};

constexpr std::chrono::microseconds kDirectPassLockRetryInterval{500};

/// Lets the direct worker run while a reflections pass holds the simulation mutex. The reflections
/// worker calls begin_run() and end_run() while it holds the simulation mutex, around the Steam Audio
/// Run* calls only, so a direct-only pass never overlaps the parts of a reflections pass that touch
/// shared state.
class ReflectionsPassGate {
  public:
    void begin_run() {
        std::lock_guard<std::mutex> lock(direct_only_mutex_);
        running_ = true;
    }

    void end_run() {
        std::lock_guard<std::mutex> lock(direct_only_mutex_);
        running_ = false;
    }

    /// Retries until the simulation mutex is free or a reflections pass is running. Returns
    /// DirectPassMode::None once should_stop() is true.
    template <typename StopFn>
    DirectPassLock lock_direct_pass(std::mutex& simulation_mutex, StopFn should_stop) {
        DirectPassLock out;
        while (!should_stop()) {
            out.simulation = std::unique_lock<std::mutex>(simulation_mutex, std::try_to_lock);
            if (out.simulation.owns_lock()) {
                out.mode = DirectPassMode::Full;
                return out;
            }
            out.direct_only = std::unique_lock<std::mutex>(direct_only_mutex_);
            if (running_) {
                out.mode = DirectPassMode::DirectOnly;
                return out;
            }
            out.direct_only.unlock();
            // The mutex holder may be a short main-thread change or a reflections pass that has not
            // reached its Run* calls yet; either way the next try decides.
            std::this_thread::sleep_for(kDirectPassLockRetryInterval);
        }
        return out;
    }

  private:
    std::mutex direct_only_mutex_;
    bool running_ = false;
};

/// A new reflections pass is handed to the reflections worker only from a full direct pass, after
/// that pass applied pending commits, and only while the reflections worker has no pass of its own.
inline bool reflections_pass_may_start(DirectPassMode mode, bool reflections_worker_busy) {
    return mode == DirectPassMode::Full && !reflections_worker_busy;
}

} // namespace resonance

#endif // RESONANCE_REFLECTIONS_PASS_GATE_H
