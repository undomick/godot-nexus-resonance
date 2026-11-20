#ifndef RESONANCE_SOURCE_SIMULATION_TABLE_H
#define RESONANCE_SOURCE_SIMULATION_TABLE_H

#include "resonance_reflections_pass_gate.h"
#include "resonance_source_handle_policy.h"
#include "resonance_source_update_policy.h"
#include "resonance_spatial_output_gate.h"

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace resonance {

enum class SourceDestroyOutcome {
    kRecycleNow,
    // The source never reached the simulation: release it without a remove, then recycle.
    kReleaseAndRecycleNow,
    // The worker removes it from the simulation and recycles the handle afterwards.
    kRemoveQueued,
};

/// Proof that the caller holds the simulation mutex, or runs a direct-only pass next to a reflections pass
/// that holds it. Only a held lock can create one.
class SimulationAccess {
  public:
    explicit SimulationAccess(const std::lock_guard<std::mutex>&) {}

    explicit SimulationAccess(const std::unique_lock<std::mutex>& simulation_lock) {
        assert(simulation_lock.owns_lock());
        (void)simulation_lock;
    }

    explicit SimulationAccess(const DirectPassLock& pass_lock)
        : direct_only_(pass_lock.mode == DirectPassMode::DirectOnly) {
        assert(pass_lock.mode != DirectPassMode::None);
    }

    /// Direct-only: a reflections pass is running, so only direct inputs may change.
    bool direct_only() const { return direct_only_; }

  private:
    bool direct_only_ = false;
};

enum class SourcePass {
    kDirect,
    kReflections,
    kRealtimeReflections,
    kPathing,
};

enum class SourceUpdateDecision {
    kApply,
    kSkipUnchanged,
    // Unchanged because a direct-only pass put it back in the queue, not because the caller repeated it.
    kSkipRequeued,
};

/// Simulation state of every source from creation to removal, indexed by source handle. Params is the
/// update payload and SourceRef the simulation object the server adds and removes; the table never
/// calls into Steam Audio itself.
template <std::size_t N, class Params, class SourceRef>
class SourceSimulationTable {
  public:
    struct PendingAdd {
        int32_t handle = -1;
        Params initial{};
    };

    struct LifecycleDrain {
        std::vector<PendingAdd> adds;
        /// Retained by the caller of source_destroyed; the drainer releases them after the remove.
        std::vector<SourceRef> removes;
        /// Recycle each one only after its remove was committed.
        std::vector<int32_t> removed_handles;

        bool empty() const { return adds.empty() && removes.empty(); }
    };

    struct QueuedUpdate {
        int32_t handle = -1;
        Params params{};
        uint32_t handle_generation = 0;
    };

    /// Input slots are indexed by simulation flag bits: 1 direct, 2 reflections, 4 pathing.
    static constexpr std::size_t kInputSlots = 8;
    static constexpr std::size_t kDirectInputSlot = 1;

    static bool fits(int32_t handle) { return handle >= 0 && static_cast<std::size_t>(handle) < N; }

    /// clear_fetch_state runs while audio fetch is already blocked for the handle and before the worker can
    /// attach the source, so it may reset per-handle state that a recycled handle must not inherit.
    /// Until the worker applies the initial params, the source counts as taking part in the direct and
    /// reflections passes, and in pathing only if takes_part_in_pathing.
    template <class ClearFetchState>
    void source_created(int32_t handle, const Params& initial, bool takes_part_in_pathing,
                        ClearFetchState&& clear_fetch_state) {
        if (fits(handle))
            attach_pending_[static_cast<std::size_t>(handle)].store(1, std::memory_order_release);
        clear_fetch_state();
        if (fits(handle)) {
            const auto h = static_cast<std::size_t>(handle);
            reflections_pending_[h].store(true, std::memory_order_release);
            store_outputs(h, true, true, false, takes_part_in_pathing);
            direct_readiness_.reset_source(handle);
        }
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        pending_adds_.push_back({handle, initial});
    }

    /// retained is the caller's retain of the source, or empty when the source is already gone. On
    /// kRemoveQueued the table keeps it for the drain; otherwise the caller still owns it.
    SourceDestroyOutcome source_destroyed(int32_t handle, SourceRef retained) {
        if (fits(handle)) {
            attach_pending_[static_cast<std::size_t>(handle)].store(0, std::memory_order_release);
            reflections_pending_[static_cast<std::size_t>(handle)].store(false, std::memory_order_release);
            clear_pass_outputs(handle);
        }
        drop_queued_update(handle);
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        const bool cancelled_pending_add = cancel_pending_add(handle);
        if (!retained)
            return SourceDestroyOutcome::kRecycleNow;
        if (cancelled_pending_add)
            return SourceDestroyOutcome::kReleaseAndRecycleNow;
        pending_removes_.push_back(retained);
        pending_removed_handles_.push_back(handle);
        return SourceDestroyOutcome::kRemoveQueued;
    }

    /// The worker applies a pending add's initial params after attaching it, so an update that arrives first
    /// must replace them instead of being overwritten by them.
    bool replace_pending_add_initial(int32_t handle, const Params& params) {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        for (PendingAdd& add : pending_adds_) {
            if (add.handle == handle) {
                add.initial = params;
                return true;
            }
        }
        return false;
    }

    bool has_pending_lifecycle() const {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        return !pending_adds_.empty() || !pending_removes_.empty();
    }

    LifecycleDrain take_lifecycle() {
        LifecycleDrain drain;
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        drain.adds.swap(pending_adds_);
        drain.removes.swap(pending_removes_);
        drain.removed_handles.swap(pending_removed_handles_);
        return drain;
    }

    void attached(int32_t handle) {
        if (fits(handle))
            attach_pending_[static_cast<std::size_t>(handle)].store(0, std::memory_order_release);
    }

    void clear_attach_pending() {
        for (std::atomic<uint8_t>& pending : attach_pending_)
            pending.store(0, std::memory_order_release);
    }

    /// Called once the source has left the simulation for good.
    void clear_pass_outputs(int32_t handle) {
        if (fits(handle))
            store_outputs(static_cast<std::size_t>(handle), false, false, false, false);
    }

    void set_direct_output(int32_t handle, bool takes_part) {
        if (fits(handle))
            outputs_[static_cast<std::size_t>(handle)][kDirectOutput].store(takes_part ? 1 : 0, std::memory_order_release);
    }

    /// The reflections pass reads these flags, so they change only together with the wet inputs, never in a
    /// direct-only pass. A source leaving the reflections pass stops waiting for its first reflections result.
    void set_wet_outputs(SimulationAccess access, int32_t handle, bool reflections, bool realtime_reflections,
                         bool pathing) {
        assert(!access.direct_only());
        (void)access;
        if (!fits(handle))
            return;
        const auto h = static_cast<std::size_t>(handle);
        outputs_[h][kReflectionsOutput].store(reflections ? 1 : 0, std::memory_order_release);
        if (!reflections)
            reflections_pending_[h].store(false, std::memory_order_release);
        outputs_[h][kRealtimeReflectionsOutput].store(realtime_reflections ? 1 : 0, std::memory_order_release);
        outputs_[h][kPathingOutput].store(pathing ? 1 : 0, std::memory_order_release);
    }

    void clear_pathing_output(int32_t handle) {
        if (fits(handle))
            outputs_[static_cast<std::size_t>(handle)][kPathingOutput].store(0, std::memory_order_release);
    }

    /// Lock-free. Handles outside the table take part in nothing.
    bool takes_part_in(SourcePass pass, int32_t handle) const {
        if (!fits(handle))
            return false;
        return outputs_[static_cast<std::size_t>(handle)][output_index(pass)].load(std::memory_order_acquire) != 0;
    }

    /// Lock-free; true until the source's first reflections result can be fetched.
    bool is_reflections_pending(int32_t handle) const {
        return fits(handle) && reflections_pending_[static_cast<std::size_t>(handle)].load(std::memory_order_acquire);
    }

    void clear_reflections_pending(int32_t handle) {
        if (fits(handle))
            reflections_pending_[static_cast<std::size_t>(handle)].store(false, std::memory_order_release);
    }

    void clear_all_reflections_pending() {
        for (std::atomic<bool>& pending : reflections_pending_)
            pending.store(false, std::memory_order_release);
    }

    /// After a reflections pass every attached source in it has a result to fetch; a source still waiting
    /// for attach stays pending.
    void reflections_pass_ran(int32_t handle) {
        if (takes_part_in(SourcePass::kReflections, handle) && !is_attach_pending(handle))
            clear_reflections_pending(handle);
    }

    /// A reloaded scene makes every direct result stale. Direct passes read readiness_generation before
    /// they start, so a result from a pass that began before the reload never counts.
    void scene_reloaded() { direct_readiness_.arm(); }

    uint32_t readiness_generation() const { return direct_readiness_.current_generation(); }

    void direct_published(int32_t handle, uint32_t readiness_generation) {
        if (fits(handle))
            direct_readiness_.mark_published(handle, readiness_generation);
    }

    bool is_direct_ready(int32_t handle) const { return fits(handle) && direct_readiness_.is_direct_ready(handle); }

    /// Lock-free; true while the source exists but is not yet part of the simulation.
    bool is_attach_pending(int32_t handle) const {
        return fits(handle) && attach_pending_[static_cast<std::size_t>(handle)].load(std::memory_order_acquire) != 0;
    }

    /// Clients compare it with the epoch they stored next to a handle; a mismatch means the handle belongs
    /// to an earlier context.
    uint32_t lifecycle_epoch() const { return lifecycle_epoch_.load(std::memory_order_acquire); }

    void bump_lifecycle_epoch() {
        lifecycle_epoch_.store(next_source_lifecycle_epoch(lifecycle_epoch_.load(std::memory_order_relaxed)),
                               std::memory_order_release);
    }

    void enqueue_update(int32_t handle, const Params& params) {
        std::lock_guard<std::mutex> lock(update_mutex_);
        queued_updates_[handle] = params;
    }

    bool has_queued_updates() const {
        std::lock_guard<std::mutex> lock(update_mutex_);
        return !queued_updates_.empty();
    }

    std::vector<QueuedUpdate> take_updates() {
        std::vector<QueuedUpdate> batch;
        std::lock_guard<std::mutex> lock(update_mutex_);
        batch.reserve(queued_updates_.size());
        for (const auto& [handle, params] : queued_updates_)
            batch.push_back({handle, params, handle_generation(handle)});
        queued_updates_.clear();
        return batch;
    }

    /// Latest wins: keeps an update enqueued while the batch was out, and drops updates of a handle that was
    /// destroyed (and maybe reused) since the batch was taken.
    void requeue_not_superseded(const std::vector<QueuedUpdate>& batch) {
        std::lock_guard<std::mutex> lock(update_mutex_);
        for (const QueuedUpdate& update : batch) {
            const bool superseded = queued_updates_.find(update.handle) != queued_updates_.end();
            if (!superseded && update.handle_generation == handle_generation(update.handle))
                queued_updates_[update.handle] = update.params;
        }
    }

    void clear_queued_updates() {
        std::lock_guard<std::mutex> lock(update_mutex_);
        queued_updates_.clear();
    }

    /// Decides whether an update must reach the simulation. On kApply the params become the source's
    /// snapshot; the caller then sets the direct inputs and reports it with direct_inputs_set.
    /// attenuation_curve_dirty must stay true until the inputs carried the new curve: the curve lives outside
    /// Params, so equal params alone do not mean nothing changed.
    SourceUpdateDecision begin_update(SimulationAccess access, int32_t handle, const Params& params,
                                      bool attenuation_curve_dirty) {
        Snapshot& snap = snapshots_[handle];
        const bool wet_inputs_owed = !access.direct_only() && snap.wet_inputs_deferred;
        const bool invalidated = snapshot_invalidation_.consume(handle, snap.invalidation_generation);
        const bool unchanged = snap.valid && !invalidated && !attenuation_curve_dirty &&
                               source_update_params_equal(snap.params, params);
        if (!wet_inputs_owed && unchanged) {
            if (access.direct_only() && snap.wet_inputs_deferred)
                return SourceUpdateDecision::kSkipRequeued;
            return SourceUpdateDecision::kSkipUnchanged;
        }
        snap.params = params;
        snap.valid = true;
        return SourceUpdateDecision::kApply;
    }

    /// A direct-only pass leaves the reflections and pathing inputs owed until the next full pass.
    void direct_inputs_set(SimulationAccess access, int32_t handle) {
        snapshots_[handle].wet_inputs_deferred = access.direct_only();
    }

    /// The params the simulation last received for the source, or null before its first update.
    const Params* snapshot(SimulationAccess, int32_t handle) const {
        const auto it = snapshots_.find(handle);
        if (it == snapshots_.end() || !it->second.valid)
            return nullptr;
        return &it->second.params;
    }

    void forget_snapshot(SimulationAccess, int32_t handle) { snapshots_.erase(handle); }

    void clear_snapshots(SimulationAccess) { snapshots_.clear(); }

    /// Any thread: the next update of the source runs even if its params did not change.
    void invalidate_snapshot(int32_t handle) { snapshot_invalidation_.invalidate(handle); }

    void invalidate_all_snapshots() { snapshot_invalidation_.invalidate_all(); }

    /// Returns whether the inputs differ from the last ones set on that slot; only then does the inputs
    /// generation move, so the pass-skip checks see real changes only.
    bool record_inputs(SimulationAccess access, int32_t handle, std::size_t slot, uint64_t hash) {
        assert(!access.direct_only() || slot == kDirectInputSlot);
        if (fits(handle)) {
            uint64_t& last = inputs_hash_[static_cast<std::size_t>(handle)][slot % kInputSlots];
            if (last == hash)
                return false;
            last = hash;
        }
        ++inputs_generation_;
        return true;
    }

    void forget_inputs(SimulationAccess, int32_t handle) {
        if (fits(handle))
            inputs_hash_[static_cast<std::size_t>(handle)] = {};
        ++inputs_generation_;
    }

    void bump_inputs_generation(SimulationAccess) { ++inputs_generation_; }

    uint64_t inputs_generation(SimulationAccess) const { return inputs_generation_; }

  private:
    static constexpr std::size_t kDirectOutput = 0;
    static constexpr std::size_t kReflectionsOutput = 1;
    static constexpr std::size_t kRealtimeReflectionsOutput = 2;
    static constexpr std::size_t kPathingOutput = 3;
    static constexpr std::size_t kOutputs = 4;

    static std::size_t output_index(SourcePass pass) {
        switch (pass) {
        case SourcePass::kDirect:
            return kDirectOutput;
        case SourcePass::kReflections:
            return kReflectionsOutput;
        case SourcePass::kRealtimeReflections:
            return kRealtimeReflectionsOutput;
        case SourcePass::kPathing:
            return kPathingOutput;
        }
        return kDirectOutput;
    }

    void store_outputs(std::size_t h, bool direct, bool reflections, bool realtime_reflections, bool pathing) {
        outputs_[h][kReflectionsOutput].store(reflections ? 1 : 0, std::memory_order_release);
        outputs_[h][kRealtimeReflectionsOutput].store(realtime_reflections ? 1 : 0, std::memory_order_release);
        outputs_[h][kPathingOutput].store(pathing ? 1 : 0, std::memory_order_release);
        outputs_[h][kDirectOutput].store(direct ? 1 : 0, std::memory_order_release);
    }

    struct Snapshot {
        Params params{};
        bool valid = false;
        bool wet_inputs_deferred = false;
        uint32_t invalidation_generation = 0;
    };

    uint32_t handle_generation(int32_t handle) const {
        return fits(handle) ? handle_generation_[static_cast<std::size_t>(handle)] : 0;
    }

    void drop_queued_update(int32_t handle) {
        std::lock_guard<std::mutex> lock(update_mutex_);
        queued_updates_.erase(handle);
        if (fits(handle))
            handle_generation_[static_cast<std::size_t>(handle)]++;
    }

    bool cancel_pending_add(int32_t handle) {
        bool cancelled = false;
        for (auto it = pending_adds_.begin(); it != pending_adds_.end();) {
            if (it->handle == handle) {
                cancelled = true;
                it = pending_adds_.erase(it);
            } else {
                ++it;
            }
        }
        return cancelled;
    }

    mutable std::mutex lifecycle_mutex_;
    std::vector<PendingAdd> pending_adds_;
    std::vector<SourceRef> pending_removes_;
    std::vector<int32_t> pending_removed_handles_;
    std::array<std::atomic<uint8_t>, N> attach_pending_{};
    std::atomic<uint32_t> lifecycle_epoch_{1};

    mutable std::mutex update_mutex_;
    std::unordered_map<int32_t, Params> queued_updates_;
    std::array<uint32_t, N> handle_generation_{};

    // Guarded by the simulation mutex, which the caller proves with a SimulationAccess.
    std::unordered_map<int32_t, Snapshot> snapshots_;
    std::array<std::array<uint64_t, kInputSlots>, N> inputs_hash_{};
    uint64_t inputs_generation_ = 0;

    SourceSnapshotInvalidation<N> snapshot_invalidation_;

    std::array<std::array<std::atomic<uint8_t>, kOutputs>, N> outputs_{};
    std::array<std::atomic<bool>, N> reflections_pending_{};
    SourceDirectReadiness<static_cast<int>(N)> direct_readiness_;
};

} // namespace resonance

#endif // RESONANCE_SOURCE_SIMULATION_TABLE_H
