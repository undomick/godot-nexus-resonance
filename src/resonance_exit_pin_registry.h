#ifndef RESONANCE_EXIT_PIN_REGISTRY_H
#define RESONANCE_EXIT_PIN_REGISTRY_H

#include <mutex>
#include <unordered_set>

namespace resonance {

/// Live objects that may outlive the extension's class table at exit.
/// pin_survivors() hands each one still tracked to a pin function.
template <typename T>
class ExitPinRegistry {
  public:
    void track(T* obj) {
        std::lock_guard<std::mutex> lock(mutex_);
        live_.insert(obj);
    }

    void untrack(T* obj) {
        std::lock_guard<std::mutex> lock(mutex_);
        live_.erase(obj);
    }

    template <typename PinFn>
    void pin_survivors(PinFn pin) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (T* obj : live_)
            pin(obj);
    }

  private:
    std::mutex mutex_;
    std::unordered_set<T*> live_;
};

} // namespace resonance

#endif
