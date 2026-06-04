#ifndef RESONANCE_STEAM_AUDIO_CONTEXT_H
#define RESONANCE_STEAM_AUDIO_CONTEXT_H

#include "resonance_constants.h"
#include "resonance_sofa_asset.h"
#include <atomic>
#include <phonon.h>

namespace godot {

/// Configuration subset required for Steam Audio context and device initialization.
struct ResonanceSteamAudioContextConfig {
    int sample_rate = 48000;
    int frame_size = resonance::kGodotDefaultFrameSize;
    int ambisonic_order = 1;
    float max_reverb_duration = 2.0f;
    int reflection_type = resonance::kReflectionConvolution; // May be modified (e.g. TAN fallback)
    int scene_type = 0;                                      // 0=Default, 1=Embree, 2=Radeon, 3=Custom (Godot Physics)
    int opencl_device_type = 0;                              // 0=GPU, 1=CPU, 2=Any
    int opencl_device_index = 0;
    bool context_validation = false;
    int context_simd_level = -1;
    float hrtf_volume_db = 0.0f;
    /// 0=None, 1=RMS - embedded default HRTF only.
    int hrtf_normalization_type = 0;
    int max_simulation_sources = resonance::kMaxSimulationSources;
    Ref<ResonanceSOFAAsset> hrtf_sofa_asset;
};

/// Manages Steam Audio context and all device handles (Context, Embree, OpenCL, Radeon Rays, TAN, HRTF).
/// ResonanceServer delegates device creation and shutdown to this class.
///
/// Lifetime: If init() returns false, native handles may be partially allocated (e.g. IPL context without a valid HRTF).
/// Call shutdown() or destroy the object before calling init() again; do not retry init() without cleaning up first.
class ResonanceSteamAudioContext {
  public:
    ResonanceSteamAudioContext() = default;
    ~ResonanceSteamAudioContext();

    ResonanceSteamAudioContext(const ResonanceSteamAudioContext&) = delete;
    ResonanceSteamAudioContext& operator=(const ResonanceSteamAudioContext&) = delete;
    ResonanceSteamAudioContext(ResonanceSteamAudioContext&&) = delete;
    ResonanceSteamAudioContext& operator=(ResonanceSteamAudioContext&&) = delete;

    /// Initialize context and devices. reflection_type in config may be modified (TAN fallback).
    /// config.scene_type is set to the effective tracer index (0–3) after device setup.
    /// Returns true on success. On false, call shutdown() or destroy this object (see class lifetime note).
    bool init(ResonanceSteamAudioContextConfig& config);

    void shutdown();

    IPLContext get_context() const { return context_; }
    IPLEmbreeDevice get_embree_device() const { return embree_device_; }
    IPLOpenCLDevice get_opencl_device() const { return opencl_device_; }
    IPLRadeonRaysDevice get_radeon_rays_device() const { return radeon_rays_device_; }
    IPLTrueAudioNextDevice get_tan_device() const { return tan_device_; }
    /// Returns HRTF for the audio thread. Double-buffer: init/main writes [1]; this call may sync [1] to [0].
    /// Despite const, updates internal handles and atomics; use only with the server's init/audio thread split.
    IPLHRTF get_hrtf() const;
    IPLSceneType get_scene_type() const { return scene_type_; }

  private:
    IPLContext context_ = nullptr;
    IPLEmbreeDevice embree_device_ = nullptr;
    IPLOpenCLDeviceList opencl_device_list_ = nullptr;
    IPLOpenCLDevice opencl_device_ = nullptr;
    IPLRadeonRaysDevice radeon_rays_device_ = nullptr;
    IPLTrueAudioNextDevice tan_device_ = nullptr;
    mutable IPLHRTF hrtf_[2] = {nullptr, nullptr};
    mutable std::atomic<bool> new_hrtf_written_{false};
    IPLSceneType scene_type_ = IPL_SCENETYPE_DEFAULT;
};

} // namespace godot

#endif // RESONANCE_STEAM_AUDIO_CONTEXT_H
