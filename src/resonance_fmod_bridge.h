#ifndef RESONANCE_FMOD_BRIDGE_H
#define RESONANCE_FMOD_BRIDGE_H

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <phonon.h>

namespace godot {

// Steam Audio FMOD plugin loader; wires ResonanceServer context into phonon_fmod.
class ResonanceFMODBridge : public Object {
    GDCLASS(ResonanceFMODBridge, Object)

  public:
    ResonanceFMODBridge() = default;
    ~ResonanceFMODBridge();

    ResonanceFMODBridge(const ResonanceFMODBridge&) = delete;
    ResonanceFMODBridge(ResonanceFMODBridge&&) = delete;

    bool init_bridge();
    void shutdown_bridge();
    /// After ResonanceServer reinit: terminate + re-init with the new IPLContext without unloading the plugin.
    bool rebind_after_reinit();
    bool is_bridge_loaded() const { return plugin_handle_ != nullptr; }
    bool is_bridge_initialized() const { return initialized_; }

    int32_t add_fmod_source(int32_t resonance_source_handle);
    void remove_fmod_source(int32_t fmod_handle);

  private:
#if defined(_WIN32) || defined(_WIN64)
    void* plugin_handle_ = nullptr; // HMODULE
#else
    void* plugin_handle_ = nullptr; // void* from dlopen
#endif

    bool initialized_ = false;

    // Function pointers (loaded from plugin)
    void (*fn_iplFMODInitialize_)(IPLContext) = nullptr;
    void (*fn_iplFMODTerminate_)() = nullptr;
    void (*fn_iplFMODSetHRTF_)(IPLHRTF) = nullptr;
    void (*fn_iplFMODSetSimulationSettings_)(IPLSimulationSettings) = nullptr;
    void (*fn_iplFMODSetReverbSource_)(IPLSource) = nullptr;
    int32_t (*fn_iplFMODAddSource_)(IPLSource) = nullptr;
    void (*fn_iplFMODRemoveSource_)(int32_t) = nullptr;

    bool load_plugin();
    void unload_plugin();
    void* get_proc(const char* name);

    static void _bind_methods();
};

} // namespace godot

#endif // RESONANCE_FMOD_BRIDGE_H
