#ifndef HANDLE_MANAGER_H
#define HANDLE_MANAGER_H

#include <atomic>
#include <climits>
#include <cstdint>
#include <mutex>
#include <phonon.h>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace godot {

inline void _handle_release_source(IPLSource* p) {
    if (p && *p)
        iplSourceRelease(p);
}
inline void _handle_release_batch(IPLProbeBatch* p) {
    if (p && *p)
        iplProbeBatchRelease(p);
}

/// Maps int32 handles to Steam Audio objects. Freed handles are reused lowest first.
/// ReleaseFunc gets the address of the stored object and must accept null.
template <typename T, void (*ReleaseFunc)(T*)>
class HandleManagerBase {
  public:
    HandleManagerBase() = default;
    HandleManagerBase(const HandleManagerBase&) = delete;
    HandleManagerBase(HandleManagerBase&&) = delete;
    HandleManagerBase& operator=(const HandleManagerBase&) = delete;
    HandleManagerBase& operator=(HandleManagerBase&&) = delete;

  public:
    /// Lock-free, for monitors that must not block on the mutex.
    int32_t size_approx() const { return item_count_.load(std::memory_order_relaxed); }

  protected:
    int32_t next_handle = 0;
    std::priority_queue<int32_t, std::vector<int32_t>, std::greater<int32_t>> free_handles;
    std::unordered_map<int32_t, T> items;
    mutable std::mutex mutex;
    /// Mirror of items.size(), written under mutex and read without it.
    std::atomic<int32_t> item_count_{0};

    /// Returns -1 when no handle is free and next_handle reached INT32_MAX.
    int32_t alloc_handle() {
        if (!free_handles.empty()) {
            int32_t h = free_handles.top();
            free_handles.pop();
            return h;
        }
        if (next_handle >= INT32_MAX) {
            return -1;
        }
        return next_handle++;
    }

    void recycle_handle(int32_t h) { free_handles.push(h); }

    /// Caller holds mutex.
    void release_all() {
        for (auto& pair : items) {
            if (pair.second)
                ReleaseFunc(&pair.second);
        }
        items.clear();
        item_count_.store(0, std::memory_order_relaxed);
    }

    /// Moves all items to out and resets handle allocation. Caller holds mutex and releases each item.
    void clear_and_reset(std::vector<T>& out) {
        out.clear();
        for (auto& pair : items) {
            out.push_back(pair.second);
        }
        items.clear();
        item_count_.store(0, std::memory_order_relaxed);
        free_handles = std::priority_queue<int32_t, std::vector<int32_t>, std::greater<int32_t>>();
        next_handle = 0;
    }
};

class SourceManager : public HandleManagerBase<IPLSource, _handle_release_source> {
  public:
    SourceManager();
    ~SourceManager();
    /// Retains its own ref, so the caller keeps and still releases theirs. Returns -1 for null or overflow.
    int32_t add_source(IPLSource source);
    /// With recycle=false the handle stays reserved until recycle_source_handle(), so a new source
    /// cannot get it while worker queues still reference it.
    void remove_source(int32_t handle, bool recycle = true);
    /// No-op unless the handle was removed with recycle=false.
    void recycle_source_handle(int32_t handle);
    /// Returns a retained ref the caller must release, or null.
    IPLSource get_source(int32_t handle);
    bool has_handle(int32_t handle) const;
    void get_all_handles(std::vector<int32_t>& out) const;

  private:
    /// Caps sequential ids so the server's per-handle caches stay in bounds.
    int32_t alloc_source_handle_locked();
    std::unordered_set<int32_t> deferred_recycle_;
};

class ProbeBatchManager : public HandleManagerBase<IPLProbeBatch, _handle_release_batch> {
  public:
    ProbeBatchManager();
    ~ProbeBatchManager();
    /// Takes the caller's ref, also on overflow (then released). Returns -1 for null or overflow.
    int32_t add_batch(IPLProbeBatch batch);
    /// Removes the batch and hands its ref to the caller.
    IPLProbeBatch take_batch(int32_t handle);
    /// Empties the manager; caller releases each batch.
    void get_all_batches(std::vector<IPLProbeBatch>& out);
    /// Returns a retained ref to an arbitrary batch (map order), or null.
    IPLProbeBatch get_first_batch();
    /// Returns a retained ref the caller must release, or null.
    IPLProbeBatch get_batch(int32_t handle) const;
};

} // namespace godot

#endif // HANDLE_MANAGER_H
