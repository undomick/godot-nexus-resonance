#include "resonance_probe_batch_registry.h"
#include "resonance_constants.h"
#include "resonance_log.h"
#include "resonance_probe_batch_pathing_policy.h"
#include "resonance_probe_data.h"
#include "resonance_reflection_type_policy.h"
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

bool ResonanceProbeBatchRegistry::is_reflection_type_compatible(int baked_type, int reflection_type) {
    return resonance::baked_reflection_type_matches_runtime(baked_type, reflection_type);
}

int32_t ResonanceProbeBatchRegistry::load_batch(IPLContext ctx, Ref<ResonanceProbeData> data, uint64_t data_hash,
                                                const uint8_t* probe_bytes, int64_t probe_byte_size) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int32_t existing = _reuse_loaded_batch_locked(data_hash);
        if (existing >= 0)
            return existing;
    }

    if (probe_byte_size <= 0 || probe_bytes == nullptr) {
        ResonanceLog::error("ResonanceProbeBatchRegistry: Probe data is empty.");
        return -1;
    }
    IPLSerializedObjectSettings sSettings{};
    sSettings.data = const_cast<IPLbyte*>(probe_bytes);
    sSettings.size = static_cast<IPLsize>(probe_byte_size);
    IPLSerializedObject sObj = nullptr;
    if (iplSerializedObjectCreate(ctx, &sSettings, &sObj) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceProbeBatchRegistry: iplSerializedObjectCreate failed.");
        return -1;
    }

    IPLProbeBatch batch = nullptr;
    IPLerror load_status = iplProbeBatchLoad(ctx, sObj, &batch);
    iplSerializedObjectRelease(&sObj);

    if (load_status != IPL_STATUS_SUCCESS) {
        UtilityFunctions::push_warning("Nexus Resonance: iplProbeBatchLoad failed (status=%d). Re-bake the probes.", (int)load_status);
        return -1;
    }

    iplProbeBatchCommit(batch);

    std::lock_guard<std::mutex> lock(mutex_);
    const int32_t existing = _reuse_loaded_batch_locked(data_hash);
    if (existing >= 0) {
        iplProbeBatchRelease(&batch);
        return existing;
    }
    if (static_cast<int>(handle_to_hash_.size()) >= resonance::kMaxProbeBatches) {
        iplProbeBatchRelease(&batch);
        ResonanceLog::error("ResonanceProbeBatchRegistry: Max probe batches exceeded (" + String::num(resonance::kMaxProbeBatches) + "). Remove unused probe volumes or increase limit.");
        return -1;
    }
    int32_t handle = probe_batch_manager_.add_batch(batch);
    if (handle < 0) {
        ResonanceLog::error("ResonanceProbeBatchRegistry: add_batch failed (handle overflow).");
        return -1;
    }
    hash_to_handle_[data_hash] = handle;
    handle_to_hash_[handle] = data_hash;
    refcount_[handle] = 1;
    const bool ipl_has_pathing = resonance::probe_batch_has_pathing_layer(batch);
    handle_has_pathing_[handle] = resonance::resolve_handle_has_pathing_from_load(ipl_has_pathing, data->get_pathing_params_hash());
    if (resonance::should_warn_pathing_hash_without_layer(ipl_has_pathing, data->get_pathing_params_hash())) {
        UtilityFunctions::push_warning(
            "Nexus Resonance: probe data pathing_params_hash is set but the loaded batch has no PATHING/DYNAMIC layer. "
            "Re-bake pathing or verify the probe resource on disk.");
    }
    handle_baked_refl_[handle] = data->get_baked_reflection_type();
    handle_to_probe_data_[handle] = data;
    has_any_batches_.store(true, std::memory_order_release);
    pending_adds_.insert(handle);
    if (Engine::get_singleton() && Engine::get_singleton()->is_editor_hint()) {
        UtilityFunctions::print_rich("[color=cyan]Nexus Resonance:[/color] Probe batch loaded successfully. Reverb simulation active.");
    }
    return handle;
}

int32_t ResonanceProbeBatchRegistry::_reuse_loaded_batch_locked(uint64_t data_hash) {
    auto it = hash_to_handle_.find(data_hash);
    if (it == hash_to_handle_.end())
        return -1;
    const int32_t existing = it->second;
    refcount_[existing]++;
    pending_removes_.erase(existing);
    if (Engine::get_singleton() && Engine::get_singleton()->is_editor_hint()) {
        UtilityFunctions::print_rich("[color=cyan]Nexus Resonance:[/color] Reusing existing probe batch (duplicate data). Refcount: " + String::num(refcount_[existing]));
    }
    return existing;
}

void ResonanceProbeBatchRegistry::remove_batch(int32_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto ref_it = refcount_.find(handle);
    if (ref_it == refcount_.end() || ref_it->second <= 0)
        return;
    ref_it->second--;
    if (ref_it->second == 0)
        pending_removes_.insert(handle);
}

bool ResonanceProbeBatchRegistry::has_pending_simulator_changes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !pending_adds_.empty() || !pending_removes_.empty();
}

std::vector<int32_t> ResonanceProbeBatchRegistry::pending_removals() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<int32_t>(pending_removes_.begin(), pending_removes_.end());
}

void ResonanceProbeBatchRegistry::apply_simulator_changes(IPLSimulator sim) {
    std::vector<IPLProbeBatch> to_remove;
    std::vector<IPLProbeBatch> to_release;
    std::vector<IPLProbeBatch> to_add;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int32_t handle : pending_removes_) {
            pending_adds_.erase(handle);
            IPLProbeBatch batch = _forget_handle_locked(handle);
            if (!batch)
                continue;
            if (in_simulator_.erase(handle) > 0)
                to_remove.push_back(batch);
            else
                to_release.push_back(batch);
        }
        pending_removes_.clear();
        for (int32_t handle : pending_adds_) {
            IPLProbeBatch batch = probe_batch_manager_.get_batch(handle);
            if (!batch)
                continue;
            to_add.push_back(batch);
            in_simulator_.insert(handle);
        }
        pending_adds_.clear();
        has_any_batches_.store(!handle_to_hash_.empty(), std::memory_order_release);
    }
    if (sim) {
        for (IPLProbeBatch batch : to_remove)
            iplSimulatorRemoveProbeBatch(sim, batch);
        for (IPLProbeBatch batch : to_add)
            iplSimulatorAddProbeBatch(sim, batch);
        if (!to_remove.empty() || !to_add.empty()) {
            iplSimulatorCommit(sim);
            simulator_generation_.fetch_add(1, std::memory_order_release);
        }
    }
    for (std::vector<IPLProbeBatch>* batches : {&to_remove, &to_release, &to_add}) {
        for (IPLProbeBatch& batch : *batches)
            iplProbeBatchRelease(&batch);
    }
}

IPLProbeBatch ResonanceProbeBatchRegistry::_forget_handle_locked(int32_t handle) {
    refcount_.erase(handle);
    handle_has_pathing_.erase(handle);
    handle_baked_refl_.erase(handle);
    handle_to_probe_data_.erase(handle);
    auto hash_it = handle_to_hash_.find(handle);
    if (hash_it != handle_to_hash_.end()) {
        hash_to_handle_.erase(hash_it->second);
        handle_to_hash_.erase(hash_it);
    }
    return probe_batch_manager_.take_batch(handle);
}

void ResonanceProbeBatchRegistry::clear_batches(IPLSimulator sim) {
    std::vector<IPLProbeBatch> batches;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        _clear_bookkeeping_locked();
        probe_batch_manager_.get_all_batches(batches);
    }
    if (batches.empty())
        return;
    if (!sim) {
        for (auto& batch : batches) {
            if (batch)
                iplProbeBatchRelease(&batch);
        }
        return;
    }
    for (auto& batch : batches) {
        if (batch) {
            iplSimulatorRemoveProbeBatch(sim, batch);
            iplProbeBatchRelease(&batch);
        }
    }
    iplSimulatorCommit(sim);
    simulator_generation_.fetch_add(1, std::memory_order_release);
}

void ResonanceProbeBatchRegistry::_clear_bookkeeping_locked() {
    hash_to_handle_.clear();
    handle_to_hash_.clear();
    refcount_.clear();
    handle_has_pathing_.clear();
    handle_baked_refl_.clear();
    handle_to_probe_data_.clear();
    pending_adds_.clear();
    pending_removes_.clear();
    in_simulator_.clear();
    has_any_batches_.store(false, std::memory_order_release);
}

std::vector<int32_t> ResonanceProbeBatchRegistry::list_incompatible_handles(int reflection_type, bool pathing_enabled) const {
    std::vector<int32_t> to_remove;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& kv : handle_to_hash_) {
        const int32_t handle = kv.first;
        if (!is_compatible(handle, reflection_type, pathing_enabled))
            to_remove.push_back(handle);
    }
    return to_remove;
}

ResonanceProbeBatchRegistry::PathingBatchResolve ResonanceProbeBatchRegistry::resolve_pathing_batch(int32_t preferred_handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    int32_t sole_pathing_handle = -1;
    size_t pathing_batch_count = 0;
    for (const int32_t handle : in_simulator_) {
        if (!handle_has_pathing_.count(handle) || !handle_has_pathing_.at(handle))
            continue;
        pathing_batch_count++;
        sole_pathing_handle = handle;
    }

    const bool preferred_exists = preferred_handle >= 0 && in_simulator_.count(preferred_handle) > 0;
    const bool preferred_has_pathing =
        preferred_exists && handle_has_pathing_.count(preferred_handle) && handle_has_pathing_.at(preferred_handle);
    const resonance::PathingBatchLookup lookup = resonance::resolve_pathing_batch_lookup(
        preferred_handle, preferred_exists, preferred_has_pathing, sole_pathing_handle, pathing_batch_count);

    PathingBatchResolve out;
    out.lookup = lookup;
    if (lookup.handle >= 0)
        out.batch = probe_batch_manager_.get_batch(lookup.handle);
    return out;
}

IPLProbeBatch ResonanceProbeBatchRegistry::get_pathing_batch(int32_t preferred_handle) const {
    return resolve_pathing_batch(preferred_handle).batch;
}

bool ResonanceProbeBatchRegistry::handle_has_pathing(int32_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (handle < 0 || !handle_to_hash_.count(handle))
        return false;
    auto it = handle_has_pathing_.find(handle);
    return it != handle_has_pathing_.end() && it->second;
}

bool ResonanceProbeBatchRegistry::is_compatible(int32_t handle, int reflection_type, bool pathing_enabled) const {
    int baked_type = -1;
    bool has_pathing = false;
    auto refl_it = handle_baked_refl_.find(handle);
    if (refl_it != handle_baked_refl_.end())
        baked_type = refl_it->second;
    auto path_it = handle_has_pathing_.find(handle);
    if (path_it != handle_has_pathing_.end())
        has_pathing = path_it->second;

    if (baked_type >= resonance::kBakedReflectionConvolution && baked_type <= resonance::kBakedReflectionHybrid) {
        if (!is_reflection_type_compatible(baked_type, reflection_type))
            return false;
    }
    // A pathing mismatch keeps the batch for reflections; pathing is skipped through has_pathing.
    (void)pathing_enabled;
    (void)has_pathing;
    return true;
}

bool ResonanceProbeBatchRegistry::has_any_batches() const {
    return has_any_batches_.load(std::memory_order_acquire);
}

void ResonanceProbeBatchRegistry::get_all_batches_for_shutdown(std::vector<IPLProbeBatch>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    _clear_bookkeeping_locked();
    probe_batch_manager_.get_all_batches(out);
}

Ref<ResonanceProbeData> ResonanceProbeBatchRegistry::get_probe_data_for_handle(int32_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = handle_to_probe_data_.find(handle);
    if (it == handle_to_probe_data_.end())
        return Ref<ResonanceProbeData>();
    return it->second;
}

void ResonanceProbeBatchRegistry::for_each_probe_data(const std::function<void(int32_t handle, const Ref<ResonanceProbeData>& data)>& fn) const {
    if (!fn)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& kv : handle_to_probe_data_) {
        if (kv.second.is_valid())
            fn(kv.first, kv.second);
    }
}

} // namespace godot
