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

/// Shared release helpers for Steam Audio handles (forward declarations; phonon.h required)
inline void _handle_release_source(IPLSource* p) {
    if (p && *p)
        iplSourceRelease(p);
}
inline void _handle_release_batch(IPLProbeBatch* p) {
    if (p && *p)
        iplProbeBatchRelease(p);
}

/// Generic handle-based resource manager. Reduces duplication between SourceManager and ProbeBatchManager.
/// T: stored resource type (IPLSource, IPLProbeBatch).
/// ReleaseFunc: void (*)(T*) - called with address of stored resource when releasing. Must handle null.
template <typename T, void (*ReleaseFunc)(T*)>
class HandleManagerBase {
  public:
    HandleManagerBase() = default;
    HandleManagerBase(const HandleManagerBase&) = delete;
    HandleManagerBase(HandleManagerBase&&) = delete;
    HandleManagerBase& operator=(const HandleManagerBase&) = delete;
    HandleManagerBase& operator=(HandleManagerBase&&) = delete;

  public:
    /// Lock-free snapshot of the number of currently held items. Maintained alongside [code]items[/code]
    /// so debug/perf monitors can read without blocking the main/audio thread on [code]mutex[/code].
    int32_t size_approx() const { return item_count_.load(std::memory_order_relaxed); }

  protected:
    int32_t next_handle = 0;
    std::priority_queue<int32_t, std::vector<int32_t>, std::greater<int32_t>> free_handles;
    std::unordered_map<int32_t, T> items;
    mutable std::mutex mutex;
    /// Mirror of [code]items.size()[/code], updated under [code]mutex[/code] so external readers can observe
    /// it without locking. Kept separate from the map because unordered_map::size() is not atomic.
    std::atomic<int32_t> item_count_{0};

    /// Returns a new handle, or -1 on overflow (no free handles and next_handle would exceed INT32_MAX).
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

    void release_all() {
        for (auto& pair : items) {
            if (pair.second)
                ReleaseFunc(&pair.second);
        }
        items.clear();
        item_count_.store(0, std::memory_order_relaxed);
    }

    /// Transfers all items to out and clears the manager. Caller must release each item.
    /// Must be called with mutex held (by derived class).
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
    /// Retains source (takes ownership of a ref). Caller may release their ref after. Returns handle or -1 for null.
    int32_t add_source(IPLSource source);
    /// Removes the source from the map and releases the manager retain.
    /// When [param recycle] is false, the handle stays reserved until [method recycle_source_handle]
    /// (needed so create cannot reuse a handle while worker lifecycle queues still reference it).
    void remove_source(int32_t handle, bool recycle = true);
    /// Returns a deferred-recycle handle to the free list. No-op if not pending.
    void recycle_source_handle(int32_t handle);
    IPLSource get_source(int32_t handle);
    /// True if handle is currently assigned (thread-safe).
    bool has_handle(int32_t handle) const;
    void get_all_handles(std::vector<int32_t>& out);

  private:
    /// Handles removed with recycle=false; must not be alloc'd again until recycle_source_handle.
    std::unordered_set<int32_t> deferred_recycle_;
};

class ProbeBatchManager : public HandleManagerBase<IPLProbeBatch, _handle_release_batch> {
  public:
    ProbeBatchManager();
    ~ProbeBatchManager();
    /// Takes ownership of batch; caller must not retain. Returns handle or -1 for null.
    int32_t add_batch(IPLProbeBatch batch);
    IPLProbeBatch take_batch(int32_t handle);
    /// Transfers all batches to out and clears the manager. Caller must release each batch.
    void get_all_batches(std::vector<IPLProbeBatch>& out);
    /// Returns a retained probe batch from an arbitrary map entry (unordered_map iteration order is undefined).
    IPLProbeBatch get_first_batch();
    IPLProbeBatch get_batch(int32_t handle) const;
};

} // namespace godot

#endif // HANDLE_MANAGER_H
