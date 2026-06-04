#include "resonance_probe_batch_registry.h"
#include "resonance_constants.h"
#include "resonance_log.h"
#include "resonance_probe_data.h"
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

bool ResonanceProbeBatchRegistry::is_reflection_type_compatible(int baked_type, int reflection_type) {
    bool uses_conv = (reflection_type == resonance::kReflectionConvolution ||
                      reflection_type == resonance::kReflectionHybrid || reflection_type == resonance::kReflectionTan);
    bool uses_param = (reflection_type == resonance::kReflectionParametric ||
                       reflection_type == resonance::kReflectionHybrid);
    return (baked_type == resonance::kBakedReflectionHybrid) ||
           (baked_type == resonance::kBakedReflectionConvolution && uses_conv) ||
           (baked_type == resonance::kBakedReflectionParametric && uses_param);
}

int32_t ResonanceProbeBatchRegistry::load_batch(IPLContext ctx, IPLSimulator sim, std::mutex* sim_mutex,
                                                Ref<ResonanceProbeData> data, uint64_t data_hash,
                                                const uint8_t* probe_bytes, int64_t probe_byte_size) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = hash_to_handle_.find(data_hash);
        if (it != hash_to_handle_.end()) {
            int32_t existing = it->second;
            refcount_[existing]++;
            if (Engine::get_singleton() && Engine::get_singleton()->is_editor_hint()) {
                UtilityFunctions::print_rich("[color=cyan]Nexus Resonance:[/color] Reusing existing probe batch (duplicate data). Refcount: " + String::num(refcount_[existing]));
            }
            return existing;
        }
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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = hash_to_handle_.find(data_hash);
        if (it != hash_to_handle_.end()) {
            iplProbeBatchRelease(&batch);
            int32_t existing = it->second;
            refcount_[existing]++;
            if (Engine::get_singleton() && Engine::get_singleton()->is_editor_hint()) {
                UtilityFunctions::print_rich("[color=cyan]Nexus Resonance:[/color] Reusing existing probe batch (concurrent load). Refcount: " + String::num(refcount_[existing]));
            }
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
        handle_has_pathing_[handle] = (data->get_pathing_params_hash() > 0);
        handle_baked_refl_[handle] = data->get_baked_reflection_type();
        handle_to_probe_data_[handle] = data;
        {
            if (sim_mutex) {
                std::lock_guard<std::mutex> sim_lock(*sim_mutex);
                iplSimulatorAddProbeBatch(sim, batch);
                iplSimulatorCommit(sim);
            } else {
                iplSimulatorAddProbeBatch(sim, batch);
                iplSimulatorCommit(sim);
            }
        }
        if (Engine::get_singleton() && Engine::get_singleton()->is_editor_hint()) {
            UtilityFunctions::print_rich("[color=cyan]Nexus Resonance:[/color] Probe batch loaded successfully. Reverb simulation active.");
        }
        return handle;
    }
}

void ResonanceProbeBatchRegistry::remove_batch(int32_t handle, IPLSimulator sim, std::mutex* sim_mutex) {
    IPLProbeBatch batch = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto ref_it = refcount_.find(handle);
        if (ref_it == refcount_.end())
            return;
        ref_it->second--;
        if (ref_it->second > 0)
            return;
        refcount_.erase(ref_it);
        handle_has_pathing_.erase(handle);
        handle_baked_refl_.erase(handle);
        handle_to_probe_data_.erase(handle);
        auto hash_it = handle_to_hash_.find(handle);
        if (hash_it != handle_to_hash_.end()) {
            hash_to_handle_.erase(hash_it->second);
            handle_to_hash_.erase(hash_it);
        }
        batch = probe_batch_manager_.take_batch(handle);
    }
    if (batch && sim) {
        if (sim_mutex) {
            std::lock_guard<std::mutex> lock(*sim_mutex);
            iplSimulatorRemoveProbeBatch(sim, batch);
            iplSimulatorCommit(sim);
            iplProbeBatchRelease(&batch);
        } else {
            iplSimulatorRemoveProbeBatch(sim, batch);
            iplSimulatorCommit(sim);
            iplProbeBatchRelease(&batch);
        }
    }
}

void ResonanceProbeBatchRegistry::clear_batches(IPLSimulator sim, std::mutex* sim_mutex) {
    std::vector<IPLProbeBatch> batches;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hash_to_handle_.clear();
        handle_to_hash_.clear();
        refcount_.clear();
        handle_has_pathing_.clear();
        handle_baked_refl_.clear();
        handle_to_probe_data_.clear();
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
    if (sim_mutex) {
        std::lock_guard<std::mutex> lock(*sim_mutex);
        for (auto& batch : batches) {
            if (batch) {
                iplSimulatorRemoveProbeBatch(sim, batch);
                iplProbeBatchRelease(&batch);
            }
        }
        iplSimulatorCommit(sim);
    } else {
        for (auto& batch : batches) {
            if (batch)
                iplProbeBatchRelease(&batch);
        }
    }
}

int ResonanceProbeBatchRegistry::revalidate_with_config(IPLSimulator sim, std::mutex* sim_mutex,
                                                        int reflection_type, bool pathing_enabled) {
    std::vector<int32_t> to_remove;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& kv : handle_to_hash_) {
            int32_t handle = kv.first;
            if (!is_compatible(handle, reflection_type, pathing_enabled))
                to_remove.push_back(handle);
        }
    }
    for (int32_t h : to_remove)
        remove_batch(h, sim, sim_mutex);
    return (int)to_remove.size();
}

IPLProbeBatch ResonanceProbeBatchRegistry::get_pathing_batch(int32_t preferred_handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (preferred_handle >= 0 && handle_to_hash_.count(preferred_handle) &&
        handle_has_pathing_.count(preferred_handle) && handle_has_pathing_.at(preferred_handle)) {
        return probe_batch_manager_.get_batch(preferred_handle);
    }
    for (const auto& kv : handle_to_hash_) {
        int32_t handle = kv.first;
        if (handle_has_pathing_.count(handle) && handle_has_pathing_.at(handle)) {
            return probe_batch_manager_.get_batch(handle);
        }
    }
    return nullptr;
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
    if (pathing_enabled && !has_pathing)
        return false;
    return true;
}

bool ResonanceProbeBatchRegistry::has_any_batches() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !handle_to_hash_.empty();
}

void ResonanceProbeBatchRegistry::get_all_batches_for_shutdown(std::vector<IPLProbeBatch>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    hash_to_handle_.clear();
    handle_to_hash_.clear();
    refcount_.clear();
    handle_has_pathing_.clear();
    handle_baked_refl_.clear();
    handle_to_probe_data_.clear();
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
