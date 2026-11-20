#ifndef RESONANCE_PROBE_BATCH_REGISTRY_H
#define RESONANCE_PROBE_BATCH_REGISTRY_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <phonon.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "handle_manager.h"
#include "resonance_pathing_batch_lookup_policy.h"
#include "resonance_probe_data.h"

namespace godot {

/// Probe batch dedup by data hash, refcounts and compatibility checks for ResonanceServer.
class ResonanceProbeBatchRegistry {
  public:
    ResonanceProbeBatchRegistry() = default;

    /// Returns the handle or -1. The caller checks compatibility first. probe_bytes must be the blob data_hash
    /// was computed from. The batch joins the simulator on the next apply_simulator_changes; pathing skips it until then.
    int32_t load_batch(IPLContext ctx, Ref<ResonanceProbeData> data, uint64_t data_hash,
                       const uint8_t* probe_bytes, int64_t probe_byte_size);

    /// Drops one reference. The last one leaves the batch visible until apply_simulator_changes removes it,
    /// so a reload of the same data in between keeps the batch.
    void remove_batch(int32_t handle);

    bool has_pending_simulator_changes() const;
    std::vector<int32_t> pending_removals() const;
    /// Caller holds the simulation mutex.
    void apply_simulator_changes(IPLSimulator sim);

    /// Caller holds the simulation mutex.
    void clear_batches(IPLSimulator sim);

    /// Handles whose baked reflection type does not match the runtime (read-only snapshot).
    std::vector<int32_t> list_incompatible_handles(int reflection_type, bool pathing_enabled) const;

    struct PathingBatchResolve {
        IPLProbeBatch batch = nullptr;
        resonance::PathingBatchLookup lookup{};
    };

    /// The returned batch is retained; the caller must iplProbeBatchRelease it.
    PathingBatchResolve resolve_pathing_batch(int32_t preferred_handle) const;

    /// The returned batch is retained; the caller must iplProbeBatchRelease it.
    IPLProbeBatch get_pathing_batch(int32_t preferred_handle) const;

    /// Same gate as a preferred hit in get_pathing_batch.
    bool handle_has_pathing(int32_t handle) const;

    /// Null for an unknown handle.
    Ref<ResonanceProbeData> get_probe_data_for_handle(int32_t handle) const;

    void for_each_probe_data(const std::function<void(int32_t handle, const Ref<ResonanceProbeData>& data)>& fn) const;

    bool is_compatible(int32_t handle, int reflection_type, bool pathing_enabled) const;

    ProbeBatchManager& get_manager() { return probe_batch_manager_; }
    const ProbeBatchManager& get_manager() const { return probe_batch_manager_; }

    void get_all_batches_for_shutdown(std::vector<IPLProbeBatch>& out);

    bool has_any_batches() const;
    uint64_t simulator_generation() const { return simulator_generation_.load(std::memory_order_acquire); }

  private:
    static bool is_reflection_type_compatible(int baked_type, int reflection_type);
    int32_t _reuse_loaded_batch_locked(uint64_t data_hash);
    IPLProbeBatch _forget_handle_locked(int32_t handle);
    void _clear_bookkeeping_locked();

    ProbeBatchManager probe_batch_manager_;
    std::unordered_map<uint64_t, int32_t> hash_to_handle_;
    std::unordered_map<int32_t, uint64_t> handle_to_hash_;
    std::unordered_map<int32_t, int> refcount_;
    std::unordered_map<int32_t, bool> handle_has_pathing_;
    std::unordered_map<int32_t, int> handle_baked_refl_;
    std::unordered_map<int32_t, Ref<ResonanceProbeData>> handle_to_probe_data_;
    std::unordered_set<int32_t> pending_adds_;
    /// Handles whose refcount reached zero; the worker removes them from the simulator.
    std::unordered_set<int32_t> pending_removes_;
    std::unordered_set<int32_t> in_simulator_;
    mutable std::mutex mutex_;
    /// Lock-free for audio fetch gate; kept in sync with handle_to_hash_ under mutex_.
    std::atomic<bool> has_any_batches_{false};
    std::atomic<uint64_t> simulator_generation_{0};
};

} // namespace godot

#endif // RESONANCE_PROBE_BATCH_REGISTRY_H
