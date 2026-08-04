#ifndef RESONANCE_PROBE_BATCH_REGISTRY_H
#define RESONANCE_PROBE_BATCH_REGISTRY_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <phonon.h>
#include <unordered_map>
#include <vector>

#include "handle_manager.h"
#include "resonance_probe_data.h"

namespace godot {

/// Manages probe batch hash mapping, refcount, and compatibility checks.
/// ResonanceServer delegates probe batch load/remove/clear/revalidate to this class.
class ResonanceProbeBatchRegistry {
  public:
    ResonanceProbeBatchRegistry() = default;

    /// Load probe batch. Returns handle or -1. Caller must validate compatibility before calling.
    /// [probe_bytes, probe_byte_size] must reference the same blob used to compute [data_hash] (no copy here).
    /// When [sim_mutex] is non-null it is acquired before the registry mutex (same order as the simulation worker).
    int32_t load_batch(IPLContext ctx, IPLSimulator sim, std::mutex* sim_mutex,
                       Ref<ResonanceProbeData> data, uint64_t data_hash,
                       const uint8_t* probe_bytes, int64_t probe_byte_size);

    void remove_batch(int32_t handle, IPLSimulator sim, std::mutex* sim_mutex);

    void clear_batches(IPLSimulator sim, std::mutex* sim_mutex);

    int revalidate_with_config(IPLSimulator sim, std::mutex* sim_mutex,
                               int reflection_type, bool pathing_enabled);

    /// Returns pathing batch for preferred_handle if valid, else first with pathing.
    /// IMPORTANT: Return value is retained. Caller MUST call iplProbeBatchRelease when done; failure to release causes leaks.
    IPLProbeBatch get_pathing_batch(int32_t preferred_handle) const;

    /// True when [handle] is loaded and has pathing baked (same gate as get_pathing_batch preferred hit).
    bool handle_has_pathing(int32_t handle) const;

    /// Probe data resource associated with a loaded batch handle (-1 if unknown).
    Ref<ResonanceProbeData> get_probe_data_for_handle(int32_t handle) const;

    void for_each_probe_data(const std::function<void(int32_t handle, const Ref<ResonanceProbeData>& data)>& fn) const;

    bool is_compatible(int32_t handle, int reflection_type, bool pathing_enabled) const;

    ProbeBatchManager& get_manager() { return probe_batch_manager_; }
    const ProbeBatchManager& get_manager() const { return probe_batch_manager_; }

    void get_all_batches_for_shutdown(std::vector<IPLProbeBatch>& out);

    bool has_any_batches() const;

  private:
    static bool is_reflection_type_compatible(int baked_type, int reflection_type);

    ProbeBatchManager probe_batch_manager_;
    std::unordered_map<uint64_t, int32_t> hash_to_handle_;
    std::unordered_map<int32_t, uint64_t> handle_to_hash_;
    std::unordered_map<int32_t, int> refcount_;
    std::unordered_map<int32_t, bool> handle_has_pathing_;
    std::unordered_map<int32_t, int> handle_baked_refl_;
    std::unordered_map<int32_t, Ref<ResonanceProbeData>> handle_to_probe_data_;
    mutable std::mutex mutex_;
    /// Lock-free for audio fetch gate; kept in sync with handle_to_hash_ under mutex_.
    std::atomic<bool> has_any_batches_{false};
};

} // namespace godot

#endif // RESONANCE_PROBE_BATCH_REGISTRY_H
