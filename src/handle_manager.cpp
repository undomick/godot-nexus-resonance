#include "handle_manager.h"
#include "resonance_log.h"

namespace godot {

SourceManager::SourceManager() {}
SourceManager::~SourceManager() {
    std::lock_guard<std::mutex> lock(mutex);
    release_all();
}

int32_t SourceManager::add_source(IPLSource source) {
    if (!source)
        return -1;
    IPLSource retained_source = iplSourceRetain(source);
    std::lock_guard<std::mutex> lock(mutex);
    int32_t handle = alloc_handle();
    if (handle < 0) {
        iplSourceRelease(&retained_source);
        ResonanceLog::error("SourceManager: Handle overflow (max sources exceeded).");
        return -1;
    }
    items[handle] = retained_source;
    item_count_.store((int32_t)items.size(), std::memory_order_relaxed);
    return handle;
}

void SourceManager::remove_source(int32_t handle, bool recycle) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = items.find(handle);
    if (it == items.end())
        return;
    _handle_release_source(&(it->second));
    items.erase(it);
    item_count_.store((int32_t)items.size(), std::memory_order_relaxed);
    if (recycle)
        recycle_handle(handle);
    else
        deferred_recycle_.insert(handle);
}

void SourceManager::recycle_source_handle(int32_t handle) {
    if (handle < 0)
        return;
    std::lock_guard<std::mutex> lock(mutex);
    if (deferred_recycle_.erase(handle) == 0)
        return;
    recycle_handle(handle);
}

IPLSource SourceManager::get_source(int32_t handle) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = items.find(handle);
    if (it != items.end()) {
        return iplSourceRetain(it->second);
    }
    return nullptr;
}

bool SourceManager::has_handle(int32_t handle) const {
    if (handle < 0)
        return false;
    std::lock_guard<std::mutex> lock(mutex);
    return items.find(handle) != items.end();
}

void SourceManager::get_all_handles(std::vector<int32_t>& out) {
    std::lock_guard<std::mutex> lock(mutex);
    out.clear();
    for (const auto& pair : items) {
        out.push_back(pair.first);
    }
}

ProbeBatchManager::ProbeBatchManager() {}
ProbeBatchManager::~ProbeBatchManager() {
    std::lock_guard<std::mutex> lock(mutex);
    release_all();
}

int32_t ProbeBatchManager::add_batch(IPLProbeBatch batch) {
    if (!batch)
        return -1;
    std::lock_guard<std::mutex> lock(mutex);
    int32_t handle = alloc_handle();
    if (handle < 0) {
        iplProbeBatchRelease(&batch);
        ResonanceLog::error("ProbeBatchManager: Handle overflow (max probe batches exceeded).");
        return -1;
    }
    items[handle] = batch;
    item_count_.store((int32_t)items.size(), std::memory_order_relaxed);
    return handle;
}

IPLProbeBatch ProbeBatchManager::take_batch(int32_t handle) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = items.find(handle);
    if (it != items.end()) {
        IPLProbeBatch batch = it->second;
        items.erase(it);
        item_count_.store((int32_t)items.size(), std::memory_order_relaxed);
        recycle_handle(handle);
        return batch;
    }
    return nullptr;
}

void ProbeBatchManager::get_all_batches(std::vector<IPLProbeBatch>& out) {
    std::lock_guard<std::mutex> lock(mutex);
    clear_and_reset(out);
}

IPLProbeBatch ProbeBatchManager::get_first_batch() {
    std::lock_guard<std::mutex> lock(mutex);
    if (items.empty())
        return nullptr;
    return iplProbeBatchRetain(items.begin()->second);
}

IPLProbeBatch ProbeBatchManager::get_batch(int32_t handle) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = items.find(handle);
    if (it != items.end())
        return iplProbeBatchRetain(it->second);
    return nullptr;
}

} // namespace godot
